// JP8K - JP-8000 / JP-8080 inspired supersaw voice for Workshop Computer.
//
// This is not an attempt to copy Roland firmware. It is a small Workshop-sized
// instrument built around the same playable idea: a wide, detuned stack of saws
// that can behave as a CV/gate oscillator or as a self-running drone. The docs'
// first patch is a Sandstorm-inspired bright gated lead: Z middle, moderate X
// detune, bright Y, Pulse In 1 for gate, and CV In 1 for the riff. Z down is
// momentary, so it behaves as a held accent/gate gesture rather than a mode.
//
// Audio-rate work stays deliberately plain: seven 32-bit phase accumulators,
// one fixed-point low-pass, one envelope, and integer mixing. Pitch and panel
// controls are refreshed every 32 samples so the interrupt has breathing room.

#include "ComputerCard.h"

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

namespace {

constexpr int32_t kControlMask = 31;
constexpr int32_t kMaxAudio = 2047;
constexpr int32_t kMinAudio = -2048;
constexpr int32_t kTuneSpreadDeadband = 96;
constexpr uint32_t kLeadHoldSamples = 3840; // 80 ms one-shot for gated lead mode.
constexpr int32_t kPitchUnitsPerOctave = 4096;
constexpr int32_t kPitchInputCountsPerVolt = 341;
constexpr int32_t kBaseMidiNote = 36;      // C2, matching fr330hfr33/Cosmik.
constexpr int32_t kCenterMidiNote = 60;    // C4 at Main noon with no CV.
constexpr int32_t kCenterPitchUnits =
    ((kCenterMidiNote - kBaseMidiNote) * kPitchUnitsPerOctave) / 12;
constexpr int32_t kMinPitchUnits = -2 * kPitchUnitsPerOctave;
constexpr int32_t kMaxPitchUnits = 8 * kPitchUnitsPerOctave;
constexpr uint32_t kC2PhaseIncrement = 5852465u;

int32_t clamp_int(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

uint32_t pitch_units_to_inc(int32_t units)
{
    static constexpr uint32_t kSemitoneRatioQ15[13] = {
        32768u, 34716u, 36781u, 38968u, 41285u, 43740u, 46341u,
        49097u, 52016u, 55109u, 58386u, 61858u, 65536u
    };

    units = clamp_int(units, kMinPitchUnits, kMaxPitchUnits);
    int32_t octaves = units / kPitchUnitsPerOctave;
    int32_t remainder = units % kPitchUnitsPerOctave;
    if (remainder < 0) {
        remainder += kPitchUnitsPerOctave;
        --octaves;
    }

    int32_t semitone = (remainder * 12) / kPitchUnitsPerOctave;
    int32_t start = (semitone * kPitchUnitsPerOctave) / 12;
    int32_t end = ((semitone + 1) * kPitchUnitsPerOctave) / 12;
    int32_t fraction = ((remainder - start) << 15) / (end - start);
    uint32_t ratio = kSemitoneRatioQ15[semitone] +
        (uint32_t)((((int32_t)kSemitoneRatioQ15[semitone + 1] -
        (int32_t)kSemitoneRatioQ15[semitone]) * fraction) >> 15);

    uint32_t increment = (uint32_t)(((uint64_t)kC2PhaseIncrement * ratio) >> 15);
    if (octaves >= 0)
        increment <<= octaves;
    else
        increment >>= -octaves;
    return increment;
}

int32_t clip_audio(int32_t v)
{
    return clamp_int(v, kMinAudio, kMaxAudio);
}

} // namespace

class JP8K : public ComputerCard {
public:
    JP8K()
    {
        for (int i = 0; i < kSawCount; ++i) {
            phase_[i] = 0x24924924u * static_cast<uint32_t>(i + 1);
            inc_[i] = pitch_units_to_inc(kCenterPitchUnits);
        }
    }

    virtual void __not_in_flash_func(ProcessSample)()
    {
        if ((control_counter_++ & kControlMask) == 0) {
            update_controls();
        }

        const bool pulse_high = Connected(Input::Pulse1) && PulseIn1();
        const bool lead_trigger = pulse_high && !last_pulse_high_;
        last_pulse_high_ = pulse_high;

        if (lead_trigger && !drone_mode_ && !accent_held_) {
            lead_hold_samples_ = kLeadHoldSamples;
        }

        bool gate = drone_mode_ || accent_held_ || (lead_hold_samples_ > 0);
        if (lead_hold_samples_ > 0) {
            --lead_hold_samples_;
        }

        if (gate) {
            envelope_ += attack_step_;
            if (envelope_ > 4095) envelope_ = 4095;
        } else {
            envelope_ -= release_step_;
            if (envelope_ < 0) envelope_ = 0;
        }

        if (!drone_mode_ && !accent_held_ && envelope_ == 0) {
            filter_state_ = 0;
            side_state_ = 0;
            AudioOut1(0);
            AudioOut2(0);
            CVOut1Precise((pitch_units_ - kCenterPitchUnits) << 3);
            CVOut2Precise((filter_coeff_ - 128) << 5);
            PulseOut1(false);
            PulseOut2(false);
            update_leds(false);
            return;
        }

        int32_t mono = render_supersaw();
        filter_state_ += ((mono - filter_state_) * filter_coeff_) >> 12;

        int32_t mixed = filter_state_;
        mixed = (mixed * envelope_) >> 12;
        mixed = (mixed * level_) >> 11;

        int32_t right = mixed;
        int32_t left = mixed;
        if (stereo_mode_) {
            left += (side_state_ * stereo_width_) >> 12;
            right -= (side_state_ * stereo_width_) >> 12;
        }

        AudioOut1(clip_audio(left));
        AudioOut2(clip_audio(right));

        CVOut1Precise((pitch_units_ - kCenterPitchUnits) << 3);
        CVOut2Precise((filter_coeff_ - 128) << 5);
        PulseOut1(gate);
        PulseOut2(phase_[0] & 0x80000000u);

        update_leds(gate);
    }

private:
    static constexpr int kSawCount = 7;

