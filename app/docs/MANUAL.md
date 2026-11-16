# CHIMERA

**A port of [CHIMERA](https://github.com/jamesondh/chimera-vocal-synth) by Jameson Hodge (MIT) to the M-VAVE FM-1**,
on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal. The voice is CHIMERA's: a simulated
airway (lungs, vocal folds, throat, mouth and nose), not samples.

FM-1 Chimera Vocal sings words: each key sings the next syllable of a line, at the key's pitch, through CHIMERA's own
text-to-speech front end. In VOWEL mode the keys play pitch and the knobs move the mouth through the vowels. The screen
is CHIMERA's flat view, live: the head moving as it sings.

![The boot screen: FM-1 Chimera Vocal, the version, the credits](img/boot.png "The boot screen")
![The home screen in WORDS: the head singing, NOW LO, NEXT THERE, page 1's knobs at the bottom](img/home-words.png "WORDS: singing the lo of Hello, there next; page 1's knobs below")

## The keys

- **WORDS** (the start, and HOME): each key sings the next syllable of the word slot: the consonants before the vowel
  when you press, the vowel for as long as you hold, the consonants after it when you let go. Press the next key
  before letting go of the last and the two syllables join (legato). Harder presses breathe harder. After the last
  syllable it starts again.
- **VOWEL** (FX): the keys play pitch only, the voice holding the vowel the knobs set; the last key held sounds, the
  pitch gliding (Glide) between keys.
- MIDI in plays as the keys do, and lights the keys its notes fall on. **OCT− / OCT+** move the keys an octave. About 50 ms after the sound dies away the
  voice rests (nothing is simulated, the output is silent).

## The buttons

| Button | What it does |
| --- | --- |
| HOME | WORDS mode, page 1 (SING). |
| FX | VOWEL mode and back (a quick press). Held with a key 1-12: that vowel snapshot (below). Its light is on in VOWEL mode. |
| SEL | The next of pages 2-5. (Until 0.5.0 this was EDIT.) |
| EDIT | Opens the word editor on the slot being sung: type words on the keys ([below](#typing-words)). (From 0.5.0; before, EDIT stepped the pages.) |
| PLAY | Speaks the word slot's words (in WORDS mode). Pressing a key while it speaks starts singing instead. |
| SAVE | Keeps the knobs in the voice preset, the vowel snapshots and the settings, through power-off. Its light is on while something is unsaved. |
| GLO | The debug page (the cores' timings) and back. |

## The knobs

**K1-K4** are the page's four values, shown at the bottom of the screen in their colours (K1 green, K2 red, K3 pink,
K4 white). **SELECT** turns the page. The ranges are CHIMERA's own.

| Page | K1 | K2 | K3 | K4 |
| --- | --- | --- | --- | --- |
| 1 SING | Vibrato (cents, sung notes) | Rate (vibrato, Hz) | Breathy | Ring (the singer's ring, near 3 kHz) |
| 2 SHAPE | Jaw (opening, cm) | Lips (opening, cm) | Larynx (up / down, cm) | Palate (the soft palate: SHUT, or the nose's opening, cm²) |
| 3 MOUTH | Round (the lips' rounding) | Protrude (the lips, cm) | Root (the tongue root, cm) | Curl (the tongue tip's) |
| 4 SOURCE | Breath (the lungs' pressure, cmH₂O) | Pressed (the vocal folds) | Rough | Fry |
| 5 PLAY | Glide (pitch, ms) | Speed (PLAY's speech, ×) | F.Folds (the false folds) | Bright (the vocal folds' closure: darker ← → brighter) |
| VOWEL | Front (← front, back →: F2, Hz) | Open (close → open: F1, Hz) | Nasal (the soft palate) | Round |

![Page 4, SOURCE: Breath, Pressed, Rough and Fry in green, red, pink and white](img/page-source.png "Page 4, SOURCE: K1 to K4 in their colours")

The shaping knobs (pages 2 and 3) set the voice's posture, as CHIMERA's do: they shape the held vowels and the voice
between words; while it sings words, the words' own movements lead.

**Bright** (page 5, K4) is how sharply the vocal folds close on each vibration, CHIMERA's own brightness. Low is soft
and dark, high is forward and edgy. The presets use it as part of their character.

## Body

**ALGORITHM** chooses the voice's body, on every page: **MALE**, **FEMALE** or **CHILD**, CHIMERA's three bodies. The
body is the whole airway, its size, its throat's length, its tongue and lips, with each body's own calibrated vowels
and consonants, so a female or child voice is a different instrument, not a pitch shift. The screen names the body as
you turn, and it changes when the knob rests (the voice switches to the new body's anatomy, with a quick fade). The head on the
screen is drawn in the body's size, and the header shows the body beside the preset.

![The child's body: a smaller head, CHILD in the header beside the preset](img/body-child.png "ALGORITHM on CHILD: the smaller head")

- **The keys still set the pitch.** Each body speaks (PLAY) at its own pitch: 112 Hz male, 205 Hz female, 250 Hz
  child. Move OCT− / OCT+ to where the body sounds best.
- **VOWEL mode follows the body:** Front and Open, and the twelve snapshots, reach the same vowels in every body, at
  that body's formants.
- **Presets keep their body:** SAVE stores it with the knobs, so a preset can be a child's or a female voice.

(From 0.5.0. Until then ALGORITHM was Bright, and page 5's K4 was Body, which only had the male.)

**VOWEL** is CHIMERA's vowel pad: Front and Open choose a place among the vowels (by their formants, F1 and F2), and
the voice blends the four calibrated vowels nearest it. The screen names the vowel made (in ARPAbet's letters: AA as
in "father", IY "see", UW "you", AE "cat", AX the neutral schwa).

![VOWEL mode: the vowel AO with its formants, Front, Open, Nasal and Round at the bottom](img/home-vowel.png "VOWEL: a key held, the knobs on an open back vowel")

**Vowel snapshots:** hold FX and press one of the first twelve keys to jump to that vowel (they start as twelve
English vowels: IY IH EY EH AE AA AO OW UH UW AH AX). Hold the key a second, with FX, to store the vowel you have
made there instead. SAVE keeps them.

## Presets and word slots

- **PRESET** chooses one of eight voice presets, loaded when the knob rests: NATURAL, BREATHY, BELT, OPERA, AIRY,
  ROUGH, FRY, TWANG (this port's own: CHIMERA has none). SAVE stores the knobs as they are into the preset chosen.
- **HOME + PRESET** (turn PRESET with HOME held) chooses the word slot the keys sing, 1-8, as the Studio sent them
  or as you typed them. The screen shows the slot and how it starts.

## Typing words

From 0.5.0 you can write the word slots on the FM-1 itself, without the Studio. **EDIT** opens the editor on the slot
the keys are singing.

![The word editor: slot 1's words with the cursor after SONG, the keyboard's letters below, G lit while held](img/edit.png "The word editor: typing, the G key held")

- **The keys type, a letter each**, in one of two layouts. **FX** switches between them, and the FM-1 remembers
  your choice. The screen's keyboard shows each key's letter and lights the key you hold. The keys don't sing while
  the editor is open; MIDI in still does.
  - **A-Z** (the first): the 26 lowest keys are A to Z from the bottom up, black keys included, and the top key (G5)
    is a space.
  - **QWERTY** in two rows: the black keys are a keyboard's top row, Q W E R T Y U I O P, with the last black key
    a space. The white keys are A S D F G H J K L, then Z X C V B N M. The first key of each row (Q, A and Z) and
    the space glow softly, and are green on the screen's keyboard, so you can find the rows.

![The word editor in QWERTY: the black keys Q to P and a space, the white keys A to L then Z to M, W lit](img/edit-qwerty.png "The word editor, QWERTY: the W key held")
- **K1** moves the cursor. **K2** changes the character before the cursor, through A-Z, space and the punctuation
  `, . ? ! ' -`. Commas and full stops give the voice its pauses, as in the Studio.
- **OCT−** deletes the character before the cursor (hold it to keep deleting), **OCT+** the one after it.
- **PLAY** speaks the words as they are typed, to try them.
- **SAVE** keeps them in the slot, through power-off, and leaves; the keys then sing them. Saving an empty slot
  removes it.
- **EDIT** or **HOME** leaves. If something is unsaved, the first press warns (UNSAVED) and a second press drops the
  changes.
- **PRESET** chooses another slot to edit, empty ones included (with nothing unsaved).

A slot holds up to 150 characters. The words you type are pronounced by the FM-1's built-in word list (about 480
common words) and its spelling rules; the Studio can give any word its dictionary pronunciation. The Studio reads the
slots back when it connects, so words typed on the FM-1 show up there too.

## What the screen shows

At the top: the page (SING 1/5 ... or VOWEL), the mode, the body and the preset; the note while one sounds. The head, side on,
moving as it sings: the tongue green, the jaw red, the lips pink, the soft palate white. Beside it in WORDS: NOW, the
syllable sounding (a hyphen when its word goes on; SPEAKING while PLAY speaks), NEXT, the one the next key sings, and
the word slot; in VOWEL: the vowel made and its formants. At the bottom: the page's four knobs.

**GLO** switches to the debug page and back: how long each core takes.

| Line | What it means |
| --- | --- |
| CORE 2 TRACT | The second core's share of each audio block (5.8 ms): the sound waves through the throat, mouth and nose, and the vocal folds, every sample. The average over the last half second, then the worst block. |
| CORE 1 CONTROL | The first core's share: moving the articulators along the gestures, then turning the tongue, jaw and lip positions into the shape of the tract. |
| AUDIO LOAD | The whole audio interrupt's share of its time. |
| XRUNS | Blocks that were late since power-on: each is a click. It should stay at 0. |
| TWO CORES / ONE CORE | Whether the second core is running. If it fails, the voice continues on one core. |

Anything under 100 % fits. The anatomy (the tongue, jaw and lip positions turned into the tract's shape) runs every
4th control step, the tract gliding between: more often overloads the FM-1 while it speaks.

## The Studio: your words

Open the site's **Studio** page in Chrome or Edge. On the left is an emulated FM-1 (the same firmware, in the
browser): press its **START**, then **Play** on a slot speaks that line there, and the emulator's keys sing it. On the
right are the eight slots, starting with CHIMERA's presets; each shows its syllables (one key each) and every word's
pronunciation from the CMU Pronouncing Dictionary. A word it does not know (a name, a made-up word) gets a box: say how
it sounds with words it knows ("kai mirror" for Chimera).

With your FM-1 on USB, **Connect** loads the slots it holds. **Send to FM-1** on a slot puts that line on it (its other
slots stay); **Update all slots** sends all eight. The FM-1 says "WORDS RECEIVED", keeps them through power-off and
sings the slot. A slot's badge says whether your FM-1 has it as shown. **Read from FM-1** brings back what it holds;
**Save file** / **Load file** keep sets of words on the computer.

## How the words are read

CHIMERA reads English with the CMU Pronouncing Dictionary (134,000 words), which does not fit in the FM-1. The
firmware knows the pronunciations of about 480 common words; others are read by spelling rules: CHIMERA's, plus a few
of this port's for doubled letters, plural and past-tense endings and the like. The rules get most words close but not
all exact, so an unusual word can come out oddly. Words sent from the Studio come with the dictionary's pronunciations,
so those are exact.
