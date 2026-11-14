# VOCAL: the timing test

**A port of [CHIMERA](https://github.com/jamesondh/chimera-vocal-synth) by Jameson Hodge (MIT) to the M-VAVE FM-1**,
on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal. The voice is CHIMERA's: a simulated
airway, not samples.

This version measures whether the voice fits the FM-1. It has one sound: an open vowel, sung by an adult male voice.

## Playing it

- **The keys** sing the vowel at their pitch (the last key held wins). Let go and the breath fades out; about 50 ms
  after the sound has died away the voice rests (nothing is simulated, the output is silent) until the next key.
- **OCT− / OCT+** move the keys an octave.
- **PLAY** switches the mouth between the held vowel and a swing: every 150 ms it closes and opens again. Closing and
  opening are the hardest work for the simulation, so this is the worst case.
- **MASTER** is the volume.

## What the screen shows

| Line | What it means |
| --- | --- |
| CORE 2 AIRWAY | The second core's share of each audio block (5.8 ms): the air moving through the throat, mouth and nose, every sample. The average over the last half second, then the worst block. |
| CORE 1 ANATOMY | The first core's share: turning the tongue, jaw and lip positions into the shape of the tract, 689 times a second. |
| AUDIO LOAD | The whole audio interrupt's share of its time. |
| XRUNS | Blocks that were late since power-on: each is a click. It should stay at 0. |
| TWO CORES / ONE CORE | Whether the second core is running. If it fails, the voice continues on one core. |

Anything under 100 % fits; the lower, the more room is left for the rest of the instrument.

Please write down both lines (average and peak) with the vowel held, and again with PLAY's swing. With no key held
the voice rests, so those lines read 0 %.
