#!/usr/bin/env bash
# Prints the path of an s2c built from $SRS/bootstrap/s2c-seed.c (SRS = ~/hk/srs-main, a jj workspace of ~/hk/srs at its
# main bookmark). It is rebuilt when the seed or rt/srs_rt.c is newer than it.
set -euo pipefail
SRS=${SRS:-$HOME/hk/srs-main}
D=${XDG_RUNTIME_DIR:-/tmp}/snx-s2c
mkdir -p "$D"
if [ ! -x "$D/s2c" ] || [ "$SRS/bootstrap/s2c-seed.c" -nt "$D/s2c" ] || [ "$SRS/rt/srs_rt.c" -nt "$D/s2c" ]; then
  timeout 300 zig cc -std=gnu11 -O2 -w -o "$D/s2c.tmp$$" "$SRS/bootstrap/s2c-seed.c" "$SRS/rt/srs_rt.c" >&2
  mv "$D/s2c.tmp$$" "$D/s2c"
fi
echo "$D/s2c"
