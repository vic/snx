// snx runtime: C services bound by `extern def` in nx/rt.srs. A string is srs_str (bytes + length, no NUL).
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct { const uint8_t* p; int64_t n; } srs_str;

static uint8_t* nx_alloc(int64_t n) {
  uint8_t* p = malloc((size_t)(n > 0 ? n : 1));
  if (!p) { fputs("snx: out of memory\n", stderr); exit(70); }
  return p;
}

// a + b as a new string (program lifetime)
srs_str Nx__cat(srs_str a, srs_str b) {
  uint8_t* p = nx_alloc(a.n + b.n);
  memcpy(p, a.p, (size_t)a.n);
  memcpy(p + a.n, b.p, (size_t)b.n);
  return (srs_str){p, a.n + b.n};
}

// the one-byte string of c (0..255)
srs_str Nx__byte(int64_t c) {
  static uint8_t all[256];
  static bool init;
  if (!init) { for (int i = 0; i < 256; i++) all[i] = (uint8_t)i; init = true; }
  return (srs_str){all + (c & 255), 1};
}

// the IEEE 754 bits of the double that s spells (strtod, as Nix's lexer)
int64_t Nx__floatBits(srs_str s) {
  char buf[512];
  int64_t n = s.n < (int64_t)sizeof buf - 1 ? s.n : (int64_t)sizeof buf - 1;
  memcpy(buf, s.p, (size_t)n); buf[n] = 0;
  double d = strtod(buf, 0);
  int64_t bits;
  memcpy(&bits, &d, sizeof bits);
  return bits;
}

// true when the decimal digits of s do not fit an int64 (Nix: "invalid integer")
bool Nx__intOverflows(srs_str s) {
  char buf[512];
  if (s.n >= (int64_t)sizeof buf) return true;
  memcpy(buf, s.p, (size_t)s.n); buf[s.n] = 0;
  errno = 0;
  (void)strtoll(buf, 0, 10);
  return errno == ERANGE;
}

// the value of the decimal digits of s (call Nx.intOverflows first)
int64_t Nx__intValue(srs_str s) {
  char buf[512];
  int64_t n = s.n < (int64_t)sizeof buf - 1 ? s.n : (int64_t)sizeof buf - 1;
  memcpy(buf, s.p, (size_t)n); buf[n] = 0;
  return (int64_t)strtoll(buf, 0, 10);
}

// v as 16 lowercase hex digits, the bits unsigned (SPEC.md: (float 3ff8000000000000))
srs_str Nx__hex64(int64_t v) {
  uint8_t* p = nx_alloc(16);
  uint64_t u = (uint64_t)v;
  for (int i = 15; i >= 0; i--) { p[i] = "0123456789abcdef"[u & 15]; u >>= 4; }
  return (srs_str){p, 16};
}
