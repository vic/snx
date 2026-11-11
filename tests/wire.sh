#!/usr/bin/env bash
# wire narSize as an unsigned decimal (nx/wire.srs Nx.u64Str) at and above 2^63: the conformance case path-info with
# its narSize word replaced; the answer is std::to_string(uint64_t) of it, as Nix's client prints it.
set -u
CONF=${CONF:-$HOME/hk/nix-conformance}
SNX=${SNX:-$("$(dirname "$0")/build.sh")}
here=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
timeout 30 zig cc -O1 -w -o "$OUT/wire-serve" "$CONF/wire/serve.c" || { echo "FAIL build wire-serve"; exit 1; }
export PATH=$OUT:$PATH SNX
src=$CONF/wire/path-info
# narSize: the 8-byte little-endian word 1234 at offset 296 of path-info.after
[[ $(od -A n -t x1 -j 296 -N 8 "$src.after" | tr -d ' ') == d204000000000000 ]] || { echo "FAIL path-info.after moved"; exit 1; }
fail=0
t() { # name, the 8 bytes, the decimal
  cp "$src.bin" "$OUT/$1.bin"; cp "$src.op" "$OUT/$1.op"; cp "$src.after" "$OUT/$1.after"
  printf "$2" | dd of="$OUT/$1.after" bs=1 seek=296 conv=notrunc status=none
  local want got
  want=$(sed "s/size=1234/size=$3/" "$src.exp")
  got=$(timeout 5 "$CONF/wire/ask" "$here/adapter/snx-adapter" "$OUT/$1.op")
  if [[ $got == "$want" ]]; then echo "ok $1"; else echo "FAIL $1: got $got"; fail=1; fi
}
t max '\xff\xff\xff\xff\xff\xff\xff\xff' 18446744073709551615
t half '\x00\x00\x00\x00\x00\x00\x00\x80' 9223372036854775808
t odd '\x01\x00\x00\x00\x00\x00\x00\x80' 9223372036854775809
t ten '\x0a\x00\x00\x00\x00\x00\x00\x80' 9223372036854775818
t below '\xff\xff\xff\xff\xff\xff\xff\x7f' 9223372036854775807
exit $fail
