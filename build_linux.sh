#!/bin/sh
# Builds the game on Linux (Debian / Ubuntu). Needs:  sudo apt install build-essential libsdl2-dev
# Run from the apeiron-sdl folder:  ./build_linux.sh      then      cd bin && ./apeiron <folder with the original files>
set -e
cd "$(dirname "$0")"
mkdir -p bin
CFLAGS="-std=c11 -O2 -Wall -Wno-unused-function -D_DEFAULT_SOURCE"
gcc $CFLAGS $(sdl2-config --cflags) -o bin/apeiron \
    src/apeiron_sdl.c src/game.c src/creatures.c src/rng.c src/mixer.c src/snd.c src/rsrc.c src/pict.c src/ppat.c \
    src/scores.c src/font.c src/fontdata.c src/writers.c src/title.c src/cicn.c src/pictdraw.c \
    $(sdl2-config --libs) -lm
gcc $CFLAGS -o bin/apeiron_export tools/apeiron_export.c src/rsrc.c src/pict.c src/ppat.c src/snd.c src/writers.c -lm
gcc $CFLAGS -o bin/apeiron_scores tools/apeiron_scores.c src/rsrc.c src/scores.c -lm
[ -f apeiron.ini ] && cp -n apeiron.ini bin/ || true
echo "built bin/apeiron"