    uint32_t phase_[kSawCount];
    uint32_t inc_[kSawCount];

    uint32_t control_counter_ = 0;
    int32_t pitch_units_ = kCenterPitchUnits;
    int32_t filter_state_ = 0;
    int32_t side_state_ = 0;
    int32_t filter_coeff_ = 900;
    int32_t stereo_width_ = 0;
    int32_t level_ = 2400;
    int32_t envelope_ = 0;
    int32_t attack_step_ = 16;
    int32_t release_step_ = 4;
    bool drone_mode_ = false;
    bool stereo_mode_ = false;
    bool accent_held_ = false;
    bool tune_mode_ = true;
    bool last_pulse_high_ = false;
    uint32_t lead_hold_samples_ = 0;

    void update_controls()
    {
        const int32_t main = KnobVal(Knob::Main);
        const int32_t x = KnobVal(Knob::X);
        const int32_t y = KnobVal(Knob::Y);

        const Switch sw = SwitchVal();
        drone_mode_ = (sw == Switch::Up);
        accent_held_ = (sw == Switch::Down);
        stereo_mode_ = true;

        // Main is now a playable transpose/tune control around middle C rather
        // than a huge sweep.
        //
        // Match the 1V/oct convention used by fr330hfr33 and CosmikC1zzl3:
        //   4096 pitch units = 1 octave
        //   341 CV input counts = 1 volt
        //
        // The input itself is not calibrated, so this is the repo's best-known
        // raw-CV convention rather than lab-grade pitch tracking.
        int32_t units = kCenterPitchUnits + (((main - 2048) * (2 * kPitchUnitsPerOctave)) >> 12);
        units += (CVIn1() * kPitchUnitsPerOctave) / kPitchInputCountsPerVolt;
        units = clamp_int(units, kMinPitchUnits, kMaxPitchUnits);
        pitch_units_ = units;

        const uint32_t base_inc = pitch_units_to_inc(units);

        int32_t spread = x;
        if (Connected(Input::CV2)) {
            spread += CVIn2();
        }
        spread = clamp_int(spread, 0, 4095);
        tune_mode_ = (spread <= kTuneSpreadDeadband);

        // Detune is deliberately asymmetric: the centre voice stays stable,
        // while the outer saws fan out more quickly for that animated JP feel.
        const int32_t detune = tune_mode_ ? 0 : (spread * spread) >> 12; // 0..4095, finer near zero.
        constexpr int32_t ratios[kSawCount] = {-34, -21, -11, 0, 13, 24, 39};
        for (int i = 0; i < kSawCount; ++i) {
            int32_t offset = static_cast<int32_t>(
                ((static_cast<int64_t>(base_inc) * detune * ratios[i]) >> 29));
            int32_t detuned = static_cast<int32_t>(base_inc) + offset;
            if (detuned < 1) detuned = 1;
            inc_[i] = static_cast<uint32_t>(detuned);
        }

        // Y is a simple brightness control. At low values it rounds the stack
        // into a warm pad; high values leave the saw edge bright for external
        // filtering in the Workshop System.
        filter_coeff_ = 96 + ((y * y) >> 12); // 96..4191
        if (filter_coeff_ > 4095) filter_coeff_ = 4095;

        stereo_width_ = (stereo_mode_ && !tune_mode_) ? (spread >> 1) : 0;
        level_ = 3200 + ((4095 - (spread >> 1)) >> 2);
        if (accent_held_) level_ += 450;
        attack_step_ = drone_mode_ ? 4 : 48;
        release_step_ = drone_mode_ ? 2 : 10;
    }

    int32_t render_supersaw()
    {
        if (tune_mode_) {
            phase_[3] += inc_[3];
            side_state_ = 0;
            return static_cast<int32_t>(phase_[3] >> 20) - 2048;
        }

        int32_t sum = 0;
        int32_t side = 0;
        for (int i = 0; i < kSawCount; ++i) {
            phase_[i] += inc_[i];
            int32_t saw = static_cast<int32_t>(phase_[i] >> 20) - 2048;
            sum += saw * ((i == 3) ? 6 : 3);
            side += saw * ((i & 1) ? 1 : -1);
        }

        side_state_ = side >> 3;
        return sum >> 4;
    }

    void update_leds(bool gate)
    {
        LedOn(0, drone_mode_);
        LedBrightness(1, stereo_width_);
        LedOn(2, gate || accent_held_);
        LedBrightness(3, filter_coeff_);
        LedBrightness(4, clamp_int((pitch_units_ - kMinPitchUnits) >> 2, 0, 4095));
        LedBrightness(5, envelope_);
    }
};

int main()
{
#ifdef JP8K_OVERCLOCK_240
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(240000, true);
#else
    set_sys_clock_khz(192000, true);
#endif

    JP8K card;
    card.EnableNormalisationProbe();
    card.Run();
}
