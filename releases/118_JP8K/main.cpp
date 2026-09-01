// JP8K - JP-8000 / JP-8080 inspired supersaw voice for Workshop Computer.
//
// This is not an attempt to copy Roland firmware. It is a small Workshop-sized
// instrument built around the same playable idea: a wide, detuned stack of saws
// that can behave as a CV/gate oscillator or as a self-running drone.
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
constexpr int32_t kGateEngage = 300;   // Audio-input gate threshold, roughly 0.9 V.
constexpr int32_t kGateRelease = 180;  // Hysteresis keeps slow envelopes stable.
constexpr int32_t kMaxAudio = 2047;
constexpr int32_t kMinAudio = -2048;

// MIDI note 24 (C1) through 96 (C7), converted to 32-bit phase increments at
// 48 kHz. The table avoids powf() and keeps pitch work out of the hot path.
constexpr uint32_t kNoteInc[] = {
    2926232u, 3100235u, 3284585u, 3479896u, 3686822u, 3906052u, 4138318u, 4384395u,
    4645104u, 4921317u, 5213953u, 5523991u, 5852465u, 6200470u, 6569170u, 6959793u,
    7373644u, 7812103u, 8276635u, 8768789u, 9290209u, 9842633u, 10427907u, 11047982u,
    11704930u, 12400941u, 13138339u, 13919586u, 14747287u, 15624207u, 16553270u, 17537579u,
    18580418u, 19685267u, 20855814u, 22095965u, 23409859u, 24801882u, 26276679u, 27839171u,
    29494575u, 31248413u, 33106541u, 35075158u, 37160835u, 39370534u, 41711627u, 44191930u,
    46819719u, 49603764u, 52553357u, 55678342u, 58989149u, 62496826u, 66213081u, 70150316u,
    74321671u, 78741067u, 83423255u, 88383859u, 93639437u, 99207528u, 105106715u, 111356685u,
    117978298u, 124993653u, 132426162u, 140300631u, 148643341u, 157482134u, 166846509u, 176767719u,
    187278874u
};

constexpr int32_t kFirstNote = 24;
constexpr int32_t kNoteCount = static_cast<int32_t>(sizeof(kNoteInc) / sizeof(kNoteInc[0]));

int32_t clamp_int(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

uint32_t pitch_to_inc(int32_t semitone_q8)
{
    int32_t note = semitone_q8 >> 8;
    int32_t frac = semitone_q8 & 255;
    note = clamp_int(note, 0, kNoteCount - 2);

    uint32_t a = kNoteInc[note];
    uint32_t b = kNoteInc[note + 1];
    return a + (((b - a) * static_cast<uint32_t>(frac)) >> 8);
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
            inc_[i] = kNoteInc[36 - kFirstNote];
        }
    }

    virtual void __not_in_flash_func(ProcessSample)()
    {
        if ((control_counter_++ & kControlMask) == 0) {
            update_controls();
        }

        bool gate = drone_mode_;
        if (!gate) {
            if (!gate_latched_ && AudioIn1() > kGateEngage) gate_latched_ = true;
            if (gate_latched_ && AudioIn1() < kGateRelease) gate_latched_ = false;
            gate = PulseIn1() || gate_latched_;
        }

        if (gate) {
            envelope_ += attack_step_;
            if (envelope_ > 4095) envelope_ = 4095;
        } else {
            envelope_ -= release_step_;
            if (envelope_ < 0) envelope_ = 0;
        }

        int32_t mono = render_supersaw();
        filter_state_ += ((mono - filter_state_) * filter_coeff_) >> 12;

        int32_t mixed = filter_state_;
        mixed = (mixed * envelope_) >> 12;
        mixed = (mixed * level_) >> 12;

        int32_t right = mixed;
        int32_t left = mixed;
        if (stereo_mode_) {
            left += (side_state_ * stereo_width_) >> 12;
            right -= (side_state_ * stereo_width_) >> 12;
        }

        AudioOut1(clip_audio(left));
        AudioOut2(clip_audio(right));

        CVOut1Precise((pitch_note_q8_ - (kBaseNoteIndex << 8)) << 4);
        CVOut2Precise((filter_coeff_ - 128) << 5);
        PulseOut1(gate);
        PulseOut2(phase_[0] & 0x80000000u);

        update_leds(gate);
    }

