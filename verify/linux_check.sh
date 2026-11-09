#!/bin/sh
# Compiles every source file with gcc as the Linux build does (no linking), using the bundled SDL headers.
cd "$(dirname "$0")/.."
INC=third_party/sdl2_dl/SDL2-2.30.9/include
for f in src/*.c tools/apeiron_export.c tools/apeiron_scores.c; do
  gcc -std=c11 -O2 -Wall -Wno-unused-function -D_DEFAULT_SOURCE -I$INC -I$INC/SDL2 -c -o /dev/null $f 2>&1 | grep -E 'error|warning' | grep -v format-truncation
done
echo done
