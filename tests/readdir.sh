#!/usr/bin/env bash
# builtins.readDir: each entry's type is a readFileType thunk (primops.cc:2398-2414), so a listing whose types are never
# read stats no entry (strace), and the types read are the ones Nix gives, a symlink not followed.
# SNX = the snx binary (default: tests/build.sh builds it).
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
SNX=${SNX:-$("$here/tests/build.sh")} || { echo "FAIL build"; exit 1; }
ST=$(command -v strace || timeout 120 nix-shell -p strace --run 'command -v strace' 2>/dev/null | tail -1)
[ -x "${ST:-}" ] || { echo "FAIL readdir: strace missing"; exit 1; }
t=$(mktemp -d) && trap 'rm -rf "$t"' EXIT
mkdir "$t/rd" "$t/rd/d" && touch "$t/rd/a" && ln -s a "$t/rd/l"
echo 'builtins.attrNames (builtins.readDir ./rd)' > "$t/names.nix"
echo 'builtins.readDir ./rd' > "$t/types.nix"
# stats NAME: the entries of rd stat'ed while NAME.nix is evaluated (strict)
stats() { timeout 1 "$ST" -f -e trace=newfstatat,statx,lstat,stat -o "$t/st" "$SNX" eval "$t/$1.nix" --strict > "$t/$1.out"; grep -c "$t/rd/" "$t/st"; }
fail=0
n=$(stats names)
if [[ $(cat "$t/names.out") == '[ "a" "d" "l" ]' && $n == 0 ]]; then echo "ok   names: no entry stat"
else echo "FAIL names: $(cat "$t/names.out"), $n entry stats, want 0"; fail=1; fi
k=$(stats types)
if [[ $(cat "$t/types.out") == '{ a = "regular"; d = "directory"; l = "symlink"; }' && $k == 3 ]]; then echo "ok   types: $k entry stats"
else echo "FAIL types: $(cat "$t/types.out"), $k entry stats, want 3"; fail=1; fi
[[ $fail == 0 ]] && echo "readdir: all pass" || echo "readdir: FAILURES"
exit $fail
