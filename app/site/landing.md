# VOCAL

**A port of [CHIMERA](https://github.com/jamesondh/chimera-vocal-synth) to the M-VAVE FM-1.** CHIMERA is a
physical-modelling voice synthesizer by **Jameson Hodge**, released under the MIT licence: lungs, vocal folds, throat,
mouth and nose, simulated. How the voice works is his. This firmware only adapts it to the FM-1. It's built on
[fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal and
[felucca-bsp](https://github.com/hugelton/felucca-bsp) by Leo Kuroshita, Hügelton Instruments.

This is version {{VERSION}}, a **timing test**, not an instrument yet. It sings one held vowel at the pitch of the key
you hold and shows how long each of the FM-1's two processor cores takes to make the sound. PLAY switches between the
held vowel and a fast swing between the vowel and a closed mouth, the hardest case.

You install it at your own risk, and the installer can put M-VAVE's own firmware back.

- [Install](install/) it from Chrome or Edge, with the FM-1 connected by USB.
- The [Manual](manual/) explains what the screen shows.
- [Download {{PKG}}](firmware/{{PKG}}) to use the command-line installer: `python3 tools/fm1_install.py {{PKG}}`.
- The [Source](https://github.com/shan-sherif/fm-1-chimera-vocal): the voice's port is MIT (CHIMERA's licence), the
  firmware's platform MPL-2.0 (fm1-base's).

Unofficial: not affiliated with or endorsed by M-VAVE, Jameson Hodge or fm1-base's authors. M-VAVE and FM-1 are
trademarks of their owners.
