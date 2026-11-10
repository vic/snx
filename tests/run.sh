#!/usr/bin/env bash
# The snx gate: build, the unit tests, then nix-conformance with the cases snx fails on purpose in tests/known.
# CONF = the nix-conformance checkout (default ~/hk/nix-conformance). Extra args = conformance paths (default parse).
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
CONF=${CONF:-$HOME/hk/nix-conformance}
export SNX
SNX=$("$here/tests/build.sh") || { echo "FAIL build"; exit 1; }
fail=0
for t in "$here"/tests/*.sh; do
  case $t in */run.sh|*/build.sh) continue ;; esac
  "$t" > "${SNX%/*}/$(basename "$t").log" 2>&1 && echo "ok $(basename "$t")" || { echo "FAIL $(basename "$t")"; fail=1; }
done
paths=("$@")
[[ ${#paths[@]} -gt 0 ]] || paths=(parse)
"$CONF/run" --tree --known "$here/tests/known" "$here/adapter/snx-adapter" "${paths[@]}" | tail -3 || fail=1
exit $fail
