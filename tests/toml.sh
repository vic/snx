#!/usr/bin/env bash
# builtins.fromTOML oracle: nix-conformance lang eval-okay-fromTOML-* (output diffed against .exp) and the cases that must
# fail (corpora/toml-invalid.nix, eval-fail-fromTOML-*). Compiles nx/toml.srs + nx/tomltest.srs. Exit 0 = all ok.
WS=$(cd "$(dirname "$0")/.." && pwd)
SRS=${SRS:-$HOME/hk/srs-main}
S2C=${S2C:-$("$(dirname "$0")/s2c.sh")}
LANGD=${LANGD:-$HOME/hk/nix-conformance/lang}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
cd "$SRS" || exit 2
timeout 25 "$S2C" lib/prelude.srs s2/coll.srs "$WS/nx/rt.srs" "$WS/nx/toml.srs" "$WS/nx/tomltest.srs" > "$OUT/x.c" 2> "$OUT/s2c.err" \
  || { echo "FAIL: s2c"; head -20 "$OUT/s2c.err"; exit 1; }
timeout 60 zig cc -std=gnu11 -O2 -w -o "$OUT/x" "$OUT/x.c" "$SRS/rt/srs_rt.c" "$WS/rt/snx_rt.c" 2> "$OUT/cc.err" \
  || { echo "FAIL: cc"; head -20 "$OUT/cc.err"; exit 1; }

# a toml-text -> one record "<flag>\t<escaped text>" ; escape = backslash, newline
rec() { printf '%s\t%s\n' "$1" "$2"; }

# eval-okay: d of the .exp is already a Nix-escaped string
for e in "$LANGD"/eval-okay-fromTOML-[0-9]*.exp; do
  f=${e%.exp}.flags
  flag=0; grep -q parse-toml-timestamps "$f" 2>/dev/null && flag=1
  line=$(cat "$e")
  d=${line#\{ d = \"}; d=${d%%\"; v = *}
  rec $flag "$d"
done > "$OUT/ok.in"
cat "$LANGD"/eval-okay-fromTOML-[0-9]*.exp > "$OUT/ok.exp"

# a '' string body of eval-{okay,fail}-fromTOML-<name>.nix as a record text: drop the first and last line, 2-space indent
body() { sed '1d;$d' "$1" | sed "s/'''/''/g; s/^  //" | awk 'BEGIN{ORS=""} {gsub(/\\/,"\\\\"); print $0 "\\n"}'; }
rec 2 "$(body "$LANGD/eval-okay-fromTOML-timestamps.nix")" > "$OUT/ts.in"
cp "$LANGD/eval-okay-fromTOML-timestamps.exp" "$OUT/ts.exp"

# must fail: toml-invalid.nix entries, in order, then the eval-fail-fromTOML-* one-liners
{
  grep '^  "' "$LANGD/corpora/toml-invalid.nix" | sed 's/^  "//; s/"$//' | while IFS= read -r d; do rec 1 "$d"; done
  rec 1 'attr = 9223372036854775808'
  rec 1 'attr = -9223372036854775809'
  rec 1 '"a\\u0000b" = 1'
  rec 1 'k = "a\\u0000b"'
  rec 0 "$(body "$LANGD/eval-fail-fromTOML-timestamps.nix")"
} > "$OUT/bad.in"

run() { timeout 5 "$OUT/x" "$1" > "$2" 2>&1; }
status=0
run "$OUT/ok.in" "$OUT/ok.got" || { echo "FAIL: run ok"; status=1; }
run "$OUT/ts.in" "$OUT/ts.got" || { echo "FAIL: run ts"; status=1; }
run "$OUT/bad.in" "$OUT/bad.got" || { echo "FAIL: run bad"; status=1; }

M=$(wc -l < "$OUT/ok.exp")
N=$(diff "$OUT/ok.got" "$OUT/ok.exp" | grep -c '^<')
echo "ok toml $((M - N))/$M"
if [ "$N" -ne 0 ]; then status=1; diff "$OUT/ok.got" "$OUT/ok.exp" | head -10; fi
if diff "$OUT/ts.got" "$OUT/ts.exp" > "$OUT/ts.diff"; then echo "ok toml-timestamps 1/1"; else echo "ok toml-timestamps 0/1"; status=1; head -c 1500 "$OUT/ts.diff"; fi
B=$(wc -l < "$OUT/bad.in")
G=$(grep -c '^ERROR$' "$OUT/bad.got")
echo "ok toml-invalid $G/$B"
if [ "$G" -ne "$B" ]; then status=1; paste -d'|' "$OUT/bad.in" "$OUT/bad.got" | grep -v '|ERROR$' | head -10; fi
exit $status
