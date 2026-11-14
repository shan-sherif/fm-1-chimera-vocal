# VOCAL: the timing test

**A port of [CHIMERA](https://github.com/jamesondh/chimera-vocal-synth) by Jameson Hodge (MIT) to the M-VAVE FM-1**,
on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal. The voice is CHIMERA's: a simulated
airway, not samples.

This version measures whether the voice fits the FM-1. It has one sound: an open vowel, sung by an adult male voice.
Since 0.0.4 the throat and mouth are a waveguide (pressure waves along the tract) driven by CHIMERA's anatomy, vocal
folds and noise sources: CHIMERA's full airflow simulation is about twice what the FM-1 can run (the measurements
are in the source's `docs/full-physics-on-fm1.md`).

## Playing it

- **The keys** sing the vowel at their pitch (the last key held wins). Let go and the breath fades out; about 50 ms
  after the sound has died away the voice rests (nothing is simulated, the output is silent) until the next key.
- **OCT− / OCT+** move the keys an octave.
- **PLAY** switches the mouth between the held vowel and a swing: every 150 ms it closes and opens again. Closing and
  opening are the hardest work for the simulation, so this is the worst case.
- **SEQ** sets how often the anatomy (the tongue, jaw and lip positions turned into the tract's shape) runs: every
  control step (689 a second), every 2nd (the default) or every 4th, the tract gliding between. Less often is cheaper
  and a little less sharp in the consonants.
- **MASTER** is the volume.

## What the screen shows

| Line | What it means |
| --- | --- |
| CORE 2 TRACT | The second core's share of each audio block (5.8 ms): the sound waves through the throat, mouth and nose, and the vocal folds, every sample. The average over the last half second, then the worst block. |
| CORE 1 ANATOMY | The first core's share: turning the tongue, jaw and lip positions into the shape of the tract, as often as SEQ says. |
| AUDIO LOAD | The whole audio interrupt's share of its time. |
| XRUNS | Blocks that were late since power-on: each is a click. It should stay at 0. |
| TWO CORES / ONE CORE | Whether the second core is running. If it fails, the voice continues on one core. |

Anything under 100 % fits; the lower, the more room is left for the rest of the instrument.

Please write down both lines (average and peak) with the vowel held and with PLAY's swing, at each of SEQ's three
settings. With no key held
the voice rests, so those lines read 0 %.
