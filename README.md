# FM-1 Chimera Vocal

> **A port of [CHIMERA](https://github.com/jamesondh/chimera-vocal-synth) to the M-VAVE FM-1.**
> CHIMERA is a physical-modelling voice synthesizer by **Jameson Hodge**, released under the
> [MIT licence](https://github.com/jamesondh/chimera-vocal-synth/blob/main/LICENSE). The sound
> engine here (the airway model, vocal folds, anatomy, gestures and the text-to-speech front end)
> is a port of his work. Credit for how the voice works belongs to CHIMERA. This project only
> adapts it to small hardware.
>
> The firmware is built on **[fm1-base](https://github.com/charlesvestal/fm1-base)** by Charles
> Vestal and **[felucca-bsp](https://github.com/hugelton/felucca-bsp)** by Leo Kuroshita
> (@kurogedelic), Hügelton Instruments, from the FM-1 community firmware work of
> **[Felucca](https://github.com/hugelton/Felucca)** and **[Saadet](https://github.com/keremimo/saadet)**
> by Kerem Kilic (Ellic Studio).
>
> This is an independent project, not affiliated with or endorsed by Jameson Hodge, Charles Vestal,
> Hügelton Instruments, Ellic Studio or M-VAVE. "M-VAVE" and "FM-1" are trademarks of their owners.

## What this is

Firmware for the M-VAVE FM-1 that does one thing: a simulated human voice. You type words on the
FM-1 (or send them from a web page), and the keyboard sings or speaks them, one syllable per key,
the way CHIMERA's **Sing** mode does in the browser.

Every sound comes from a simulated airway (lungs, vocal folds, throat, mouth and nose), not from
samples. That is CHIMERA's design, kept here.

What isn't ported: CHIMERA's web interface, 3D heads, visualisations, the bestiary creatures, the
beatbox sequencer, the video recorder and the browser audio chain. The FM-1 gets its own small
interface.

## Status

Early firmware: a test. The keys sing a held vowel at their pitch, and PLAY speaks one phrase CHIMERA planned,
through CHIMERA's control ported to C. Both of the FM-1's cores are used, and the screen shows how long each one
takes. It is not an instrument yet.

CHIMERA's full airway physics ran on the FM-1 but at about twice what it can afford
([docs/full-physics-on-fm1.md](docs/full-physics-on-fm1.md): the measurements, and what someone could
try to make it fit). The voice now keeps CHIMERA's anatomy, vocal folds, calibrated shapes and noise
sources and drives a waveguide tract (`engine/ch_wave.c`): measured on the FM-1 (0.0.4), the tract
takes 36 % of the second core and the anatomy 82 % of the first (every second control step), with no
dropouts. Against CHIMERA's own renders it is about 5.2-5.5 dB in spectrum (the full physics port: 1.2).

## Layout

| Path | What |
| --- | --- |
| `engine/` | The C port of CHIMERA's voice (MIT, Jameson Hodge's notice kept): `ch_voice.c` the literal audio rate (the reference), `ch_fast.c` the same rearranged for the FM-1, `ch_tract.c` the head model, `ch_wave.c` the FM-1's waveguide tract, `ch_control.c` the control rate (articulators, gestural score, source, contacts), `ch_common.h` what they share |
| `app/` | The firmware, on fm1-base (started from its template): `app.c`, `fm1.toml`, `dsp/voice_male.h` (the airway's layout) and `dsp/phrase_male.h` (one phrase's gestures), both generated from a recording until the airway builder and the text front end are ported |
| `base/` | [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal (MPL-2.0), a git submodule pinned to a tested revision, with felucca-bsp inside |
| `lab/` | The test bench: CHIMERA itself as a Rust library, the recorder, the C replay checks, comparisons, cost estimates |

Build (the JieLi toolchain in `~/.jieli/toolchain`, the AC79 SDK in `~/fw-AC79_AIoT_SDK`, Docker on macOS,
Python with Pillow and fontTools: `python3 -m venv .venv && .venv/bin/pip install Pillow fonttools`):

    .venv/bin/python base/tools/build.py app --host       # the simulator: app/build/host/vocal_host
    app/build/host/vocal_host app/tests/timing.txt app/build/host
    JIELI_TOOLCHAIN=~/.jieli/toolchain .venv/bin/python base/tools/build.py app --sdk ~/fw-AC79_AIoT_SDK
                                                         # the FM-1: app/build/vocal.fwsc, identity FM-1_04200

The package identity is `FM-1_042` for every version (releases add the version: `FM-1_042XXYYZZ`).

The site, with the web installer (Chrome or Edge, the FM-1 on USB):

    .venv/bin/python tools/make_site.py 0.0.1          # the release package, then app/build/site
    python3 -m http.server 8042 --bind 127.0.0.1 --directory app/build/site
                                                         # open http://127.0.0.1:8042/install/

It is fm1-base's site (`base/tools/make_site.py`) with CHIMERA's credit at the top of each page; the browser emulator
joins it once Emscripten is set up.

The lab:

- `cargo run --release -- rates "Text." [male|female|child]` runs CHIMERA itself at the cut-down
  settings and compares it with the original; `-- nodes` lists the airway sizes.
- `cargo run --release -- record "Text." [body]` records CHIMERA's voice at 44.1 kHz with its
  control-rate output and its articulation (`sh patch-chimera.sh` first: it makes the fields the
  recorder reads public, nothing else).
- `sh port/build.sh`, then `out/replay64 out/male.rec` checks the literal port against CHIMERA,
  `out/replay_fm1 out/male.rec out.wav --tract=out/male.art --drive --fast=16` runs the FM-1's version
  (its maths, the head model in C, geometry every 16 samples), `--anat-every=N` the head model every
  N control steps. `--control=out/male.gst` (with `--tract`) runs the C control from CHIMERA's gestures, checks each
  step's articulation, vocal-fold settings and breath against the recording, and renders through the waveguide.
- `cargo run --release -- compare ref.wav other.wav[@shift]` compares level and spectrum frame by frame.
- `tools/pi_cost.py` estimates per-sample instruction counts from the FM-1 compiler's assembly.
- `tools/gen_voice_header.py out/male.rec out/male.art > ../app/dsp/voice_male.h` makes the firmware's airway header,
  `tools/gen_phrase_header.py out/male.gst "Text." > ../app/dsp/phrase_male.h` its phrase.

The reference sources are cloned into `external/` (gitignored): CHIMERA, Felucca, Saadet, Melodee,
fm-1-midi-mania and fm1-base.

## Licence

fm1-base is MPL-2.0 file by file and lets the app choose its own licence. The files ported from
CHIMERA (`engine/`, `app/dsp/voice_male.h`) are MIT and keep Jameson Hodge's copyright notice. The
app's files that started from fm1-base's template stay MPL-2.0. The JieLi AC79 SDK files that every
FM-1 package carries are Apache-2.0 and are read from your SDK at build time.

Unofficial: not affiliated with or endorsed by M-VAVE, JieLi, Jameson Hodge or fm1-base's authors.

## Credits

- **CHIMERA** by Jameson Hodge (MIT): the voice model and everything it sounds like.
  <https://github.com/jamesondh/chimera-vocal-synth>. CHIMERA's own references (Ishizaka, French &
  Flanagan 1975; Hillenbrand et al. 1995; Klatt 1979; and others) are listed in its
  [architecture notes](https://github.com/jamesondh/chimera-vocal-synth/blob/main/docs/architecture.md#references-and-credits).
- **fm1-base** by Charles Vestal (MPL-2.0): the firmware's platform, build, installer and emulator.
- **felucca-bsp** by Leo Kuroshita, Hügelton Instruments (MPL-2.0): the FM-1's hardware layer, from
  Felucca; the second core from Melodee by Kerem Kilic.
- **Felucca** (Leo Kuroshita) and **Saadet** (Kerem Kilic, Ellic Studio): FM-1 firmwares studied for this port.
- **CMU Pronouncing Dictionary**, Carnegie Mellon University (BSD-style), if a pronunciation
  dictionary is included.
