# JP8K

JP8K is a JP-8000 / JP-8080 inspired supersaw voice for the Music Thing
Workshop Computer. It is not a clone. It borrows the famous playable idea - a
wide, animated stack of saw oscillators - and reshapes it into a patchable
Workshop System card.

The first version is designed as a CV/gate Eurorack oscillator with a drone
option. Patch it into the Workshop System filters, Ring Mod, Slopes, Stompbox,
or Mix just as you would patch an analog voice.

## Controls

| Control | Function |
| --- | --- |
| Main | Pitch, with CV In 1 added on top |
| X | Supersaw spread / detune |
| Y | Brightness, from warm pad to raw bright saw |
| Z Up | Drone stereo mode |
| Z Middle | Gated mono mode |
| Z Down | Gated stereo mode |

## Patch Points

| Jack | Function |
| --- | --- |
| CV In 1 | Pitch CV, roughly +/- two octaves |
| CV In 2 | Spread modulation |
| Pulse In 1 | Gate input |
| Audio In 1 | Alternate gate/envelope trigger using a threshold |
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

The card runs the RP2040 at 192 MHz by default. A `JP8K_OVERCLOCK_240` build
define is provided for later testing if the voice grows heavier, but the first
version should not need it.

The pitch CV input is intentionally described as approximate. Workshop Computer
input calibration is not implemented in ComputerCard, so this is designed to be
musically playable inside the Workshop System rather than a precision
laboratory oscillator.

