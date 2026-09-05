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
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "usb_midi_host.h"

namespace {

constexpr int32_t kControlMask = 31;
constexpr int32_t kMaxAudio = 2047;
constexpr int32_t kMinAudio = -2048;
constexpr int32_t kTuneSpreadDeadband = 96;
constexpr int32_t kTransientMax = 4095;
constexpr int32_t kPitchUnitsPerOctave = 4096;
constexpr int32_t kPitchInputCountsPerVolt = 313; // 341 * 11/12 from hardware test.
constexpr int32_t kBaseMidiNote = 36;      // C2, matching fr330hfr33/Cosmik.
constexpr int32_t kCenterMidiNote = 60;    // C4 at Main noon with no CV.
constexpr int32_t kCenterPitchUnits =
    ((kCenterMidiNote - kBaseMidiNote) * kPitchUnitsPerOctave) / 12;
constexpr int32_t kMinPitchUnits = -2 * kPitchUnitsPerOctave;
constexpr int32_t kMaxPitchUnits = 8 * kPitchUnitsPerOctave;
constexpr uint32_t kC2PhaseIncrement = 5852465u;
constexpr uint8_t kMidiCcModWheel = 1;
constexpr uint8_t kMidiCcVolume = 7;
constexpr uint8_t kMidiCcSpread = 20;
constexpr uint8_t kMidiCcBrightnessAlt = 21;
constexpr uint8_t kMidiCcBrightness = 74;
constexpr int32_t kMidiActivitySamples = 24000;

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
        static constexpr uint32_t kClusteredPhase[kSawCount] = {
            0xFFF00000u, 0x00080000u, 0x00180000u, 0x00000000u,
            0xFFE80000u, 0x00280000u, 0xFFD80000u
        };
        for (int i = 0; i < kSawCount; ++i) {
            phase_[i] = kClusteredPhase[i];
            inc_[i] = pitch_units_to_inc(kCenterPitchUnits);
        }
    }

    bool ShouldBootUsbHost()
    {
        return USBPowerState() == USBPowerState_t::DFP;
    }

    void SetUsbHostMode(bool host_mode)
    {
        usb_host_mode_ = host_mode;
    }

    void SetUsbMidiConnected(bool connected)
    {
        usb_midi_connected_ = connected;
        midi_activity_countdown_ = kMidiActivitySamples;
    }

    void ProcessUsbMidiByte(uint8_t byte)
    {
        midi_activity_countdown_ = kMidiActivitySamples;
        process_midi_voice_byte(byte);
    }

    uint8_t MidiInChannel() const
    {
        return midi_in_channel_;
    }

    void ProcessUsbMidiVoiceByte(uint8_t byte)
    {
        ProcessUsbMidiByte(byte);
    }

    void SendPendingUsbMidiOutput()
    {
    }

    void RefreshControls()
    {
        update_controls();
    }

    virtual void __not_in_flash_func(ProcessSample)()
    {
        apply_pending_midi_events();
        if (midi_activity_countdown_ > 0) {
            --midi_activity_countdown_;
        }

        const bool lead_gate = Connected(Input::Pulse1) && PulseIn1();
        const bool accent_gate = Connected(Input::Pulse2) && PulseIn2();
        const bool midi_gate = midi_note_active_;
        const bool gate = drone_mode_ || accent_held_ || lead_gate || midi_gate;
        if (lead_gate && !last_lead_gate_) {
            sync_supersaw_phases();
            transient_env_ = kTransientMax;
        }
        last_lead_gate_ = lead_gate;
        if (accent_gate && !last_accent_gate_) {
            transient_env_ = kTransientMax;
        }
        last_accent_gate_ = accent_gate;

        if (gate) {
            envelope_ += attack_step_;
            if (envelope_ > 4095) envelope_ = 4095;
        } else {
            envelope_ -= release_step_;
            if (envelope_ < 0) envelope_ = 0;
        }

        if (!gate && envelope_ == 0) {
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
        if (transient_env_ > 0) {
            mixed += (mono * transient_env_) >> 13;
            transient_env_ -= transient_decay_;
            if (transient_env_ < 0) transient_env_ = 0;
        }
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

    int32_t pitch_units_ = kCenterPitchUnits;
    int32_t filter_state_ = 0;
    int32_t side_state_ = 0;
    int32_t filter_coeff_ = 900;
    int32_t stereo_width_ = 0;
    int32_t level_ = 2400;
    int32_t envelope_ = 0;
    int32_t transient_env_ = 0;
    int32_t transient_decay_ = 24;
    int32_t attack_step_ = 16;
    int32_t release_step_ = 4;
    bool drone_mode_ = false;
    bool stereo_mode_ = false;
    bool accent_held_ = false;
    bool tune_mode_ = true;
    bool last_lead_gate_ = false;
    bool last_accent_gate_ = false;

    uint8_t midi_in_channel_ = 0;
    uint8_t midi_running_status_ = 0;
    uint8_t midi_data_[2] = {};
    uint8_t midi_data_count_ = 0;
    uint8_t midi_note_ = kCenterMidiNote;
    uint8_t midi_velocity_ = 100;
    volatile uint8_t pending_midi_note_ = kCenterMidiNote;
    volatile uint8_t pending_midi_velocity_ = 100;
    volatile uint8_t pending_midi_note_off_ = kCenterMidiNote;
    volatile bool pending_midi_note_on_ = false;
    volatile bool pending_midi_note_off_event_ = false;
    volatile bool midi_note_active_ = false;
    volatile int32_t midi_pitch_bend_ = 0;
    volatile int32_t midi_spread_cc_ = 0;
    volatile int32_t midi_brightness_cc_ = 0;
    volatile int32_t midi_volume_cc_ = 4095;
    volatile bool midi_spread_cc_active_ = false;
    volatile bool midi_brightness_cc_active_ = false;
    volatile bool midi_volume_cc_active_ = false;
    volatile bool usb_host_mode_ = false;
    volatile bool usb_midi_connected_ = false;
    volatile int32_t midi_activity_countdown_ = 0;

    void update_controls()
    {
        const int32_t main = KnobVal(Knob::Main);
        const int32_t x = midi_spread_cc_active_ ? midi_spread_cc_ : KnobVal(Knob::X);
        const int32_t y = midi_brightness_cc_active_ ? midi_brightness_cc_ : KnobVal(Knob::Y);

        const Switch sw = SwitchVal();
        drone_mode_ = (sw == Switch::Up);
        accent_held_ = (sw == Switch::Down);
        stereo_mode_ = true;

        // Main is now a playable transpose/tune control around middle C rather
        // than a huge sweep.
        //
        // Start from the 1V/oct convention used by fr330hfr33 and CosmikC1zzl3:
        //   4096 pitch units = 1 octave
        //   341 CV input counts = 1 volt
        //
        // The measured JP8K hardware test played 11 semitones for a one-octave
        // keyboard span, so this build tightens the input scale to 313 counts
        // per volt while keeping the same pitch-unit math.
        //
        // The input itself is not calibrated, so this is the repo's best-known
        // raw-CV convention rather than lab-grade pitch tracking.
        int32_t units = midi_note_active_ ? midi_note_pitch_units(midi_note_) : kCenterPitchUnits;
        units += (((main - 2048) * (2 * kPitchUnitsPerOctave)) >> 12);
        units += (CVIn1() * kPitchUnitsPerOctave) / kPitchInputCountsPerVolt;
        if (midi_note_active_) {
            units += (midi_pitch_bend_ * kPitchUnitsPerOctave) / (8192 * 6);
        }
        units = clamp_int(units, kMinPitchUnits, kMaxPitchUnits);
        pitch_units_ = units;

        const uint32_t base_inc = pitch_units_to_inc(units);

        int32_t spread = x + (x >> 2);
        if (x > 2048) {
            spread += (x - 2048) >> 1;
        }
        if (Connected(Input::CV2)) {
            spread += CVIn2();
        }
        spread = clamp_int(spread, 0, 4095);
        tune_mode_ = (spread <= kTuneSpreadDeadband);

        // Detune is deliberately asymmetric: the centre voice stays stable,
        // while the outer saws fan out more quickly for that animated JP feel.
        const int32_t detune = tune_mode_ ? 0 : ((spread * spread) >> 13) + (spread >> 2);
        constexpr int32_t ratios[kSawCount] = {-28, -17, -9, 0, 10, 19, 31};
        for (int i = 0; i < kSawCount; ++i) {
            int32_t offset = static_cast<int32_t>(
                ((static_cast<int64_t>(base_inc) * detune * ratios[i]) >> 22));
            int32_t detuned = static_cast<int32_t>(base_inc) + offset;
            if (detuned < 1) detuned = 1;
            inc_[i] = static_cast<uint32_t>(detuned);
        }

        // Y is a simple brightness control. At low values it rounds the stack
        // into a warm pad; high values leave the saw edge bright for external
        // filtering in the Workshop System.
        filter_coeff_ = 220 + ((y * y) >> 12);
        if (y > 2048) {
            filter_coeff_ += (y - 2048) >> 1;
        }
        if (filter_coeff_ > 4095) filter_coeff_ = 4095;

        stereo_width_ = (stereo_mode_ && !tune_mode_) ? (spread >> 3) : 0;
        level_ = 3600 + ((4095 - (spread >> 1)) >> 3);
        if (midi_volume_cc_active_) {
            const int32_t volume_scale = 1024 + ((midi_volume_cc_ * 3) >> 2);
            level_ = (level_ * volume_scale) >> 12;
        }
        if (midi_note_active_) {
            const int32_t velocity_scale = 2048 + ((int32_t)midi_velocity_ << 4);
            level_ = (level_ * velocity_scale) >> 12;
        }
        const bool pulse2_high = Connected(Input::Pulse2) && PulseIn2();
        if (accent_held_ || pulse2_high) level_ += 420;
        transient_decay_ = (accent_held_ || pulse2_high) ? 16 : 24;
        attack_step_ = drone_mode_ ? 4 : 48;
        release_step_ = drone_mode_ ? 2 : 10;
    }

    int32_t __not_in_flash_func(render_supersaw)()
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
            sum += saw * ((i == 3) ? 12 : 6);
            side += saw * ((i & 1) ? 1 : -1);
        }

        side_state_ = side >> 5;
        return sum >> 5;
    }

    void __not_in_flash_func(sync_supersaw_phases)()
    {
        static constexpr uint32_t kClusteredPhase[kSawCount] = {
            0xFFF00000u, 0x00080000u, 0x00180000u, 0x00000000u,
            0xFFE80000u, 0x00280000u, 0xFFD80000u
        };
        for (int i = 0; i < kSawCount; ++i) {
            phase_[i] = kClusteredPhase[i];
        }
    }

    void process_midi_voice_byte(uint8_t byte)
    {
        if (byte >= 0xF8u) {
            return;
        }

        if (byte & 0x80u) {
            midi_running_status_ = byte;
            midi_data_count_ = 0;
            return;
        }

        const uint8_t type = midi_running_status_ & 0xF0u;
        if (type != 0x80u && type != 0x90u && type != 0xB0u && type != 0xE0u) {
            return;
        }

        midi_data_[midi_data_count_++] = byte & 0x7Fu;
        if (midi_data_count_ < 2u) {
            return;
        }

        midi_data_count_ = 0;
        const uint8_t channel = midi_running_status_ & 0x0Fu;
        if (channel != midi_in_channel_) {
            return;
        }

        if (type == 0x90u && midi_data_[1] > 0) {
            pending_midi_note_ = midi_data_[0];
            pending_midi_velocity_ = midi_data_[1];
            pending_midi_note_on_ = true;
            return;
        }

        if (type == 0x80u || (type == 0x90u && midi_data_[1] == 0)) {
            pending_midi_note_off_ = midi_data_[0];
            pending_midi_note_off_event_ = true;
            return;
        }

        if (type == 0xB0u) {
            const int32_t control = midi_cc_to_control(midi_data_[1]);
            if (midi_data_[0] == kMidiCcModWheel || midi_data_[0] == kMidiCcSpread) {
                midi_spread_cc_ = control;
                midi_spread_cc_active_ = true;
            } else if (midi_data_[0] == kMidiCcBrightness ||
                       midi_data_[0] == kMidiCcBrightnessAlt) {
                midi_brightness_cc_ = control;
                midi_brightness_cc_active_ = true;
            } else if (midi_data_[0] == kMidiCcVolume) {
                midi_volume_cc_ = control;
                midi_volume_cc_active_ = true;
            }
            return;
        }

        if (type == 0xE0u) {
            const int32_t bend = ((int32_t)midi_data_[1] << 7) | midi_data_[0];
            midi_pitch_bend_ = bend - 8192;
        }
    }

    int32_t midi_cc_to_control(uint8_t value) const
    {
        return ((int32_t)value * 4095) / 127;
    }

    int32_t midi_note_pitch_units(uint8_t note) const
    {
        return ((int32_t)note - kBaseMidiNote) * kPitchUnitsPerOctave / 12;
    }

    void apply_pending_midi_events()
    {
        if (pending_midi_note_on_) {
            pending_midi_note_on_ = false;
            midi_note_ = pending_midi_note_;
            midi_velocity_ = pending_midi_velocity_;
            midi_note_active_ = true;
            sync_supersaw_phases();
            transient_env_ = kTransientMax;
        }

        if (pending_midi_note_off_event_) {
            pending_midi_note_off_event_ = false;
            if (pending_midi_note_off_ == midi_note_) {
                midi_note_active_ = false;
            }
        }
    }

    void update_leds(bool gate)
    {
        LedOn(0, drone_mode_);
        LedBrightness(1, stereo_width_);
        LedOn(2, gate || accent_held_);
        LedBrightness(3, filter_coeff_);
        int32_t midi_status = usb_host_mode_ ? 2600 : 700;
        if (!usb_midi_connected_) {
            midi_status = usb_host_mode_ ? 1200 : 250;
        }
        if (midi_activity_countdown_ > 0) {
            midi_status = 4095;
        }
        LedBrightness(4, midi_status);
        LedBrightness(5, midi_note_active_ ? 4095 : envelope_);
    }
};

