#!/usr/bin/env bash
# compile nx/tree.srs + nx/treetest.srs with s2c1, run, diff against tests/tree.out. Exit 0 = ok.
WS=$(cd "$(dirname "$0")/.." && pwd)
SRS=$HOME/hk/srs
S2C=$XDG_RUNTIME_DIR/srs-build/s2c1
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
cd "$SRS" || exit 2
timeout 25 "$S2C" lib/prelude.srs s2/coll.srs "$WS/nx/ast.srs" "$WS/nx/rt.srs" "$WS/nx/tree.srs" "$WS/nx/treetest.srs" > "$OUT/x.c" 2> "$OUT/s2c.err" \
  || { echo "FAIL: s2c"; head -20 "$OUT/s2c.err"; exit 1; }
timeout 60 zig cc -std=gnu11 -O2 -w -o "$OUT/x" "$OUT/x.c" "$SRS/rt/srs_rt.c" "$WS/rt/snx_rt.c" 2> "$OUT/cc.err" \
  || { echo "FAIL: cc"; head -20 "$OUT/cc.err"; exit 1; }
timeout 5 "$OUT/x" > "$OUT/got" || { echo "FAIL: run"; exit 1; }
if diff "$OUT/got" "$WS/tests/tree.out" > "$OUT/diff"; then echo "ok tree"; else echo "FAIL tree"; head -20 "$OUT/diff"; exit 1; fi
