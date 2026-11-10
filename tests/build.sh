#!/usr/bin/env bash
# Builds snx into $B/snx (B = $XDG_RUNTIME_DIR/snx-build) with the s2c of ~/hk/srs.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
SRS=${SRS:-$HOME/hk/srs}
B=${B:-${XDG_RUNTIME_DIR:-/tmp}/snx-build}
S2C=${S2C:-${XDG_RUNTIME_DIR:-/tmp}/srs-build/s2c1}
mkdir -p "$B"
srcs=("$here"/nx/ast.srs "$here"/nx/rt.srs)
for f in "$here"/nx/*.srs; do
  case $f in */ast.srs|*/rt.srs|*/main.srs|*test.srs|*main.srs) ;; *) srcs+=("$f") ;; esac
done
srcs+=("$here"/nx/main.srs)
cd "$SRS"
timeout 60 "$S2C" lib/prelude.srs s2/coll.srs "${srcs[@]}" > "$B/snx.c"
timeout 120 zig cc -std=gnu11 -O2 -w -o "$B/snx" "$B/snx.c" "$SRS/rt/srs_rt.c" "$here/rt/snx_rt.c" "$here/rt/snx_hash.c"
echo "$B/snx"
