#!/bin/sh
# Build the replay tool in its variants (host), into ../out/:
#   replay64   double (-DCH_DOUBLE), the literal port's check against CHIMERA
#   replay32   float, with libm
#   replay32f  float with the port's own fast sqrt and cos
#   replay_fm1 float with the FM-1 build's maths (fm1-base's fastmath.h, -DCH_FM1), as on the device
set -e
cd "$(dirname "$0")"
E=../../engine
SRC="replay.c $E/ch_voice.c $E/ch_fast.c $E/ch_tract.c $E/ch_wave.c"
W="-std=c99 -Wall -Wextra -Wno-unused-parameter -ffp-contract=off -I$E"
cc -O2 $W -DCH_DOUBLE -o ../out/replay64 $SRC -lm
cc -O2 $W -o ../out/replay32 $SRC -lm
cc -O2 $W -DCH_FASTMATH -o ../out/replay32f $SRC -lm
cc -O2 $W -DCH_FM1 -DPLAT_HOST -I../../base/include -o ../out/replay_fm1 $SRC -lm
