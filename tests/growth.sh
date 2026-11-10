#!/usr/bin/env bash
# snx parse is linear in the input: a pattern of N formals and a set of N merged names (`x = { aI = I; };` N times), N =
# 1000, 4000, 16000, parsed by `snx parse`; the user-mode instruction count of a 4x larger input must be at most 4.4x
# (a rescan per name or per formal makes it 16x). Then the load limit: a file one byte over Nx.loadLimit is refused
# with the "file too large to load" error, before it is lexed. SNX = the snx binary (default: tests/build.sh builds it);
# PERF = a perf (default: perf on PATH, else linuxPackages.perf from nix-shell). Exit 0 = ok, 1 = fail, 77 = no perf.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
SNX=${SNX:-$("$here/tests/build.sh")} || { echo "FAIL build"; exit 1; }
PERF=${PERF:-$(command -v perf || timeout 300 nix-shell -p linuxPackages.perf --run 'command -v perf' 2>/dev/null)}
[[ -x ${PERF:-} ]] || { echo "SKIP growth: no perf"; exit 77; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
ulimit -s unlimited
fail=0
instr() { timeout 20 "$PERF" stat -x, -e instructions:u "$SNX" parse "$1" 2>&1 >/dev/null | cut -d, -f1 | grep -E '^[0-9]+$' || echo 0; }
for kind in formals merges; do
  prev=0
  for n in 1000 4000 16000; do
    f=$T/$kind$n.nix
    case $kind in
      formals) awk -v n="$n" 'BEGIN { printf "{ "; for (i = 1; i <= n; i++) printf "%sa%d", (i > 1 ? ", " : ""), i; print " }: a1" }' > "$f" ;;
      merges) awk -v n="$n" 'BEGIN { print "{"; for (i = 1; i <= n; i++) printf "  x = { a%d = %d; };\n", i, i; print "}" }' > "$f" ;;
    esac
    got=$(instr "$f")
    if [[ $prev != 0 ]]; then
      # ratio in tenths, rounded down: <= 44
      r=$(( got * 10 / prev ))
      if [[ $got == 0 || $r -gt 44 ]]; then echo "FAIL $kind $((n / 4)) -> $n: $prev -> $got instr (ratio x10 = $r, limit 44)"; fail=1
      else echo "ok $kind $((n / 4)) -> $n: $prev -> $got instr (ratio x10 = $r)"; fi
    fi
    prev=$got
  done
done
# the load limit
big=$T/big.nix
truncate -s $((134217728 + 1)) "$big"
want="error: file too large to load: $big is 134217729 bytes, the limit is 134217728 bytes"
got=$(timeout 10 "$SNX" eval "$big" 2>&1)
if [[ $got == "$want" ]]; then echo "ok load limit"; else echo "FAIL load limit: got: $got"; fail=1; fi
exit $fail