JP8K card;
static volatile uint8_t host_midi_device_address = 0;

extern "C" void tuh_midi_mount_cb(
    uint8_t dev_addr,
    uint8_t in_ep,
    uint8_t out_ep,
    uint8_t num_cables_rx,
    uint16_t num_cables_tx)
{
    (void)in_ep;
    (void)out_ep;
    (void)num_cables_rx;
    (void)num_cables_tx;

    if (host_midi_device_address == 0) {
        host_midi_device_address = dev_addr;
    }
    card.SetUsbMidiConnected(true);
}

extern "C" void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    (void)instance;

    if (dev_addr == host_midi_device_address) {
        host_midi_device_address = 0;
        card.SetUsbMidiConnected(false);
    }
}

extern "C" void tuh_midi_rx_cb(uint8_t dev_addr, uint32_t num_packets)
{
    if (dev_addr != host_midi_device_address || num_packets == 0) {
        return;
    }

    uint8_t cable = 0;
    uint8_t bytes[128];
    while (true) {
        uint32_t count = tuh_midi_stream_read(dev_addr, &cable, bytes, sizeof(bytes));
        if (count == 0) {
            break;
        }

        for (uint32_t i = 0; i < count; ++i) {
            card.ProcessUsbMidiByte(bytes[i]);
        }
    }
}

