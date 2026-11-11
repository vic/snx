#!/usr/bin/env bash
# Builds snx into $B/snx (B = $XDG_RUNTIME_DIR/snx-build) with the s2c that tests/s2c.sh builds from ~/hk/srs-main.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
# ~/hk/srs-main is a jj workspace of ~/hk/srs at its main bookmark (~/hk/srs itself is the srs session's working copy)
SRS=${SRS:-$HOME/hk/srs-main}
B=${B:-${XDG_RUNTIME_DIR:-/tmp}/snx-build}
mkdir -p "$B"
S2C=${S2C:-$(SRS="$SRS" "$here/tests/s2c.sh")}
srcs=("$here"/nx/ast.srs "$here"/nx/rt.srs)
for f in "$here"/nx/*.srs; do
  case $f in */ast.srs|*/rt.srs|*/main.srs|*test.srs|*main.srs) ;; *) srcs+=("$f") ;; esac
done
srcs+=("$here"/nx/main.srs)
cd "$SRS"
timeout 60 "$S2C" lib/prelude.srs s2/coll.srs "${srcs[@]}" > "$B/snx.c"
# every observation extern goes through rt/snx_trace.c (the SNX_CACHE memo): -Wl,--wrap=X sends calls of X to __wrap_X
wrap=()
for x in Nx__readFile rt_read_file Nx__fileBytes Nx__getEnv Nx__dirNames Nx__readLink Nx__cwd Nx__realPath Nx__isDir Nx__fsExec Nx__fsKind Nx__timeNow Nx__netOpen; do wrap+=("-Wl,--wrap=$x"); done
# SNX_CFLAGS=-DSNX_PROBES builds in the SNX_PROBE switches (rt/snx_rt.c Nx__probe)
timeout 120 zig cc -std=gnu11 -O2 -flto -w ${SNX_CFLAGS:-} "${wrap[@]}" -o "$B/snx" "$B/snx.c" -I "$SRS/rt" "$SRS/rt/srs_rt.c" "$here/rt/snx_rt.c" "$here/rt/snx_hash.c" "$here/rt/snx_fs.c" "$here/rt/snx_net.c" "$here/rt/snx_trace.c"
echo "$B/snx"
