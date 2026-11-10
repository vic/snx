#!/usr/bin/env bash
# lexer cases: build nx/lexmain.srs, run tests/lex/NAME.nix, compare with NAME.toks. Exit 1 on any FAIL.
# Each .toks is derived by hand from Nix 2.34.8 src/libexpr/lexer.l; the rule (line) each case tests:
#   keywords 125-134,153 · ops2 135-151 · id_dash 103 (ID takes -) · punct 343 {ANY} · int 154 · int_overflow 154 throw
#   float 105,166 (1. ok, 1e3 = INT ID, 01.5 = INT FLOAT) · float_overflow 166 ERANGE · str_escapes 46 unescapeStr
#   str_nested 176-186 state stack · str_dollar 189 ($ before ") 190 ($$ eats {) · str_unterminated_bs 199 EOF
#   str_unterminated EOF in DEFAULT · ind_basic 207 · ind_escapes 213,218,222,227 · ind_lone 214,228 · ind_dollar_dollar 208
#   ind_interp 226 · ind_open_nl 207 (\ *\n) · paths 262,270,294 · path_vs_div 107 vs 343 · path_interp 262,279,285
#   path_seg_dollar 233 · hpath_interp 234 · path_slash_eof 306 · path_slash 305 · spath 111,313 · uri 112,314
#   crlf 46,339,340 · comment_eof 340 · comment_long 326,341 · comment_unterminated 341 fails, 343 · lambda 343
#   ellipsis 135 · pipes 146 · any_char 343 · nul 343 (byte 0 = end of file) · brace_stack 178 · dots_path 107 vs 135
set -u
WS=$(cd "$(dirname "$0")/.." && pwd)
SRS=${SRS:-$HOME/hk/srs}
S2C=${S2C:-$XDG_RUNTIME_DIR/srs-build/s2c1}
OUT=${OUT:-$(mktemp -d)}; mkdir -p "$OUT"
( cd "$SRS" && timeout 25 "$S2C" lib/prelude.srs s2/coll.srs "$WS/nx/ast.srs" "$WS/nx/rt.srs" "$WS/nx/lex.srs" "$WS/nx/lexmain.srs" > "$OUT/lexmain.c" ) || { echo "FAIL build (s2c)"; exit 1; }
timeout 60 zig cc -std=gnu11 -O2 -w -o "$OUT/lexmain" "$OUT/lexmain.c" "$SRS/rt/srs_rt.c" "$WS/rt/snx_rt.c" || { echo "FAIL build (cc)"; exit 1; }
fail=0
for nix in "$WS"/tests/lex/*.nix; do
  name=$(basename "$nix" .nix)
  if timeout 1 "$OUT/lexmain" "$nix" > "$OUT/$name.out" 2>&1 && diff -u "${nix%.nix}.toks" "$OUT/$name.out" > "$OUT/$name.diff"; then
    echo "ok $name"
  else
    echo "FAIL $name"; cat "$OUT/$name.diff"; fail=1
  fi
done
exit $fail
