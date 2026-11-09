#!/bin/sh
# Copies the data folder with every name lowercased and checks the tools still find the files (Linux file names are case-sensitive).
cd "$(dirname "$0")/.."
gcc -std=c11 -D_DEFAULT_SOURCE -o /tmp/ap_scores tools/apeiron_scores.c src/rsrc.c src/scores.c -lm || exit 1
rm -rf /tmp/ap_lc && mkdir /tmp/ap_lc
for f in bin/data/*; do
  b=$(basename "$f" | tr 'A-Z' 'a-z')
  cp "$f" "/tmp/ap_lc/$b"
done
ls /tmp/ap_lc
/tmp/ap_scores /tmp/ap_lc 2>&1 | head -2
cat > /tmp/ap_t.c <<'C'
#include "rsrc.h"
#include <stdio.h>
int main(void){ ResFile rf; char e[300]; int r = rsrc_open(&rf, "/tmp/ap_lc", "Apeiron Sprites", e, sizeof e); printf("%s %s\n", r ? "NOT FOUND:" : "found", r ? e : rf.source); return r; }
C
gcc -std=c11 -D_DEFAULT_SOURCE -Isrc -o /tmp/ap_t /tmp/ap_t.c src/rsrc.c && /tmp/ap_t
