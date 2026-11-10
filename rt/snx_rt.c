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

// the bits of -d for the bits of a double d: the sign bit flipped (nx/tree.srs, SPEC.md rule 3)
int64_t Nx__treeFloatNeg(int64_t bits) {
  return (int64_t)((uint64_t)bits ^ 0x8000000000000000ULL);
}

// ── word arrays: env frames, set and list slots. One bump arena for the whole run, never freed.
static uint8_t* g_bump; static size_t g_bump_left;
static void* nx_words(int64_t n) {
  size_t sz = (size_t)(n > 0 ? n : 1) * sizeof(void*);
  if (sz > g_bump_left) { size_t cap = sz > (64u << 20) ? sz : (64u << 20); g_bump = nx_alloc((int64_t)cap); g_bump_left = cap; }
  void* p = g_bump; g_bump += sz; g_bump_left -= sz;
  return p;
}
void* Nx__arrNew(int64_t n) { return nx_words(n); }
void* Nx__aget(void** p, int64_t i) { return p[i]; }
int64_t Nx__aset(void** p, int64_t i, void* v) { p[i] = v; return 0; }
int64_t Nx__ageti(int64_t* p, int64_t i) { return p[i]; }
int64_t Nx__aseti(int64_t* p, int64_t i, int64_t v) { p[i] = v; return 0; }
void* Nx__parent(void** f) { return f[0]; }
void* Nx__frame(void* parent, int64_t n) { void** f = nx_words(n + 1); f[0] = parent; return f; }
void* Nx__noFrame(void) { return 0; }

// ── symbols: open-addressing table from bytes to ids, ids in first-seen order
static srs_str* g_names; static int64_t g_nsyms, g_names_cap;
static int64_t* g_tab; static int64_t g_tab_cap;
static uint64_t nx_hash(srs_str s) { uint64_t h = 1469598103934665603ull; for (int64_t i = 0; i < s.n; i++) { h ^= s.p[i]; h *= 1099511628211ull; } return h; }
static void nx_tab_grow(void) {
  int64_t cap = g_tab_cap ? g_tab_cap * 2 : 4096;
  int64_t* t = malloc((size_t)cap * sizeof *t);
  if (!t) { fputs("snx: out of memory\n", stderr); exit(70); }
  for (int64_t i = 0; i < cap; i++) t[i] = -1;
  for (int64_t id = 0; id < g_nsyms; id++) { uint64_t j = nx_hash(g_names[id]) & (uint64_t)(cap - 1); while (t[j] >= 0) j = (j + 1) & (uint64_t)(cap - 1); t[j] = id; }
  free(g_tab); g_tab = t; g_tab_cap = cap;
}
int64_t Nx__intern(srs_str s) {
  if (2 * (g_nsyms + 1) > g_tab_cap) nx_tab_grow();
  uint64_t j = nx_hash(s) & (uint64_t)(g_tab_cap - 1);
  for (; g_tab[j] >= 0; j = (j + 1) & (uint64_t)(g_tab_cap - 1)) { srs_str o = g_names[g_tab[j]]; if (o.n == s.n && memcmp(o.p, s.p, (size_t)s.n) == 0) return g_tab[j]; }
  if (g_nsyms == g_names_cap) { g_names_cap = g_names_cap ? g_names_cap * 2 : 4096; g_names = realloc(g_names, (size_t)g_names_cap * sizeof *g_names); if (!g_names) { fputs("snx: out of memory\n", stderr); exit(70); } }
  uint8_t* copy = nx_alloc(s.n); memcpy(copy, s.p, (size_t)s.n);
  g_names[g_nsyms] = (srs_str){copy, s.n};
  g_tab[j] = g_nsyms;
  return g_nsyms++;
}
srs_str Nx__symName(int64_t id) { return (id >= 0 && id < g_nsyms) ? g_names[id] : (srs_str){(const uint8_t*)"", 0}; }
int64_t Nx__findSym(const int64_t* syms, int64_t n, int64_t sym) {
  int64_t lo = 0, hi = n;
  while (lo < hi) { int64_t m = (lo + hi) >> 1; if (syms[m] < sym) lo = m + 1; else hi = m; }
  return (lo < n && syms[lo] == sym) ? lo : n;
}

// ── stack: evaluation recurses as deep as the Nix program; raise the stack limit to 4 GiB (or the hard limit) and
// re-exec once, since the main thread stack size is fixed at exec time
#include <sys/resource.h>
#include <unistd.h>
__attribute__((constructor)) static void nx_big_stack(int argc, char** argv) {
  struct rlimit r;
  const rlim_t want = (rlim_t)4 << 30;
  if (getrlimit(RLIMIT_STACK, &r) != 0 || r.rlim_cur == RLIM_INFINITY || r.rlim_cur >= want) return;
  if (getenv("SNX_STACK_SET")) return;
  r.rlim_cur = (r.rlim_max == RLIM_INFINITY || r.rlim_max >= want) ? want : r.rlim_max;
  if (setrlimit(RLIMIT_STACK, &r) != 0) return;
  setenv("SNX_STACK_SET", "1", 1);
  execv("/proc/self/exe", argv);
}
