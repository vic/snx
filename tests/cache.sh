#!/usr/bin/env bash
# The SNX_CACHE memo (rt/snx_trace.c): a fixture a.nix imports b.nix, reads c.txt, tests pathExists d and reads $CV.
# A cold run stores, a warm run hits with the same output; after a change to any one input the next run misses and
# prints the new answer. A run that uses builtins.currentTime and a failed run are never stored.
# SNX = the snx binary (default: tests/build.sh builds it).
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
SNX=${SNX:-$("$here/tests/build.sh")} || { echo "FAIL build"; exit 1; }
t=$(mktemp -d) && trap 'rm -rf "$t"' EXIT
C=$t/cache fail=0
cat > "$t/a.nix" <<'EOF'
let b = import ./b.nix; in "${b} ${builtins.readFile ./c.txt} ${if builtins.pathExists ./d then "d" else "no-d"} ${builtins.getEnv "CV"}"
EOF
echo '"b1"' > "$t/b.nix"
printf c1 > "$t/c.txt"
export CV=v1

# run NAME WANT-OUTPUT WANT-EVENT [ARG...]: one run in $t; the event is hit, miss, store or skip (SNX_CACHE_DEBUG lines)
run() {
  local name=$1 want=$2 ev=$3; shift 3
  local out err
  out=$(cd "$t" && SNX_CACHE=$C SNX_CACHE_DEBUG=1 timeout 1 "$SNX" eval "$@" 2> "$t/err")
  err=$(grep -o 'snx-cache: [a-z]\+' "$t/err" | tr '\n' ' ')
  if [[ $out == "$want" && $err == *"snx-cache: $ev"* ]]; then echo "ok   $name: $out [$err]"
  else echo "FAIL $name: want $want ($ev) got $out [$err]"; fail=1; fi
}
f=$t/a.nix
run cold        '"b1 c1 no-d v1"' store "$f"
run warm        '"b1 c1 no-d v1"' hit   "$f"
echo '"b2"' > "$t/b.nix"
run edit-b      '"b2 c1 no-d v1"' miss  "$f"
run edit-b-warm '"b2 c1 no-d v1"' hit   "$f"
printf c2 > "$t/c.txt"
run edit-c      '"b2 c2 no-d v1"' miss  "$f"
run edit-c-warm '"b2 c2 no-d v1"' hit   "$f"
touch "$t/d"
run create-d    '"b2 c2 d v1"'    miss  "$f"
run create-d-warm '"b2 c2 d v1"'  hit   "$f"
export CV=v2
run env         '"b2 c2 d v2"'    miss  "$f"
run env-warm    '"b2 c2 d v2"'    hit   "$f"
run argv        '"b2 c2 d v2"'    store "$f" --strict
run argv-warm   '"b2 c2 d v2"'    hit   "$f" --strict
# same size, same content length: the stat shortcut must still see the edit (new ctime)
printf c3 > "$t/c.txt"
SNX_CACHE_STAT=1 run stat-edit-c '"b2 c3 d v2"' miss "$f"
# a corrupt entry is a miss, never a crash or a wrong answer
for e in "$C"/*; do printf X | dd of="$e" bs=1 seek=60 conv=notrunc 2>/dev/null; done
run corrupt     '"b2 c3 d v2"'    miss  "$f"
for e in "$C"/*; do head -c 30 "$e" > "$e.t" && mv "$e.t" "$e"; done
run truncated   '"b2 c3 d v2"'    miss  "$f"
echo 'builtins.currentTime > 0' > "$t/time.nix"
run time        'true'            skip  "$t/time.nix"
run time-again  'true'            skip  "$t/time.nix"
echo 'throw "no"' > "$t/bad.nix"
run failed      ''                skip  "$t/bad.nix"
# store roots: SNX_CACHE_STORE points at a fake store; a file under <store>/<hash>-<name> is trusted by its root's
# (dev, inode, ctime): an in-place edit inside the root is not seen, a replaced root (new inode) is a miss
mkdir -p "$t/store/h1-pkg"
echo '"s1"' > "$t/store/h1-pkg/x.nix"
echo "import $t/store/h1-pkg/x.nix" > "$t/s.nix"
export SNX_CACHE_STORE=$t/store
run store-cold    '"s1"' store "$t/s.nix"
run store-warm    '"s1"' hit   "$t/s.nix"
echo '"s2"' > "$t/store/h1-pkg/x.nix"
run store-trusted '"s1"' hit   "$t/s.nix"
rm -rf "$t/store/h1-pkg"; mkdir "$t/store/h1-pkg"; echo '"s3"' > "$t/store/h1-pkg/x.nix"
run store-replace '"s3"' miss  "$t/s.nix"
run store-replace-warm '"s3"' hit "$t/s.nix"
rm -rf "$t/store/h1-pkg"
run store-gone    ''     miss  "$t/s.nix"
unset SNX_CACHE_STORE
[[ $fail == 0 ]] && echo "cache: all pass" || echo "cache: FAILURES"
exit $fail
