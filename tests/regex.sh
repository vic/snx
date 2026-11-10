#!/usr/bin/env bash
# nx/regex.srs against nix-conformance: every eval-okay-regex-NNN.exp (corpora/regex.nix entry NNN) must come out
# byte-equal from nx/retest.srs, and every pattern of an eval-fail-regex-invalid-NNN-*.nix (corpora/regex-invalid.nix
# entry NNN) must be rejected by Nx.reCompile. Exit 0 = all equal.
WS=$(cd "$(dirname "$0")/.." && pwd)
SRS=$HOME/hk/srs
S2C=$XDG_RUNTIME_DIR/srs-build/s2c1
LANGD=${CONF:-$HOME/hk/nix-conformance}/lang
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
cd "$SRS" || exit 2
timeout 25 "$S2C" lib/prelude.srs s2/coll.srs "$WS/nx/ast.srs" "$WS/nx/rt.srs" "$WS/nx/val.srs" "$WS/nx/regex.srs" "$WS/nx/retest.srs" > "$B/x.c" 2> "$B/s2c.err" \
  || { echo "FAIL: s2c"; head -20 "$B/s2c.err"; exit 1; }
timeout 60 zig cc -std=gnu11 -O2 -w -o "$B/x" "$B/x.c" "$SRS/rt/srs_rt.c" "$WS/rt/snx_rt.c" 2> "$B/cc.err" \
  || { echo "FAIL: cc"; head -20 "$B/cc.err"; exit 1; }

# the okay cases: re = "…"; s = "…"; of each .exp, in file order
exps=("$LANGD"/eval-okay-regex-[0-9][0-9][0-9].exp)
cat "${exps[@]}" > "$B/want"
awk '{ i = index($0, "; }; re = \""); r = substr($0, i + 11); j = index(r, "\"; s = \""); re = substr(r, 1, j - 1)
       r = substr(r, j + 8); k = index(r, "\"; sp = { "); print re "\t" substr(r, 1, k - 1) }' "$B/want" > "$B/in.tsv"
timeout 10 "$B/x" "$B/in.tsv" > "$B/got"; echo "EXIT run=$?" > "$B/run.log"
fail=0
n=$(wc -l < "$B/want")
diffs=$(paste -d '\n' "$B/want" "$B/got" | awk -v names="${exps[*]}" 'BEGIN { split(names, f, " ") }
  NR % 2 == 1 { w = $0; next } { if (w != $0) { c++; if (c <= 10) { print "DIFF " f[NR / 2]; print "  want " substr(w, 1, 300); print "  got  " substr($0, 1, 300) } } }
  END { print c + 0 > "/dev/stderr" }' 2> "$B/ndiff")
got=$(wc -l < "$B/got")
bad=$(cat "$B/ndiff")
[[ $got -eq $n ]] || { echo "FAIL: $got answers for $n cases ($(cat "$B/run.log"))"; bad=$((bad + n - got)); }
echo "ok regex $((n - bad))/$n"
[[ -z $diffs ]] || { echo "$diffs"; fail=1; }

# the invalid patterns: entry N of corpora/regex-invalid.nix for each eval-fail-regex-invalid-N-match.nix
awk '/^\[/ { on = 1; next } on && /^ *"/ { s = $0; sub(/^ *"/, "", s); sub(/"[^"]*$/, "", s); print s }' \
  "$LANGD/corpora/regex-invalid.nix" > "$B/all-invalid"
for f in "$LANGD"/eval-fail-regex-invalid-*-match.nix; do
  k=${f##*invalid-}; k=$((10#${k%%-*}))
  sed -n "$((k + 1))p" "$B/all-invalid"
done > "$B/invalid"
timeout 10 "$B/x" "$B/invalid" bad > "$B/gotbad"
m=$(wc -l < "$B/invalid")
okb=$(grep -c '^bad$' "$B/gotbad")
echo "ok regex-invalid $okb/$m"
if [[ $okb -ne $m ]]; then
  paste "$B/invalid" "$B/gotbad" | grep -v 'bad$' | head -10 | sed 's/^/  accepted /'
  fail=1
fi
[[ $fail -eq 0 && $bad -eq 0 ]]
