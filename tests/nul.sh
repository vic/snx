#!/usr/bin/env bash
# builtins.readFile refuses a file with a NUL byte (nx/rt.srs Nx.hasNul), around the 16-byte step: every NUL offset of
# lengths 0 to 18, 23, 24, 31 to 33 and 40 with the other bytes 0x80, and each length with no NUL, all bytes 0x80 or
# 0x01 (each sets one half of the zero-byte test)
set -u
SNX=${SNX:-$("$(dirname "$0")/build.sh")}
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
printf 'builtins.stringLength (builtins.readFile %s/f)\n' "$OUT" > "$OUT/e.nix"
fail=0 runs=0
t() { # fill, length, NUL offset (-1: none)
  : > "$OUT/f"
  local i got want
  for ((i = 0; i < $2; i++)); do if [[ $i == "$3" ]]; then printf '\0'; else printf "$1"; fi; done > "$OUT/f"
  got=$(timeout 2 "$SNX" eval "$OUT/e.nix" 2>&1)
  if [[ $3 == -1 ]]; then want=$2; else want=refused; [[ $got == *"cannot be represented as a Nix string"* ]] && got=refused; fi
  runs=$((runs + 1))
  [[ $got == "$want" ]] || { echo "FAIL fill $1 n $2 nul $3: got $got"; fail=1; }
}
for n in $(seq 0 18) 23 24 31 32 33 40; do
  t '\x01' "$n" -1
  for ((at = -1; at < n; at++)); do t '\x80' "$n" "$at"; done
done
[[ $fail == 0 ]] && echo "nul: all pass ($runs)"
exit $fail
