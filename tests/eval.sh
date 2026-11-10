#!/usr/bin/env bash
# snx eval core: the unit cases tests/eval/*.nix (NAME.exp = what `nix-instantiate --eval --strict` printed, or
# `error` when it failed), then nix-conformance lang with a pass count per category.
# SNX = the snx binary (default: tests/build.sh builds it). CONF = the nix-conformance checkout.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
CONF=${CONF:-$HOME/hk/nix-conformance}
SNX=${SNX:-$("$here/tests/build.sh")} || { echo "FAIL build"; exit 1; }
export SNX
fail=0 n=0
for f in "$here"/tests/eval/*.nix; do
  b=${f%.nix}
  n=$((n + 1))
  if got=$(HOME=/fake-home TEST_VAR=foo timeout 5 "$SNX" eval "$f" 2>/dev/null); then :; else got=error; fi
  [[ $got == "$(cat "$b.exp")" ]] || { echo "FAIL $(basename "$f"): want $(cat "$b.exp") got $got"; fail=1; }
done
echo "unit: $n cases, $([[ $fail == 0 ]] && echo all pass || echo FAILURES)"

# conformance: the runner prints FAIL/SKIP lines; a category's passes = its cases - its failures - its skips
out=$("$CONF/run" "$here/adapter/snx-adapter" lang 2>&1)
grep "passed," <<< "$out"
cat_of() { case $1 in *regex*) echo "${1%%-regex*}-regex" ;; *fromTOML*) echo "${1%%-fromTOML*}-fromTOML" ;; *) echo "${1%%-*}-$(cut -d- -f2 <<< "$1")" ;; esac; }
declare -A total bad
for f in "$CONF"/lang/{eval,parse}-*.nix; do
  b=$(basename "$f" .nix)
  [[ $b == eval-okay-* && -e ${f%.nix}.exp-disabled ]] && continue
  c=$(cat_of "$b"); total[$c]=$(( ${total[$c]:-0} + 1 ))
done
while read -r st path; do
  [[ $st == FAIL || $st == SKIP ]] || continue
  c=$(cat_of "$(basename "$path" .nix)"); bad[$c]=$(( ${bad[$c]:-0} + 1 ))
done <<< "$out"
for c in $(printf '%s\n' "${!total[@]}" | sort); do echo "  $c: $(( total[$c] - ${bad[$c]:-0} ))/${total[$c]}"; done
exit $fail