private:
    static constexpr int kSawCount = 7;
    static constexpr int32_t kBaseNoteIndex = 36; // Table index 36 = MIDI note 60, middle C.

    uint32_t phase_[kSawCount];
    uint32_t inc_[kSawCount];

    uint32_t control_counter_ = 0;
    int32_t pitch_note_q8_ = kBaseNoteIndex << 8;
    int32_t filter_state_ = 0;
    int32_t side_state_ = 0;
    int32_t filter_coeff_ = 900;
    int32_t stereo_width_ = 0;
    int32_t level_ = 1800;
    int32_t envelope_ = 0;
    int32_t attack_step_ = 16;
    int32_t release_step_ = 4;
    bool drone_mode_ = false;
    bool stereo_mode_ = false;
    bool gate_latched_ = false;

    void update_controls()
    {
        const int32_t main = KnobVal(Knob::Main);
        const int32_t x = KnobVal(Knob::X);
        const int32_t y = KnobVal(Knob::Y);

        const Switch sw = SwitchVal();
        drone_mode_ = (sw == Switch::Up);
        stereo_mode_ = (sw != Switch::Middle);

        // Main scans five octaves from C1 to C6. CV In 1 adds roughly +/- two
        // octaves, which makes the 4 Voltages section and Slopes useful pitch
        // sources without pretending the uncalibrated input is precision 1V/oct.
        int32_t note_q8 = (kBaseNoteIndex << 8) + (((main - 2048) * (60 << 8)) >> 12);
        note_q8 += (CVIn1() * (24 << 8)) >> 11;
        note_q8 = clamp_int(note_q8, 0, (kNoteCount - 2) << 8);
        pitch_note_q8_ = note_q8;

        const uint32_t base_inc = pitch_to_inc(note_q8);

        int32_t spread = x + ((CVIn2() + 2048) >> 1);
        spread = clamp_int(spread, 0, 4095);

        // Detune is deliberately asymmetric: the centre voice stays stable,
        // while the outer saws fan out more quickly for that animated JP feel.
        const int32_t detune = (spread * spread) >> 12; // 0..4095, finer near zero.
        constexpr int32_t ratios[kSawCount] = {-34, -21, -11, 0, 13, 24, 39};
        for (int i = 0; i < kSawCount; ++i) {
            int32_t offset = (static_cast<int32_t>(base_inc >> 12) * detune * ratios[i]) >> 17;
            inc_[i] = static_cast<uint32_t>(static_cast<int32_t>(base_inc) + offset);
        }

        // Y is a simple brightness control. At low values it rounds the stack
        // into a warm pad; high values leave the saw edge bright for external
        // filtering in the Workshop System.
        filter_coeff_ = 96 + ((y * y) >> 12); // 96..4191
        if (filter_coeff_ > 4095) filter_coeff_ = 4095;

        stereo_width_ = stereo_mode_ ? (spread >> 1) : 0;
        level_ = 1450 + ((4095 - (spread >> 1)) >> 2);
        attack_step_ = drone_mode_ ? 4 : 24;
        release_step_ = drone_mode_ ? 2 : 7;
    }

    int32_t render_supersaw()
    {
        int32_t sum = 0;
        int32_t side = 0;
        for (int i = 0; i < kSawCount; ++i) {
            phase_[i] += inc_[i];
            int32_t saw = static_cast<int32_t>(phase_[i] >> 20) - 2048;
            sum += saw;
            side += saw * ((i & 1) ? 1 : -1);
        }

        side_state_ = side >> 2;
        return sum >> 3;
    }

    void update_leds(bool gate)
    {
        LedOn(0, drone_mode_);
        LedBrightness(1, stereo_width_);
        LedOn(2, gate);
        LedBrightness(3, filter_coeff_);
        LedBrightness(4, pitch_note_q8_ >> 4);
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
