#!/usr/bin/env bash
# Builds the drv bench ($B/drvbench): unparse + SHA-256 of one .drv, N times. Usage: drvbench.sh FILE N
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
SRS=${SRS:-$HOME/hk/srs}
B=${B:-${XDG_RUNTIME_DIR:-/tmp}/snx-build}
S2C=${S2C:-${XDG_RUNTIME_DIR:-/tmp}/srs-build/s2c1}
cd "$SRS"
timeout 60 "$S2C" lib/prelude.srs s2/coll.srs "$here"/nx/ast.srs "$here"/nx/rt.srs "$here"/nx/hash.srs "$here"/nx/toml.srs "$here"/nx/drv.srs "$here"/nx/drvbenchmain.srs > "$B/drvbench.c"
timeout 120 zig cc -std=gnu11 -O2 -w -o "$B/drvbench" "$B/drvbench.c" "$SRS/rt/srs_rt.c" "$here/rt/snx_rt.c" "$here/rt/snx_hash.c"
exec timeout 25 "$B/drvbench" "$@"