extern "C" void tuh_midi_tx_cb(uint8_t dev_addr)
{
    (void)dev_addr;
}

extern "C" void tud_mount_cb(void)
{
    card.SetUsbMidiConnected(true);
}

extern "C" void tud_umount_cb(void)
{
    card.SetUsbMidiConnected(false);
}

void usb_midi_worker()
{
    sleep_ms(100);
    const bool host_mode = card.ShouldBootUsbHost();
    card.SetUsbHostMode(host_mode);

    if (host_mode) {
        tuh_init(0);
    } else {
        tud_init(0);
    }

    while (true) {
        if (host_mode) {
            tuh_task();
        } else {
            tud_task();
            card.SendPendingUsbMidiOutput();

            uint8_t bytes[64];
            uint32_t count = tud_midi_stream_read(bytes, sizeof(bytes));
            for (uint32_t i = 0; i < count; ++i) {
                card.ProcessUsbMidiByte(bytes[i]);
            }
        }

        static uint32_t control_divider = 0;
        if ((control_divider++ & kControlMask) == 0) {
            card.RefreshControls();
        }
        sleep_us(50);
    }
}

int main()
{
#ifdef JP8K_OVERCLOCK_240
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(240000, true);
#else
    set_sys_clock_khz(192000, true);
#endif

    multicore_launch_core1(usb_midi_worker);
    card.EnableNormalisationProbe();
    card.Run();
}
