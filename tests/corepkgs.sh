#!/usr/bin/env bash
# The embedded corepkgs strings are up to date: tests/gen-corepkgs.sh into a temp copy must equal nx/. Exit 1 if stale.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
timeout 5 "$here/tests/gen-corepkgs.sh" "$tmp" || { echo "FAIL gen-corepkgs"; exit 1; }
fail=0
for f in b_core.srs b_flake.srs; do
  diff -q "$here/nx/$f" "$tmp/$f" > /dev/null || { echo "FAIL nx/$f is stale: run tests/gen-corepkgs.sh"; fail=1; }
done
[[ $fail == 0 ]] && echo "ok corepkgs"
exit $fail
