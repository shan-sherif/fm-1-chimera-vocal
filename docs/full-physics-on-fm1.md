# CHIMERA's full airway physics on the FM-1: what was tried

> **A port of [CHIMERA](https://github.com/jamesondh/chimera-vocal-synth) by Jameson Hodge (MIT) to the M-VAVE FM-1.**
> This page records the first attempt: running CHIMERA's own airway simulation on the FM-1. It works and sounds
> right, but it is about **2x too slow** for the FM-1. The firmware moved to a cheaper tract model (see the README).
> Everything needed to pick this up again is here: the code is kept, it is verified against CHIMERA, and the
> measurements are below. If you can make it fit, the voice gets CHIMERA's full sound back.

## What "full physics" means here

CHIMERA simulates the airway as a branched staggered-grid FDTD transmission line (its `airway/network.rs`):
lungs, trachea, glottis, pharynx, mouth, nose, a sinus and the piriform fossae, with yielding walls, Borda-Carnot
jet losses solved as an exact quadratic per branch, Reynolds-number turbulence noise, moving-wall volume sources and
a radiation load at lips and nostrils. Bursts, frication, the voice bar and the vocal folds' reaction to the mouth
all come out of the airflow. Above it, at 689 control steps a second, a 2D head model turns the articulators
(jaw, tongue, lips, velum, larynx...) into the area function (`anatomy/human.rs`): it builds the outline of the
head and casts 49 rays across the tract.

## What was ported (all in `engine/`, MIT, Jameson Hodge's notice kept)

| File | What | Verified against CHIMERA |
| --- | --- | --- |
| `ch_voice.c` | The audio rate, literally: the FDTD network, the parametric glottis, the output stage | `double`: SNR 247 dB (max error 1.5e-11) over a spoken sentence; `float`: 58-74 dB |
| `ch_fast.c` | The same rearranged for the FM-1 (below) | 0.7-1.2 dB mean spectral distance (male, female, child) |
| `ch_tract.c` | The head model: articulators to tract areas, the slew limits, the noise sources | `double`: areas within 1e-14 cm² over 2,552 control steps, every noise source identical |
| `ch_common.h` | What they share; the FM-1's maths | |

The lab (`lab/`) records CHIMERA's own voice (`cargo run --release -- record`), replays it through the C port
(`lab/port/replay.c`) and compares (`cargo run --release -- compare`). See the README's Layout section.

For scale: CHIMERA at 44.1 kHz without its 2x oversampling is 3.4 dB from the oversampled original, and was judged
"almost, but not fully" by ear. Everything below starts from that.

## What `ch_fast.c` changes (and what each change costs in sound)

- **44.1 kHz, no oversampling** (CHIMERA runs 2x): 46 nodes instead of 76. Stable for all three bodies.
- **The geometry every 16 samples instead of every sample**: inertances, resistances, kinetic coefficients,
  compliances, wall and radiation terms. Holding them naively broke the sound (+3.5 dB level, clicks): near a
  closure the inertance (1/A) grows tenfold in a few samples, and the output is the time derivative of the radiated
  flow, so any stepped flow is a click. Fixed by (a) recomputing every sample the nodes that move more than 2 % per
  update ("dynamic" nodes: closures and releases) and (b) ramping the moving-wall injection between updates.
  Result 1.15 dB from CHIMERA at 44.1 kHz.
- **The flow's quadratic** solved as `(c/bq) * 2/(1+sqrt(1+x))`, with the series `1 - x/4 + x²/8` when x < 1e-3
  (no constriction: almost always): no square root and no divide on most branches.
- **Data layout**: one struct per branch and per node (`ch_fbr_t`, `ch_fnd_t`) instead of CHIMERA's arrays: on the
  FM-1 each separate array cost an address computation. Walls fused into the node loop.
- **The head model**: its rays, roof projections and tongue distances skip whole chunks of outline by bounding box
  (exact: the same results, to the bit). Ray tests per step 6,425 → 994, projections 3,784 → 1,771, tongue-distance
  segments 3,017 → 2,346.
- Measured trade-off of running the head model less often (male / female / child spectral distance from CHIMERA):
  every step 1.15 / 0.76 / 0.73 dB, every 2nd 2.4 / 1.8 / 1.6 dB, every 4th 3.1 / 2.5 / 2.4 dB. The loss is real
  smoothing of fast consonant movements, not latency.

## What the FM-1 measured

The test firmware (`app/`, versions 0.0.1 to 0.0.3) sings one held vowel, the airway on the second core and the head
model for the next block on the first, in 256-sample blocks (5.8 ms), and shows each core's share of the block.

| Build | Airway (core 2) | Head model (core 1) | fm1-base's audio load |
| --- | --- | --- | --- |
| 0.0.1: everything from the flash (XIP) | ~208 % (swing peak ~240 %) | ~183 % | 206-214 % |
| 0.0.3: the airway and the head model's three inner loops in RAM (`.c1_text`) | 195 % (swing 207 %, peak 235 %) | 158 % | 200-209 % |

(0.0.1's screen showed every share 3.32x too high: `256 * 24000000` overflowed 32 bits. The table is corrected.)

- **Not the flash cache**: moving the hot code to RAM gained only 6-14 %.
- **Not soft float**: the objects call no `__*sf*` routines; the FPU instructions are there (`-mcpu=r3 -mfprev1`).
- **It is the arithmetic**: the airway takes about 15,900 cycles a sample against 8,160 available (360 MHz / 44.1 kHz).
  Static instruction counts from the compiler's assembly (`lab/tools/pi_cost.py`) predicted ~7,000: the FM-1 runs
  this code at about 2.3-2.5 cycles an instruction. The head model came out about 5x its estimate.
- Overloaded, the audio buffer replays stale audio (a constant sound with no key, until 0.0.3 let the voice rest)
  and the screen gets little time.

## Tried and rejected

- **The airway at 22.05 kHz** (CHIMERA's minimum of 8 sections a tube lowered to 4 to stay stable: 24 nodes, about
  4x cheaper; `lab/patch-chimera.sh` `CHIMERA_MIN_SECTIONS`). Against CHIMERA at 44.1 kHz: +5 to +10 dB from 1.3 to
  4 kHz (where F2 and F3 are) and -26 dB at 8 kHz. It changes the vowels. Rejected.

## Untried ideas, roughly by promise

1. **Measure the FPU.** A microbenchmark of dependent and independent float multiplies, adds and divides on the
   FM-1 would say whether the cost is latency (then interleaving independent work, two branches at a time, would
   pay) or throughput (then only fewer operations will).
2. **Fewer sections at 44.1 kHz.** At 44.1 kHz the 46 nodes are: lungs 1, trachea 13, pharynx 8, oral 8, nasal 13,
   piriform fossa 2, sinus 1. The trachea (subglottal resonances only) could be 6 sections or a lumped impedance;
   the nose 8 (CHIMERA's own minimum); the fossa 1. Estimated 30-40 % saved, which alone leaves ~120-135 %.
3. **Walls only where they matter.** Every node with a wall costs a mass-spring update a sample; the trachea's and
   the nose's walls are stiff and could go.
4. **Fixed point** for the parts with a bounded range (the walls, the noise filters), if the FPU's latency is the
   cost.
5. **The head model**: compute it every 2nd control step (measured above), split its rays across both cores, and
   look at what the bounding boxes left: the roof projections and tongue distances now dominate.
6. **A cheaper glottis**: two cosines and several divides a sample; a table of the pulse shape would do.

Combining 2, 3 and 6 with the head model every 2nd step might reach ~100 % on two cores: not enough headroom for
a product, but close enough that someone with time could find the rest.

## How to rebuild the test firmware

The timing test is kept as its own fm1-base app, `lab/fdtd-test/`: the firmware as it stood at 0.0.3, `app.c` (the
two-core pipeline, the readout, the rest when silent), `dsp/voice_male.h` (the male airway's layout, generated from a
recording by `lab/tools/gen_voice_header.py`), with `engine/ch_fast.c` and `engine/ch_tract.c` (`fm1.toml`'s `o2`
list). Its tests: `tests/timing.txt`, `silence.txt`, `rest.txt`.

    .venv/bin/python base/tools/build.py lab/fdtd-test --host        # the simulator
    JIELI_TOOLCHAIN=~/.jieli/toolchain .venv/bin/python base/tools/build.py lab/fdtd-test --sdk ~/fw-AC79_AIoT_SDK
    .venv/bin/python base/tools/fm1_install.py lab/fdtd-test/build/vocalfdtd.fwsc   # or the installer page The readout: CORE 2
AIRWAY and CORE 1 ANATOMY as a share of the 5.8 ms block (average and peak over half a second), AUDIO LOAD (fm1-base's
own), XRUNS; PLAY swings between a vowel and a closure every 150 ms (the worst case).
