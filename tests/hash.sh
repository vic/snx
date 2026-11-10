#!/usr/bin/env bash
# hash known answers: md5 sha1 sha256 sha512 of "", "abc", 1,000,000 x "a" against the coreutils sums (the FIPS 180 /
# RFC 1321 vectors), then the encoders against real nix (nix32 of sha256 "", base64) and base64(1), and decoder round trips.
set -u
WS=$(cd "$(dirname "$0")/.." && pwd)
SRS=${SRS:-$HOME/hk/srs-main}
S2C=${S2C:-$("$(dirname "$0")/s2c.sh")}
OUT=${OUT:-$(mktemp -d)}; mkdir -p "$OUT"
( cd "$SRS" && timeout 25 "$S2C" lib/prelude.srs s2/coll.srs "$WS/nx/ast.srs" "$WS/nx/rt.srs" "$WS/nx/hash.srs" "$WS/nx/hashtest.srs" > "$OUT/hashtest.c" ) || { echo "FAIL build (s2c)"; exit 1; }
timeout 60 zig cc -std=gnu11 -O2 -w -o "$OUT/hashtest" "$OUT/hashtest.c" "$SRS/rt/srs_rt.c" "$WS/rt/snx_rt.c" "$WS/rt/snx_hash.c" -lm || { echo "FAIL build (cc)"; exit 1; }
fail=0
t() { # name want got
  if [[ $2 == "$3" ]]; then echo "ok $1"; else echo "FAIL $1: want $2 got $3"; fail=1; fi
}
printf '' > "$OUT/empty"; printf abc > "$OUT/abc"; head -c 1000000 /dev/zero | tr '\0' a > "$OUT/mil"
algos=(md5 sha1 sha256 sha512)
for f in empty abc mil; do
  for i in 0 1 2 3; do
    want=$(${algos[$i]}sum < "$OUT/$f" | cut -d' ' -f1)
    t "${algos[$i]} $f" "$want" "$(timeout 2 "$OUT/hashtest" hash $i "$OUT/$f")"
  done
done
# sha256 on both block paths (SHA extensions when the CPU has them, and the portable one), lengths around a block
for n in 55 56 63 64 65 119 120 128 1000; do head -c $n "$OUT/mil" > "$OUT/len$n"; done
for f in empty abc mil len55 len56 len63 len64 len65 len119 len120 len128 len1000; do
  want=$(sha256sum < "$OUT/$f" | cut -d' ' -f1)
  t "sha256 $f" "$want" "$(timeout 2 "$OUT/hashtest" hash 2 "$OUT/$f")"
  t "sha256 portable $f" "$want" "$(SNX_SHA_PORTABLE=1 timeout 2 "$OUT/hashtest" hash 2 "$OUT/$f")"
done
t "sha256 abc FIPS" ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad "$(timeout 2 "$OUT/hashtest" hash 2 "$OUT/abc")"
for f in empty abc; do
  hex=$(sha256sum < "$OUT/$f" | cut -d' ' -f1); printf "$(printf %s "$hex" | sed 's/../\\x&/g')" > "$OUT/raw"
  t "nix32 sha256 $f" "$(nix hash convert --hash-algo sha256 --to nix32 "$hex")" "$(timeout 2 "$OUT/hashtest" enc 32 "$OUT/raw")"
  t "base64 sha256 $f" "$(nix hash convert --hash-algo sha256 --to base64 "$hex")" "$(timeout 2 "$OUT/hashtest" enc 64 "$OUT/raw")"
  t "base16 sha256 $f" "$hex" "$(timeout 2 "$OUT/hashtest" enc 16 "$OUT/raw")"
  for k in 16 32 64; do
    timeout 2 "$OUT/hashtest" enc $k "$OUT/raw" | tr -d '\n' > "$OUT/e"
    t "roundtrip $k $f" "$hex" "$(timeout 2 "$OUT/hashtest" dec $k "$OUT/e")"
  done
done
printf 'hello world' > "$OUT/hw"
t "base64 hw" "$(base64 < "$OUT/hw")" "$(timeout 2 "$OUT/hashtest" enc 64 "$OUT/hw")"
printf 'e' > "$OUT/bad"; t "unb32 invalid char" invalid "$(timeout 2 "$OUT/hashtest" dec 32 "$OUT/bad")"
printf 'xyz' > "$OUT/bad"; t "unb16 odd" invalid "$(timeout 2 "$OUT/hashtest" dec 16 "$OUT/bad")"
exit $fail
