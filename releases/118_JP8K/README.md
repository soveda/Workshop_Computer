# JP8K

JP8K is a JP-8000 / JP-8080 inspired supersaw voice for the Music Thing
Workshop Computer. It is not a clone. It borrows the famous playable idea - a
wide, animated stack of saw oscillators - and reshapes it into a patchable
Workshop System card.

The first version is designed as a CV/gate Eurorack oscillator with a drone
option. Patch it into the Workshop System filters, Ring Mod, Slopes, Stompbox,
or Mix just as you would patch an analog voice.

## Initial Setup: Supersaw Lead

For the intended first patch, start with a Sandstorm-inspired supersaw lead:

| Control | Starting position |
| --- | --- |
| Z | Middle: gated supersaw lead |
| Main | 12 o'clock, then trim by ear |
| X | 10:30 to 11 o'clock for tight animated detune |
| Y | 2 to 3 o'clock for bright saw edge |

Patch **Pulse In 1** from a fast gate or envelope rhythm, and patch **CV In 1**
from 4 Voltages, a sequencer, or another pitch source. CV In 1 uses the same
4096-internal-units-per-octave pitch math as fr330hfr33 and CosmikC1zzl3, with
the input scale trimmed to about 313 counts per volt after hardware testing
showed 341 counts per volt was about one semitone flat per octave on this card.
Use Main as the tuning trim because the input is not calibrated. Take **Audio
Out 1** as mono, or use both audio outs for the wide version. A filter,
VCA/envelope, and short delay after the card will get much closer to the
classic trance lead shape than the raw oscillator alone.

Patch **Pulse In 2** from an accent rhythm if you want extra bite on selected
steps. It adds a short attack snap and a small level lift while high.

For an instant hands-free version, flip **Z Up**. The same supersaw stack stays
open as a drone, with X setting the width and Y setting the edge.

Hold **Z Down** for a momentary accent. It forces the envelope open and gives
the voice a small level lift; release it to return to the normal gated lead or
drone behavior.

In **Z Middle**, Pulse In 1 is a sustained gate: high opens the lead envelope,
low closes it. When the envelope reaches zero, the audio path is hard-muted.

## Controls

| Control | Function |
| --- | --- |
| Main | Tune / transpose for CV/gate mode; ignored while MIDI notes are active |
| X | Supersaw spread / detune; fully counter-clockwise switches to a single saw for tuning |
| Y | Brightness, from warm pad to raw bright saw |
| Z Up | Drone stereo mode |
| Z Middle | Gated stereo lead mode |
| Z Down | Momentary accent / forced gate |

## Patch Points

| Jack | Function |
| --- | --- |
| CV In 1 | Pitch CV, roughly 1V/oct across +/-6 V |
| CV In 2 | Spread modulation |
| Pulse In 1 | Sustained lead gate |
| Pulse In 2 | Accent / attack snap |
| Audio In 1 | Reserved |
| Audio Out 1 | Left / mono output |
| Audio Out 2 | Right output |
| CV Out 1 | Approximate pitch monitor |
| CV Out 2 | Brightness monitor |
| Pulse Out 1 | Gate monitor |
| Pulse Out 2 | Square pulse from the centre oscillator |

## Notes

The audio engine uses seven integer saw oscillators, fixed-point detune, a
simple tone filter, and an attack/release envelope. Control calculations run
every 32 samples so the 48 kHz audio interrupt stays light.

The supersaw mix is centre-weighted with a bright attack transient and an
audible spread curve, so X around 10:30-11:30 aims at the classic bright trance
lead while higher settings clearly widen and detune the swarm.

USB MIDI works in device mode when patched to a computer, and in host mode when
the Workshop Computer is powering a class-compliant USB MIDI controller. MIDI
uses channel 1. Note on/off messages play the same voice and act like another
sustained gate source; pitch bend is +/-2 semitones. While a MIDI note is held,
the MIDI note supersedes Main and sets the pitch. CV In 1 is still added on top
as patchable pitch modulation.

MIDI CCs:

| CC | Function |
| --- | --- |
| 1 or 20 | Spread, replacing X after the first received CC |
| 21 or 74 | Brightness, replacing Y after the first received CC |
| 7 | Volume, scaling the output level after the first received CC |

LED 4 shows the USB MIDI setting: dimmer for device mode, brighter for host
mode, and full-bright on MIDI activity. LED 5 follows the envelope, but stays
bright while a MIDI note is held.

The card runs the RP2040 at 192 MHz by default. A `JP8K_OVERCLOCK_240` build
define is provided for later testing if the voice grows heavier, but the first
version should not need it.

Each firmware build is kept as a versioned UF2 in `UF2/JP8K_0.1.x.uf2`.
`UF2/jp8k.uf2` is the current latest build for quick flashing.

The pitch CV input is intentionally described as approximate. Workshop Computer
input calibration is not implemented in ComputerCard, so this is designed to be
musically playable inside the Workshop System rather than a precision
laboratory oscillator.

For pitch testing, set **Z Up** for drone or **Z Middle** with a gate patched,
set **Main** to 12 o'clock, set **X** fully counter-clockwise, leave **CV In 2**
unpatched, and set **Y** high enough to hear a bright saw. In that position the
card disables the detuned side oscillators and outputs only the centre saw, so
1V/oct tracking can be checked without the supersaw beating confusing the
tuner. Bring X up after tuning to restore the JP-style spread.
