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

// s as 1 to 18 decimal digits (no sign, always in range: the lexer's INT tokens), or false
static bool nx_digits18(srs_str s, int64_t* v) {
  if (s.n < 1 || s.n > 18) return false;
  int64_t x = 0;
  for (int64_t i = 0; i < s.n; i++) { unsigned d = s.p[i] - (unsigned)'0'; if (d > 9) return false; x = 10 * x + d; }
  *v = x;
  return true;
}

// true when the decimal digits of s do not fit an int64 (Nix: "invalid integer")
bool Nx__intOverflows(srs_str s) {
  char buf[512];
  int64_t v;
  if (nx_digits18(s, &v)) return false;
  if (s.n >= (int64_t)sizeof buf) return true;
  memcpy(buf, s.p, (size_t)s.n); buf[s.n] = 0;
  errno = 0;
  (void)strtoll(buf, 0, 10);
  return errno == ERANGE;
}

// the value of the decimal digits of s (call Nx.intOverflows first)
int64_t Nx__intValue(srs_str s) {
  char buf[512];
  int64_t v;
  if (nx_digits18(s, &v)) return v;
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

// the bits of 0.0 - d for the bits of a double d, as the parse oracle folds a negated literal (nx/tree.srs, SPEC.md rule 3)
int64_t Nx__treeFloatNeg(int64_t bits) {
  double d; memcpy(&d, &bits, sizeof d);
  double r = 0.0 - d;
  int64_t out; memcpy(&out, &r, sizeof out);
  return out;
}

// ── word arrays: env frames, set and list slots. One bump arena for the whole run, never freed.
static uint8_t* g_bump; static size_t g_bump_left; static uint8_t* g_chunk; static uint8_t* g_pin;
static void* nx_words(int64_t n) {
  size_t sz = (size_t)(n > 0 ? n : 1) * sizeof(void*);
  if (sz > g_bump_left) { size_t cap = sz > (64u << 20) ? sz : (64u << 20); g_pin = g_chunk = g_bump = nx_alloc((int64_t)cap); g_bump_left = cap; }
  void* p = g_bump; g_bump += sz; g_bump_left -= sz;
  return p;
}
void* Nx__arrNew(int64_t n) { return nx_words(n); }
void* Nx__aget(void** p, int64_t i) { return p[i]; }
int64_t Nx__aset(void** p, int64_t i, void* v) { p[i] = v; return 0; }
int64_t Nx__ageti(int64_t* p, int64_t i) { return p[i]; }
int64_t Nx__aseti(int64_t* p, int64_t i, int64_t v) { p[i] = v; return 0; }
void* Nx__slotAddr(void** p, int64_t i) { return p + i; }
int64_t Nx__slotOfAddr(void** p, void** q) { return q - p; }
void* Nx__parent(void** f) { return f[0]; }
void* Nx__frame(void* parent, int64_t n) { void** f = nx_words(n + 1); f[0] = parent; return f; }
void* Nx__noFrame(void) { return 0; }
void* Nx__valPtr(void* v) { return v; }

// ── stack discipline for the word arena (nx/eval.srs Nx.apply): a lambda call that returns a scalar and let nothing
// escape gives back the words it took. An escape (a leak) = a pointer into the arena stored where older code can reach
// it (a forced cell taking a non-scalar value, an import or regex cache entry). A leak pins the arena below the bump
// pointer of that moment: a call whose mark is under the pin keeps its words. The bump pointer never drops under the
// pin (a release needs mark >= pin), so the pin stays meaningful after releases.

void* Nx__mark(void) { return g_bump; }
int64_t Nx__leak(void) { g_pin = g_bump; return 0; }
// back to mark m when m is in the current chunk (a call that opened a new chunk keeps it) and above the pin
// probe k (default off): SNX_PROBE=<bits>; bit 0 = no arena release, bit 1 = no numeric args at thunk time,
// bit 2 = trace forces of thunks re-entered while busy (bit 3: the whole force stack, not only the cycle).
// Built in only with SNX_CFLAGS=-DSNX_PROBES (tests/build.sh): else every probe is a constant 0 that LTO folds away
int64_t Nx__probe(int64_t k) {
#ifndef SNX_PROBES
  (void)k; return 0;
#endif
  static int64_t bits = -1;
  if (bits < 0) { const char* v = getenv("SNX_PROBE"); bits = v ? strtoll(v, 0, 0) : 0; }
  return (bits >> k) & 1;
}
// probe bit 2: a stack of the thunks being forced, each with the attribute name last selected or looked up when it
// started; meeting a busy thunk prints the chain from its entry up (the cycle)
srs_str Nx__symName(int64_t id);
static struct { void* c; int64_t sym; } g_tr[1 << 16]; static int64_t g_trn, g_trsym = -1;
int64_t Nx__trSym(int64_t sym) { g_trsym = sym; return 0; }
int64_t Nx__trPush(void* c) { if (g_trn < (1 << 16)) { g_tr[g_trn].c = c; g_tr[g_trn].sym = g_trsym; } g_trn++; return 0; }
int64_t Nx__trPop(void) { g_trn--; return 0; }
int64_t Nx__trBusy(void* c) {
  int64_t top = g_trn < (1 << 16) ? g_trn : (1 << 16), i = top - 1;
  while (i >= 0 && g_tr[i].c != c) i--;
  fprintf(stderr, "snx-probe: busy thunk %p re-entered, depth %lld, entry %lld; chain:\n", c, (long long)g_trn, (long long)i);
  return i < 0 ? 0 : i;
}
int64_t Nx__trTop(void) { return g_trn < (1 << 16) ? g_trn : (1 << 16); }
void* Nx__trCell(int64_t j) { return g_tr[j].c; }
int64_t Nx__trSymAt(int64_t j) { return g_tr[j].sym; }
// probe bit 4: a release poisons the words (0xa5 bytes) and keeps them, and logs the range with its sequence number.
// A crash on a poisoned word: `call Nx__relFind(addr)` in gdb names the release; SNX_RELSEQ=<n> makes release n
// return 1 (nx/eval.srs Nx.apply then prints the call and the force stack)
static struct { uint8_t *b, *e; } *g_rel; static int64_t g_reln, g_relcap;
int64_t Nx__relProbe(uint8_t* m) {
  if (!(m >= g_pin && m >= g_chunk && m < g_bump)) return 0;
  if (g_reln == g_relcap) { g_relcap = g_relcap ? 2 * g_relcap : 1 << 16; g_rel = realloc(g_rel, (size_t)g_relcap * sizeof *g_rel); }
  g_rel[g_reln].b = m; g_rel[g_reln].e = g_bump;
  memset(m, 0xa5, (size_t)(g_bump - m));
  static int64_t want = -2;
  if (want == -2) { const char* v = getenv("SNX_RELSEQ"); want = v ? strtoll(v, 0, 0) : -1; }
  return g_reln++ == want;
}
int64_t Nx__relFind(uint8_t* a) {
  for (int64_t i = g_reln - 1; i >= 0; i--) if (a >= g_rel[i].b && a < g_rel[i].e) { fprintf(stderr, "snx-probe: %p in release %lld [%p, %p)\n", a, (long long)i, g_rel[i].b, g_rel[i].e); return i; }
  fprintf(stderr, "snx-probe: %p in no release\n", a); return -1;
}
int64_t Nx__release(uint8_t* m) {
  if (Nx__probe(0)) return 0;
  if (m >= g_pin && m >= g_chunk && m <= g_bump) { g_bump_left += (size_t)(g_bump - m); g_bump = m; }
  return 0;
}

// ── symbols: open-addressing table from bytes to ids, ids in first-seen order
static srs_str* g_names; static int64_t g_nsyms, g_names_cap;
static int64_t* g_tab; static int64_t g_tab_cap;
// words, not bytes: 8-byte loads over the string, the last one overlapping its end (4-byte loads below 8 bytes, the
// bytes below 4), each multiplied in; the length is mixed in so overlaps of different lengths differ
static inline uint64_t nx_mix(uint64_t h, uint64_t w) { h = (h ^ w) * 0x9e3779b97f4a7c15ull; return h ^ (h >> 29); }
static uint64_t nx_hash(srs_str s) {
  const uint8_t* p = s.p; int64_t n = s.n; uint64_t h = 0x243f6a8885a308d3ull ^ (uint64_t)n, w; uint32_t a, b;
  if (n >= 8) {
    for (int64_t i = 0; i + 8 < n; i += 8) { memcpy(&w, p + i, 8); h = nx_mix(h, w); }
    memcpy(&w, p + n - 8, 8); h = nx_mix(h, w);
  } else if (n >= 4) { memcpy(&a, p, 4); memcpy(&b, p + n - 4, 4); h = nx_mix(h, (uint64_t)a << 32 | b); }
  else if (n > 0) h = nx_mix(h, (uint64_t)p[0] << 16 | (uint64_t)p[n >> 1] << 8 | p[n - 1]);
  return nx_mix(h, 0);
}
// n bytes at a and b equal: words as nx_hash loads them (an overlapping last word), no call for a short name
static inline bool nx_same(const uint8_t* a, const uint8_t* b, int64_t n) {
  uint64_t x, y; uint32_t u, v;
  if (n >= 8) {
    for (int64_t i = 0; i + 8 < n; i += 8) { memcpy(&x, a + i, 8); memcpy(&y, b + i, 8); if (x != y) return false; }
    memcpy(&x, a + n - 8, 8); memcpy(&y, b + n - 8, 8); return x == y;
  }
  if (n >= 4) { memcpy(&u, a, 4); memcpy(&v, b, 4); if (u != v) return false; memcpy(&u, a + n - 4, 4); memcpy(&v, b + n - 4, 4); return u == v; }
  for (int64_t i = 0; i < n; i++) if (a[i] != b[i]) return false;
  return true;
}
// a string's 64-bit hash (nx/parse.srs keys its attribute index by it; equal names are still compared)
int64_t Nx__strKey(srs_str s) { return (int64_t)nx_hash(s); }
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
  for (; g_tab[j] >= 0; j = (j + 1) & (uint64_t)(g_tab_cap - 1)) { srs_str o = g_names[g_tab[j]]; if (o.n == s.n && nx_same(o.p, s.p, s.n)) return g_tab[j]; }
  if (g_nsyms == g_names_cap) { g_names_cap = g_names_cap ? g_names_cap * 2 : 4096; g_names = realloc(g_names, (size_t)g_names_cap * sizeof *g_names); if (!g_names) { fputs("snx: out of memory\n", stderr); exit(70); } }
  uint8_t* copy = nx_alloc(s.n); memcpy(copy, s.p, (size_t)s.n);
  g_names[g_nsyms] = (srs_str){copy, s.n};
  g_tab[j] = g_nsyms;
  return g_nsyms++;
}
srs_str Nx__symName(int64_t id) { return (id >= 0 && id < g_nsyms) ? g_names[id] : (srs_str){(const uint8_t*)"", 0}; }
// per symbol: the cell of its name as a string (nx/prim.srs Nx.nameCell), Nx__noFrame until made
static void** g_namecell; static int64_t g_namecell_cap;
void* Nx__nameCellOf(int64_t id) { return id < g_namecell_cap ? g_namecell[id] : 0; }
int64_t Nx__nameCellSet(int64_t id, void* c) {
  if (id >= g_namecell_cap) {
    int64_t cap = g_namecell_cap ? g_namecell_cap : 4096; while (cap <= id) cap *= 2;
    g_namecell = realloc(g_namecell, (size_t)cap * sizeof *g_namecell);
    if (!g_namecell) { fputs("snx: out of memory\n", stderr); exit(70); }
    memset(g_namecell + g_namecell_cap, 0, (size_t)(cap - g_namecell_cap) * sizeof *g_namecell); g_namecell_cap = cap;
  }
  g_namecell[id] = c;
  return 0;
}
int64_t Nx__findSym(const int64_t* syms, int64_t n, int64_t sym) {
  int64_t lo = 0, hi = n;
  // halve down to eight names, then walk them: sorted, so the first not below sym is its place
  while (hi - lo > 8) { int64_t m = (lo + hi) >> 1; if (syms[m] < sym) lo = m + 1; else hi = m; }
  while (lo < hi && syms[lo] < sym) lo++;
  return (lo < n && syms[lo] == sym) ? lo : n;
}

// k sorted leaves (n, syms, vals) in t, newest first, merged by symbol into syms/vals; the newest leaf's cell wins a
// shared name. Selection over the k heads: k is at most 9 and the leaves above the base are short. Returns the count
static int64_t nx_kmerge(int64_t k, const int64_t* t, int64_t* syms, void** vals) {
  int64_t pos[16] = {0}, o = 0;
  for (;;) {
    int64_t best = INT64_MAX, bi = -1;
    for (int64_t i = 0; i < k; i++)
      if (pos[i] < t[3 * i]) { int64_t s = ((const int64_t*)t[3 * i + 1])[pos[i]]; if (s < best) { best = s; bi = i; } }
    if (bi < 0) return o;
    syms[o] = best; vals[o++] = ((void**)t[3 * bi + 2])[pos[bi]];
    for (int64_t i = bi; i < k; i++)
      if (pos[i] < t[3 * i] && ((const int64_t*)t[3 * i + 1])[pos[i]] == best) pos[i]++;
  }
}

// the leaves above the base (0..k-2 of t) merged into new arrays (nx/eval.srs Nx.compact); the unused name words go back
// The oldest of them (k-2, an earlier merged top) may be long: the short newer ones merge first, then the two
int64_t Nx__mergeTop(int64_t k, const int64_t* t, int64_t* out) {
  int64_t ns = 0;
  for (int64_t i = 0; i < k - 2; i++) ns += t[3 * i];
  int64_t stk[64]; void* stv[64];
  int64_t* ts = ns <= 64 ? stk : malloc((size_t)ns * sizeof *ts);
  void** tv = ns <= 64 ? stv : malloc((size_t)ns * sizeof *tv);
  if (!ts || !tv) { fputs("snx: out of memory\n", stderr); exit(70); }
  int64_t pair[6] = { nx_kmerge(k - 2, t, ts, tv), (int64_t)ts, (int64_t)tv, t[3 * (k - 2)], t[3 * (k - 2) + 1], t[3 * (k - 2) + 2] };
  int64_t ntop = pair[0] + pair[3];
  void** vals = nx_words(ntop);
  int64_t* syms = nx_words(ntop);
  int64_t o = nx_kmerge(2, pair, syms, vals);
  size_t back = (size_t)(ntop - o) * sizeof(void*);
  if ((uint8_t*)(syms + ntop) == g_bump) { g_bump -= back; g_bump_left += back; }
  out[0] = o; out[1] = (int64_t)syms; out[2] = (int64_t)vals;
  if (ts != stk) { free(ts); free(tv); }
  return 0;
}

// a layered set flat (nx/eval.srs Nx.flatLeaves): k >= 2 leaves as (n, syms, vals) triples in t, newest first. The
// leaves above the base merge first (short); when all their names are in the base, the result shares the base's name
// array and copies its cells with the newer ones written over, else base and the merged top merge into new arrays
int64_t Nx__mergeLeaves(int64_t k, const int64_t* t, int64_t* out) {
  int64_t nb = t[3 * (k - 1)], ntop = 0;
  const int64_t* sb = (const int64_t*)t[3 * (k - 1) + 1];
  void** vb = (void**)t[3 * (k - 1) + 2];
  for (int64_t i = 0; i < k - 1; i++) ntop += t[3 * i];
  int64_t stk[64]; void* stv[64];
  int64_t* ts = ntop <= 64 ? stk : malloc((size_t)ntop * sizeof *ts);
  void** tv = ntop <= 64 ? stv : malloc((size_t)ntop * sizeof *tv);
  if (!ts || !tv) { fputs("snx: out of memory\n", stderr); exit(70); }
  int64_t nt = nx_kmerge(k - 1, t, ts, tv);
  int64_t in = 0;
  while (in < nt && Nx__findSym(sb, nb, ts[in]) < nb) in++;
  if (in == nt) {
    void** vals = nx_words(nb);
    memcpy(vals, vb, (size_t)nb * sizeof *vals);
    for (int64_t j = 0; j < nt; j++) vals[Nx__findSym(sb, nb, ts[j])] = tv[j];
    out[0] = nb; out[1] = (int64_t)sb; out[2] = (int64_t)vals;
  } else {
    int64_t pair[6] = { nt, (int64_t)ts, (int64_t)tv, nb, (int64_t)sb, (int64_t)vb };
    void** vals = nx_words(nt + nb);
    int64_t* syms = nx_words(nt + nb);
    int64_t o = nx_kmerge(2, pair, syms, vals);
    // syms is the last allocation: its unused tail goes back
    size_t back = (size_t)(nt + nb - o) * sizeof(void*);
    if ((uint8_t*)(syms + nt + nb) == g_bump) { g_bump -= back; g_bump_left += back; }
    out[0] = o; out[1] = (int64_t)syms; out[2] = (int64_t)vals;
  }
  if (ts != stk) { free(ts); free(tv); }
  return 0;
}

// ── stack: evaluation recurses as deep as the Nix program; raise the stack limit to 4 GiB (or the hard limit) and
// re-exec once, since the main thread stack size is fixed at exec time
#include <sys/resource.h>
#include <unistd.h>
#include <malloc.h>
// the srs arenas take 1 MB chunks and a file's arena is freed once it is loaded: kept in the heap (no mmap per chunk,
// no trim), the next file's chunks reuse those pages instead of faulting in fresh ones
__attribute__((constructor)) static void nx_malloc_keep(void) { mallopt(M_MMAP_THRESHOLD, 64 << 20); mallopt(M_TRIM_THRESHOLD, 256 << 20); }
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

// ── cells: one word holding a value (nx/val.srs Nx.cell)
void* Nx__cell(void* v) { void** c = nx_words(1); *c = v; return c; }
void* Nx__get(void** c) { return *c; }
// a thunk is two words: its code with the low bit set (values and code are word-aligned, so a value never has it),
// then its env; forcing writes the value over the first word
void* Nx__lazy(void* env, void* code) { void** c = nx_words(2); c[0] = (void*)((uintptr_t)code | 1); c[1] = env; return c; }
bool Nx__isLazy(void** c) { return (uintptr_t)*c & 1; }
void* Nx__lazyCode(void** c) { return (void*)((uintptr_t)*c & ~(uintptr_t)1); }
int64_t Nx__put(void** c, void* v) { *c = v; return 0; }

// a double as std::ostream prints it by default (precision 6, %g): print-ambiguous.cc:93-95
srs_str Nx__floatShow(int64_t bits) {
  double d; memcpy(&d, &bits, sizeof d);
  char buf[64]; int n = snprintf(buf, sizeof buf, "%g", d);
  uint8_t* p = nx_alloc(n); memcpy(p, buf, (size_t)n);
  return (srs_str){p, n};
}

// ── eval core services (nx/eval.srs, nx/prim.srs, nx/print.srs)
#include <math.h>
#include <time.h>
#include <sys/stat.h>

// sixteen zeroed words for run-wide state (the base env, the primop table, the call depth): nx/base.srs
void* Nx__globals(void) { static void* g[20]; return g; }
bool Nx__ptrEq(void* a, void* b) { return a == b; }
// the n slots of frame f (a set built from a rec frame shares them)
void* Nx__slots(void** f) { return f + 1; }

// keys ascending, vals moved along; stable (insertion sort below 16, else merge sort through a scratch buffer)
static void nx_msort(int64_t* k, void** v, int64_t* tk, void** tv, int64_t n) {
  if (n < 16) {
    for (int64_t i = 1; i < n; i++) {
      int64_t kk = k[i]; void* vv = v[i]; int64_t j = i;
      while (j > 0 && k[j - 1] > kk) { k[j] = k[j - 1]; v[j] = v[j - 1]; j--; }
      k[j] = kk; v[j] = vv;
    }
    return;
  }
  int64_t h = n / 2;
  nx_msort(k, v, tk, tv, h); nx_msort(k + h, v + h, tk, tv, n - h);
  int64_t i = 0, j = h, o = 0;
  while (i < h && j < n) { if (k[j] < k[i]) { tk[o] = k[j]; tv[o++] = v[j++]; } else { tk[o] = k[i]; tv[o++] = v[i++]; } }
  while (i < h) { tk[o] = k[i]; tv[o++] = v[i++]; }
  while (j < n) { tk[o] = k[j]; tv[o++] = v[j++]; }
  memcpy(k, tk, (size_t)n * sizeof *k); memcpy(v, tv, (size_t)n * sizeof *v);
}
int64_t Nx__sortPairs(int64_t* keys, void** vals, int64_t n) {
  if (n < 2) return 0;
  int64_t* tk = n >= 16 ? malloc((size_t)n * sizeof *tk) : 0; void** tv = n >= 16 ? malloc((size_t)n * sizeof *tv) : 0;
  nx_msort(keys, vals, tk, tv, n);
  free(tk); free(tv);
  return 0;
}

// the indexes 0..n-1 of syms ordered by the bytes of their names (print-ambiguous.cc:36, attr-set.hh:420-430)
// each name keyed by its first eight bytes big-endian (zero padded): most pairs differ there, the rest compare bytes
typedef struct { uint64_t pre; int64_t sym, i; } nx_si;
static inline bool nx_name_less(const nx_si* a, const nx_si* b) {
  if (a->pre != b->pre) return a->pre < b->pre;
  srs_str x = g_names[a->sym], y = g_names[b->sym];
  int64_t m = x.n < y.n ? x.n : y.n;
  int c = memcmp(x.p, y.p, (size_t)m);
  return c ? c < 0 : x.n < y.n;
}
// merge sort on t[0..n) with tmp room of n: insertion below 16
static void nx_name_sort(nx_si* t, nx_si* tmp, int64_t n) {
  if (n < 16) {
    for (int64_t i = 1; i < n; i++) { nx_si x = t[i]; int64_t j = i; while (j > 0 && nx_name_less(&x, &t[j - 1])) { t[j] = t[j - 1]; j--; } t[j] = x; }
    return;
  }
  int64_t h = n / 2;
  nx_name_sort(t, tmp, h); nx_name_sort(t + h, tmp, n - h);
  int64_t i = 0, j = h, o = 0;
  while (i < h && j < n) tmp[o++] = nx_name_less(&t[j], &t[i]) ? t[j++] : t[i++];
  while (i < h) tmp[o++] = t[i++];
  memcpy(t, tmp, (size_t)o * sizeof *t);
}
void* Nx__byName(const int64_t* syms, int64_t n) {
  nx_si* t = malloc((size_t)(n > 0 ? 2 * n : 1) * sizeof *t);
  if (!t) { fputs("snx: out of memory\n", stderr); exit(70); }
  for (int64_t i = 0; i < n; i++) {
    srs_str s = g_names[syms[i]];
    uint64_t pre = 0;
    for (int64_t k = 0; k < 8; k++) pre = pre << 8 | (k < s.n ? s.p[k] : 0);
    t[i].pre = pre; t[i].sym = syms[i]; t[i].i = i;
  }
  nx_name_sort(t, t + n, n);
  int64_t* s = nx_words(n);
  for (int64_t i = 0; i < n; i++) s[i] = t[i].i;
  free(t);
  return s;
}
// a < b, bytes
bool Nx__strLess(srs_str a, srs_str b) {
  int64_t m = a.n < b.n ? a.n : b.n;
  int c = memcmp(a.p, b.p, (size_t)m);
  return c ? c < 0 : a.n < b.n;
}

// a set of pointers: open addressing, never shrinks (print-ambiguous.cc:33,49 and forceValueDeep)
typedef struct { void** t; int64_t cap, n; } nx_set;
void* Nx__seenNew(void) { nx_set* s = calloc(1, sizeof *s); s->cap = 64; s->t = calloc(64, sizeof(void*)); return s; }
static uint64_t nx_ph(void* p) { uint64_t h = (uint64_t)(uintptr_t)p; h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ull; return h ^ (h >> 32); }
bool Nx__seenAdd(nx_set* s, void* p) {
  if (2 * (s->n + 1) > s->cap) {
    int64_t cap = s->cap * 2; void** t = calloc((size_t)cap, sizeof(void*));
    for (int64_t i = 0; i < s->cap; i++) if (s->t[i]) { uint64_t j = nx_ph(s->t[i]) & (uint64_t)(cap - 1); while (t[j]) j = (j + 1) & (uint64_t)(cap - 1); t[j] = s->t[i]; }
    free(s->t); s->t = t; s->cap = cap;
  }
  uint64_t j = nx_ph(p) & (uint64_t)(s->cap - 1);
  for (; s->t[j]; j = (j + 1) & (uint64_t)(s->cap - 1)) if (s->t[j] == p) return false;
  s->t[j] = p; s->n++;
  return true;
}

// integers: wrapping results and their overflow tests (NixInt::valueChecked; primops.cc:4286,4317,4348)
int64_t Nx__iadd(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }
int64_t Nx__isub(int64_t a, int64_t b) { return (int64_t)((uint64_t)a - (uint64_t)b); }
int64_t Nx__imul(int64_t a, int64_t b) { return (int64_t)((uint64_t)a * (uint64_t)b); }
bool Nx__addOv(int64_t a, int64_t b) { int64_t r; return __builtin_add_overflow(a, b, &r); }
bool Nx__subOv(int64_t a, int64_t b) { int64_t r; return __builtin_sub_overflow(a, b, &r); }
bool Nx__mulOv(int64_t a, int64_t b) { int64_t r; return __builtin_mul_overflow(a, b, &r); }
// a / b truncated; call only with b != 0 and not (INT64_MIN, -1)
int64_t Nx__idiv(int64_t a, int64_t b) { return a / b; }
// prim_bitAnd, prim_bitOr, prim_bitXor
int64_t Nx__iand(int64_t a, int64_t b) { return a & b; }
int64_t Nx__ior(int64_t a, int64_t b) { return a | b; }
int64_t Nx__ixor(int64_t a, int64_t b) { return a ^ b; }
// doubles on their bits
static double nx_d(int64_t b) { double d; memcpy(&d, &b, sizeof d); return d; }
static int64_t nx_b(double d) { int64_t b; memcpy(&b, &d, sizeof b); return b; }
int64_t Nx__fadd(int64_t a, int64_t b) { return nx_b(nx_d(a) + nx_d(b)); }
int64_t Nx__fsub(int64_t a, int64_t b) { return nx_b(nx_d(a) - nx_d(b)); }
int64_t Nx__fmul(int64_t a, int64_t b) { return nx_b(nx_d(a) * nx_d(b)); }
int64_t Nx__fdiv(int64_t a, int64_t b) { return nx_b(nx_d(a) / nx_d(b)); }
bool Nx__flt(int64_t a, int64_t b) { return nx_d(a) < nx_d(b); }
bool Nx__feq(int64_t a, int64_t b) { return nx_d(a) == nx_d(b); }
int64_t Nx__fOfInt(int64_t i) { return nx_b((double)i); }
static srs_str nx_fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static srs_str nx_fmt(const char* f, ...) {
  char buf[512]; va_list ap; va_start(ap, f); int n = vsnprintf(buf, sizeof buf, f, ap); va_end(ap);
  if (n < 0) n = 0; if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
  uint8_t* p = nx_alloc(n); memcpy(p, buf, (size_t)n);
  return (srs_str){p, n};
}
srs_str Nx__intStr(int64_t i) { return nx_fmt("%lld", (long long)i); }
// std::to_string(double) = "%f" (coerceToString, eval.cc: toString of a float)
srs_str Nx__floatFixed(int64_t bits) { return nx_fmt("%f", nx_d(bits)); }

// string builder
typedef struct { uint8_t* p; int64_t n, cap; } nx_sb;
void* Nx__sbNew(void) { nx_sb* b = calloc(1, sizeof *b); return b; }
// room for k more bytes in b
static void nx_sbRoom(nx_sb* b, int64_t k) {
  if (b->n + k <= b->cap) return;
  int64_t cap = b->cap ? b->cap : 64; while (cap < b->n + k) cap *= 2;
  b->p = realloc(b->p, (size_t)cap); if (!b->p) { fputs("snx: out of memory\n", stderr); exit(70); } b->cap = cap;
}
int64_t Nx__sbAdd(nx_sb* b, srs_str s) {
  nx_sbRoom(b, s.n);
  memcpy(b->p + b->n, s.p, (size_t)s.n); b->n += s.n;
  return 0;
}
// the one reusable builder (snx is single-threaded), emptied on each call: one user at a time
static nx_sb nx_scratch;
srs_str Nx__sbGet(nx_sb* b) { int64_t n = b->n; if (b == &nx_scratch || n == 0) { uint8_t* p = nx_alloc(n); memcpy(p, b->p, (size_t)n); return (srs_str){p, n}; } uint8_t* p = realloc(b->p, (size_t)n); if (!p) { fputs("snx: out of memory\n", stderr); exit(70); } *b = (nx_sb){0}; return (srs_str){p, n}; }
void* Nx__sbScratch(void) { nx_scratch.n = 0; return &nx_scratch; }
// the bytes of b without a copy, valid until b is next changed
srs_str Nx__sbView(nx_sb* b) { return (srs_str){b->p, b->n}; }

// s quoted as print.cc:32-62 printLiteralString: \" \\ \n \r \t \${, all else verbatim
srs_str Nx__escStr(srs_str s) {
  uint8_t* p = nx_alloc(2 * s.n + 2); int64_t o = 0;
  p[o++] = '"';
  for (int64_t i = 0; i < s.n; i++) {
    uint8_t c = s.p[i];
    if (c == '"' || c == '\\') { p[o++] = '\\'; p[o++] = c; }
    else if (c == '\n') { p[o++] = '\\'; p[o++] = 'n'; }
    else if (c == '\r') { p[o++] = '\\'; p[o++] = 'r'; }
    else if (c == '\t') { p[o++] = '\\'; p[o++] = 't'; }
    else if (c == '$' && i + 1 < s.n && s.p[i + 1] == '{') { p[o++] = '\\'; p[o++] = '$'; }
    else p[o++] = c;
  }
  p[o++] = '"';
  return (srs_str){p, o};
}

static char* nx_cstr(srs_str s) { char* c = (char*)nx_alloc(s.n + 1); memcpy(c, s.p, (size_t)s.n); c[s.n] = 0; return c; }
// the bytes of the file at path; length -1 when it cannot be read
srs_str Nx__readFile(srs_str path) {
  char* c = nx_cstr(path);
  FILE* f = fopen(c, "rb"); free(c);
  if (!f) return (srs_str){(const uint8_t*)"", -1};
  nx_sb b = {0};
  uint8_t buf[65536]; size_t k;
  while ((k = fread(buf, 1, sizeof buf, f)) > 0) Nx__sbAdd(&b, (srs_str){buf, (int64_t)k});
  int bad = ferror(f); fclose(f);
  if (bad) { free(b.p); return (srs_str){(const uint8_t*)"", -1}; }
  srs_str r = Nx__sbGet(&b); free(b.p);
  return r;
}
bool Nx__isDir(srs_str path) { char* c = nx_cstr(path); struct stat st; bool r = stat(c, &st) == 0 && S_ISDIR(st.st_mode); free(c); return r; }
// the variable's value, "" when unset (prim_getEnv)
srs_str Nx__getEnv(srs_str name) {
  char* c = nx_cstr(name); const char* v = getenv(c); free(c);
  if (!v) return (srs_str){(const uint8_t*)"", 0};
  return (srs_str){(const uint8_t*)v, (int64_t)strlen(v)};
}
int64_t Nx__eprint(srs_str s) { fwrite(s.p, 1, (size_t)s.n, stderr); return 0; }
int64_t Nx__timeNow(void) { return (int64_t)time(0); }

// an absolute path with . and .. resolved and repeated or trailing slashes dropped (CanonPath)
srs_str Nx__canonPath(srs_str s) {
  uint8_t* p = nx_alloc(s.n + 1); int64_t o = 0, i = 0;
  while (i < s.n) {
    while (i < s.n && s.p[i] == '/') i++;
    int64_t b = i;
    while (i < s.n && s.p[i] != '/') i++;
    int64_t len = i - b;
    if (len == 0 || (len == 1 && s.p[b] == '.')) continue;
    if (len == 2 && s.p[b] == '.' && s.p[b + 1] == '.') { while (o > 0 && p[o - 1] != '/') o--; if (o > 0) o--; continue; }
    p[o++] = '/'; memcpy(p + o, s.p + b, (size_t)len); o += len;
  }
  if (o == 0) p[o++] = '/';
  return (srs_str){p, o};
}
// the words of a from word i on
void* Nx__wordsFrom(void** a, int64_t i) { return a + i; }
// an attribute name as print-ambiguous prints it: raw when isVarName, else quoted (print.cc:83-136 printAttributeName;
// the 2.34.8 binary quotes, the oracle .exp files agree)
srs_str Nx__attrName(srs_str s) {
  static const char* kw[] = {"if", "then", "else", "assert", "with", "let", "in", "rec", "inherit"};
  bool ok = s.n > 0;
  for (size_t k = 0; ok && k < sizeof kw / sizeof *kw; k++) if ((int64_t)strlen(kw[k]) == s.n && memcmp(kw[k], s.p, (size_t)s.n) == 0) ok = false;
  if (ok) { uint8_t c = s.p[0]; if ((c >= '0' && c <= '9') || c == '-' || c == '\'') ok = false; }
  for (int64_t i = 0; ok && i < s.n; i++) { uint8_t c = s.p[i]; if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '\'')) ok = false; }
  return ok ? s : Nx__escStr(s);
}

// nx/ast.srs Nx.lineCol: a line ends at \n, \r\n or a lone \r; the column counts bytes from 1, also past the end of src
int64_t Nx__lcLine(srs_str s, int64_t p) {
  int64_t line = 1, e = p < s.n ? p : s.n;
  for (int64_t i = 0; i < e; i++) {
    uint8_t c = s.p[i];
    if (c > '\r') continue;
    if (c == '\n') line++;
    else if (c == '\r') { if (i + 1 < e && s.p[i + 1] == '\n') i++; line++; }
  }
  return line;
}
// back to the last line end before p (a \r right before p ends a line there too)
int64_t Nx__lcCol(srs_str s, int64_t p) {
  int64_t j = (p < s.n ? p : s.n) - 1;
  while (j >= 0 && s.p[j] != '\n' && s.p[j] != '\r') j--;
  return p - j;
}

// ATerm strings (derivations.cc): printUnquotedString = s in quotes; printString also escapes " \ newline CR tab
int64_t Nx__sbAddQ(nx_sb* b, srs_str s) {
  nx_sbRoom(b, s.n + 2);
  b->p[b->n] = '"'; memcpy(b->p + b->n + 1, s.p, (size_t)s.n); b->p[b->n + 1 + s.n] = '"'; b->n += s.n + 2;
  return 0;
}
// the high bit set in some byte of w when one of its bytes is " or \ or below 14 (hasless, exact when none is)
static inline uint64_t nx_atermSpecial(uint64_t w) {
  const uint64_t ones = 0x0101010101010101ull, high = 0x8080808080808080ull;
  uint64_t q = w ^ (ones * '"'), s = w ^ (ones * '\\');
  return (((q - ones) & ~q) | ((s - ones) & ~s) | ((w - ones * 14) & ~w)) & high;
}
int64_t Nx__sbAddEsc(nx_sb* b, srs_str s) {
  nx_sbRoom(b, 2 * s.n + 2);
  uint8_t* o = b->p + b->n; *o++ = '"';
  for (int64_t i = 0; i < s.n;) {
    // eight bytes at a time: the plain ones before the first special byte (the lowest flagged byte is a true one) go
    // out in one store, which the 2n + 2 bytes of room cover
    if (i + 8 <= s.n) {
      uint64_t w, m;
      memcpy(&w, s.p + i, 8);
      memcpy(o, &w, 8);
      if (!(m = nx_atermSpecial(w))) { o += 8; i += 8; continue; }
      o += __builtin_ctzll(m) >> 3; i += __builtin_ctzll(m) >> 3;
    }
    uint8_t c = s.p[i++];
    if (c == '"' || c == '\\') { *o++ = '\\'; *o++ = c; }
    else if (c == '\n') { *o++ = '\\'; *o++ = 'n'; }
    else if (c == '\r') { *o++ = '\\'; *o++ = 'r'; }
    else if (c == '\t') { *o++ = '\\'; *o++ = 't'; }
    else *o++ = c;
  }
  *o++ = '"'; b->n = o - b->p;
  return 0;
}

// ── lexer byte classes and runs (nx/lex.srs): one table lookup a byte instead of a chain of compares. A class bit per
// flex class; -1 (past the end, Str.at) and NUL are in none
// NX_IDPC: in both ID and PATH_CHAR (one run serves both)
enum { NX_ALPHA = 1, NX_DIGIT = 2, NX_ID = 4, NX_PC = 8, NX_SCHEME = 16, NX_URI = 32, NX_WS = 64, NX_IDPC = 128 };
static uint8_t nx_cls[256];
__attribute__((constructor)) static void nx_cls_init(void) {
  for (int c = 0; c < 256; c++) {
    bool a = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'), d = c >= '0' && c <= '9';
    uint8_t k = 0;
    if (a) k |= NX_ALPHA;
    if (d) k |= NX_DIGIT;
    if (a || d || c == '_' || c == '\'' || c == '-') k |= NX_ID;
    if (a || d || c == '.' || c == '_' || c == '-' || c == '+') k |= NX_PC;
    if (a || d || c == '+' || c == '-' || c == '.') k |= NX_SCHEME;
    if (a || d || (c && strchr("%/?:@&=+$,-_.!~*'", c))) k |= NX_URI;
    if (c == ' ' || c == 9 || c == 13 || c == '\n') k |= NX_WS;
    if (a || d || c == '_' || c == '-') k |= NX_IDPC;
    nx_cls[c] = k;
  }
}
static inline bool nx_is(int64_t c, uint8_t k) { return c >= 0 && c < 256 && (nx_cls[c] & k); }
// SWAR: the bytes of word w equal to b, as the high bit of each (exact for the lowest one, which is all that is read)
#define NX_ONES 0x0101010101010101ull
static inline uint64_t nx_zeros(uint64_t v) { return (v - NX_ONES) & ~v & (NX_ONES << 7); }
static inline uint64_t nx_eq8(uint64_t w, uint8_t b) { return nx_zeros(w ^ (NX_ONES * b)); }
// the end of the run of class k in s from i (0 <= i): four bytes a bound check
static inline int64_t nx_run(srs_str s, int64_t i, uint8_t k) {
  for (; i + 4 <= s.n; i += 4) {
    if (!(nx_cls[s.p[i]] & k)) return i;
    if (!(nx_cls[s.p[i + 1]] & k)) return i + 1;
    if (!(nx_cls[s.p[i + 2]] & k)) return i + 2;
    if (!(nx_cls[s.p[i + 3]] & k)) return i + 3;
  }
  while (i < s.n && (nx_cls[s.p[i]] & k)) i++;
  return i;
}
// the end of the whitespace run from i: spaces 8 bytes a word (indentation), the other bytes one by one
static inline int64_t nx_wsRun(srs_str s, int64_t i) {
  for (;;) {
    for (uint64_t w; i + 8 <= s.n; i += 8) {
      memcpy(&w, s.p + i, 8);
      if ((w ^= 0x2020202020202020ull)) { i += __builtin_ctzll(w) >> 3; break; }
    }
    if (i < s.n && (nx_cls[s.p[i]] & NX_WS)) i++;
    else return i;
  }
}
// the end of a # comment's line: at \r, \n or the end
static int64_t nx_lineEnd(srs_str s, int64_t i) {
  for (uint64_t w, m; i + 8 <= s.n; i += 8) {
    memcpy(&w, s.p + i, 8);
    if ((m = nx_eq8(w, '\n') | nx_eq8(w, 13))) return i + (__builtin_ctzll(m) >> 3);
  }
  while (i < s.n && s.p[i] != '\n' && s.p[i] != 13) i++;
  return i;
}
// the end of a /* comment whose body starts at j (after the */), or i when it does not close
static int64_t nx_commentEnd(srs_str s, int64_t j, int64_t i) {
  for (; j >= 0 && j + 1 < s.n; j++) if (s.p[j] == '*' && s.p[j + 1] == '/') return j + 2;
  return i;
}

// ── word buffer (nx/load.srs): a file's compiled code written out as words before its arena is freed, then read back
// in order. malloc'd, freed by Nx.wbFree
typedef struct { int64_t* w; int64_t n, cap, rd; } nx_wb;
void* Nx__wbNew(void) { nx_wb* b = calloc(1, sizeof *b); if (!b) { fputs("snx: out of memory\n", stderr); exit(70); } return b; }
int64_t Nx__wbPut(nx_wb* b, int64_t x) {
  if (b->n == b->cap) { b->cap = b->cap ? 2 * b->cap : 1024; b->w = realloc(b->w, (size_t)b->cap * sizeof *b->w); if (!b->w) { fputs("snx: out of memory\n", stderr); exit(70); } }
  b->w[b->n++] = x;
  return 0;
}
int64_t Nx__wbGet(nx_wb* b) { return b->w[b->rd++]; }
int64_t Nx__wbPutP(nx_wb* b, void* p) { return Nx__wbPut(b, (int64_t)p); }
void* Nx__wbGetP(nx_wb* b) { return (void*)b->w[b->rd++]; }
// a string as its two words: the bytes are not in an arena (source text, malloc'd or static)
int64_t Nx__wbPutS(nx_wb* b, srs_str s) { Nx__wbPut(b, (int64_t)s.p); return Nx__wbPut(b, s.n); }
srs_str Nx__wbGetS(nx_wb* b) { const uint8_t* p = (const uint8_t*)b->w[b->rd++]; return (srs_str){p, b->w[b->rd++]}; }
int64_t Nx__wbFree(nx_wb* b) { free(b->w); free(b); return 0; }

// string runs (nx/lex.srs <STRING>, <IND_STRING>): ordinary bytes are skipped by a table test, the bytes that end or
// escape a run are handled as the flex rules do
enum { NX_SSTR = 1, NX_SIND = 2, NX_SNPLAIN = 4 };
static uint8_t nx_scls[256];
__attribute__((constructor)) static void nx_scls_init(void) {
  nx_scls['"'] |= NX_SSTR; nx_scls['\\'] |= NX_SSTR | NX_SNPLAIN; nx_scls['$'] |= NX_SSTR | NX_SIND;
  nx_scls['\''] |= NX_SIND; nx_scls[13] |= NX_SNPLAIN; nx_scls[0] |= NX_SNPLAIN;
}
static inline int nx_at(srs_str s, int64_t i) { return (uint64_t)i < (uint64_t)s.n ? s.p[i] : -1; }
// <STRING>: the first STRING rule's match from i: the run ([^\$\"\\]|\$[^\{\"\\]|\\{ANY}|\$\\{ANY})+, then a $ right
// before " when the run ends there. As end * 2 + 1 when the match has a backslash, CR or NUL (unescapeStr changes
// it or checks it), else end * 2
int64_t Nx__lxStrPart(srs_str s, int64_t i) {
  int64_t np = 0;
  for (;;) {
    for (uint64_t w, m; i + 8 <= s.n; i += 8) {
      memcpy(&w, s.p + i, 8);
      if ((m = nx_eq8(w, '"') | nx_eq8(w, '\\') | nx_eq8(w, '$') | nx_eq8(w, 13) | nx_zeros(w))) { i += __builtin_ctzll(m) >> 3; break; }
    }
    while (i < s.n && !(nx_scls[s.p[i]] & (NX_SSTR | NX_SNPLAIN))) i++;
    int c = nx_at(s, i), d = nx_at(s, i + 1);
    if (c == -1 || c == '"') break;
    if (c == 13 || c == 0) { np = 1; i++; }
    else if (c == '\\') { if (d == -1) break; np = 1; i += 2; }
    else if (d == '\\') { if (nx_at(s, i + 2) == -1) break; np = 1; i += 3; }
    else if (d == -1 || d == '{' || d == '"') break;
    else { if (d == 13 || d == 0) np = 1; i += 2; }
  }
  if (nx_at(s, i) == '$' && nx_at(s, i + 1) == '"') i++;
  return i << 1 | np;
}
// <IND_STRING>: the run ([^\$\']|\$[^\{\']|\'[^\'\$])+ from i
int64_t Nx__lxIndRun(srs_str s, int64_t i) {
  if (i < 0) return i;
  for (;;) {
    for (uint64_t w, m; i + 8 <= s.n; i += 8) {
      memcpy(&w, s.p + i, 8);
      if ((m = nx_eq8(w, '$') | nx_eq8(w, '\''))) { i += __builtin_ctzll(m) >> 3; break; }
    }
    while (i < s.n && !(nx_scls[s.p[i]] & NX_SIND)) i++;
    int c = nx_at(s, i), d = nx_at(s, i + 1);
    if (c == -1) return i;
    if (c == '$' && (d == -1 || d == '{' || d == '\'')) return i;
    if (c == '\'' && (d == -1 || d == '\'' || d == '$')) return i;
    i += 2;
  }
}
// a NUL byte in [i, e)
bool Nx__lxAnyNul(srs_str s, int64_t i, int64_t e) {
  if (i < 0) i = 0;
  if (e > s.n) e = s.n;
  return e > i && memchr(s.p + i, 0, (size_t)(e - i)) != NULL;
}

// ── one DEFAULT lexeme (nx/lex.srs Nx.lxGo): the rule flex picks at i (the longest match, the earlier rule on a tie)
// and the end of its match, in one pass. The rule codes are the order of Nx.LxRule (nx/lex.srs Nx.lxRuleOf)
enum { NX_R_ANY, NX_R_OP, NX_R_ID, NX_R_INT, NX_R_FLOAT, NX_R_DOLLAR, NX_R_CLOSE, NX_R_OPEN, NX_R_QUOTE, NX_R_INDOPEN,
       NX_R_SEGDOLLAR, NX_R_PATH, NX_R_SPATH, NX_R_URI, NX_R_SKIP };
// == != <= >= && || -> // ++ <| |>
static inline bool nx_op2(int a, int b) {
  switch (a) {
  case '=': case '!': case '<': case '>': return b == '=' || (a == '<' && b == '|');
  case '&': return b == '&';
  case '|': return b == '|' || b == '>';
  case '-': return b == '>';
  case '/': return b == '/';
  case '+': return b == '+';
  default: return false;
  }
}
// ... and the two-byte operators
static int64_t nx_opEnd(srs_str s, int64_t i) {
  int c = nx_at(s, i), d = nx_at(s, i + 1);
  if (c == '.' && d == '.' && nx_at(s, i + 2) == '.') return i + 3;
  return nx_op2(c, d) ? i + 2 : i;
}
// FLOAT (([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?
static int64_t nx_floatEnd(srs_str s, int64_t i) {
  int c = nx_at(s, i);
  int64_t m;
  if (c >= '1' && c <= '9') { int64_t d = nx_run(s, i, NX_DIGIT); m = nx_at(s, d) == '.' ? nx_run(s, d + 1, NX_DIGIT) : i; }
  else if (c == '0' && nx_at(s, i + 1) == '.' && nx_is(nx_at(s, i + 2), NX_DIGIT)) m = nx_run(s, i + 2, NX_DIGIT);
  else if (c == '.' && nx_is(nx_at(s, i + 1), NX_DIGIT)) m = nx_run(s, i + 1, NX_DIGIT);
  else m = i;
  if (m == i) return i;
  int e = nx_at(s, m), sg = nx_at(s, m + 1);
  int64_t k = sg == '+' || sg == '-' ? m + 2 : m + 1;
  return (e == 'e' || e == 'E') && nx_is(nx_at(s, k), NX_DIGIT) ? nx_run(s, k, NX_DIGIT) : m;
}
// (\/{PATH_CHAR}+)+\/? from j, or i when there is none
static int64_t nx_segs(srs_str s, int64_t j, int64_t i) {
  if (!(nx_at(s, j) == '/' && nx_is(nx_at(s, j + 1), NX_PC))) return i;
  j = nx_run(s, j + 1, NX_PC);
  while (nx_at(s, j) == '/' && nx_is(nx_at(s, j + 1), NX_PC)) j = nx_run(s, j + 1, NX_PC);
  return nx_at(s, j) == '/' ? j + 1 : j;
}
// PATH {PATH_CHAR}*(\/{PATH_CHAR}+)+\/? ; HPATH \~(\/{PATH_CHAR}+)+\/?
static int64_t nx_pathEnd(srs_str s, int64_t i) { return nx_segs(s, nx_run(s, i, NX_PC), i); }
static int64_t nx_hpathEnd(srs_str s, int64_t i) { return nx_at(s, i) == '~' ? nx_segs(s, i + 1, i) : i; }
// PATH_SEG {PATH_CHAR}*\/
static int64_t nx_segEnd(srs_str s, int64_t i) { int64_t j = nx_run(s, i, NX_PC); return nx_at(s, j) == '/' ? j + 1 : i; }
// {PATH_SEG}\$\{ | {HPATH_START}\$\{ (the whole match, ${ included)
static int64_t nx_segDollarEnd(srs_str s, int64_t i) {
  int64_t j = nx_at(s, i) == '~' && nx_at(s, i + 1) == '/' ? i + 2 : nx_segEnd(s, i);
  return j > i && nx_at(s, j) == '$' && nx_at(s, j + 1) == '{' ? j + 2 : i;
}
// SPATH \<{PATH_CHAR}+(\/{PATH_CHAR}+)*\>
static int64_t nx_spathEnd(srs_str s, int64_t i) {
  if (!(nx_at(s, i) == '<' && nx_is(nx_at(s, i + 1), NX_PC))) return i;
  int64_t j = nx_run(s, i + 1, NX_PC);
  while (nx_at(s, j) == '/' && nx_is(nx_at(s, j + 1), NX_PC)) j = nx_run(s, j + 1, NX_PC);
  return nx_at(s, j) == '>' ? j + 1 : i;
}
// URI [a-zA-Z][a-zA-Z0-9\+\-\.]*\:[a-zA-Z0-9\%\/\?\:\@\&\=\+\$\,\-\_\.\!\~\*\']+
static int64_t nx_uriEnd(srs_str s, int64_t i) {
  if (!nx_is(nx_at(s, i), NX_ALPHA)) return i;
  int64_t j = nx_run(s, i + 1, NX_SCHEME);
  return nx_at(s, j) == ':' && nx_is(nx_at(s, j + 1), NX_URI) ? nx_run(s, j + 1, NX_URI) : i;
}
// the first of the candidate rules (in file order) whose end is the largest, as end * 16 + rule
static inline int64_t nx_pick(int n, const int64_t* e, const int* r) {
  int b = 0;
  for (int k = 1; k < n; k++) if (e[k] > e[b]) b = k;
  return e[b] << 4 | r[b];
}
// the rule at i (i < s.n) and the end of its match, as end * 16 + rule: the first byte picks the rules that can start
// with it. PATH and PATH_SEG${ need / right after the PATH_CHAR run from i, URI needs : (scheme chars are PATH_CHARs)
int64_t Nx__lxScan(srs_str s, int64_t i) {
  int c = s.p[i];
  uint8_t k = nx_cls[c];
  if (k & NX_WS) return nx_wsRun(s, i) << 4 | NX_R_SKIP;
  if (k & NX_ALPHA) {
    // the ID run and the PATH_CHAR run from i in one pass while their bytes are in both classes
    int64_t j = nx_run(s, i + 1, NX_IDPC);
    uint8_t kj = j < s.n ? nx_cls[s.p[j]] : 0;
    int64_t id = kj & NX_ID ? nx_run(s, j, NX_ID) : j;
    int x = nx_at(s, kj & NX_PC ? nx_run(s, j, NX_PC) : j);
    if (x != '/' && x != ':') return id << 4 | NX_R_ID;
    int64_t e[4] = { id, nx_segDollarEnd(s, i), nx_pathEnd(s, i), nx_uriEnd(s, i) };
    static const int r[4] = { NX_R_ID, NX_R_SEGDOLLAR, NX_R_PATH, NX_R_URI };
    return nx_pick(4, e, r);
  }
  // PATH and PATH_SEG${ match from a PATH_CHAR only when / follows its PATH_CHAR run; else they are no candidates
  bool path = k & NX_PC && nx_at(s, nx_run(s, i, NX_PC)) == '/';
  if (k & NX_DIGIT) {
    int64_t d = nx_run(s, i, NX_DIGIT), f = nx_floatEnd(s, i);
    if (!path) return f > d ? f << 4 | NX_R_FLOAT : d << 4 | NX_R_INT;
    int64_t e[4] = { d, f, nx_segDollarEnd(s, i), nx_pathEnd(s, i) };
    static const int r[4] = { NX_R_INT, NX_R_FLOAT, NX_R_SEGDOLLAR, NX_R_PATH };
    return nx_pick(4, e, r);
  }
  switch (c) {
  case '#': return nx_lineEnd(s, i + 1) << 4 | NX_R_SKIP;
  case '_': {
    if (!path) return nx_run(s, i + 1, NX_ID) << 4 | NX_R_ID;
    int64_t e[3] = { nx_run(s, i + 1, NX_ID), nx_segDollarEnd(s, i), nx_pathEnd(s, i) };
    static const int r[3] = { NX_R_ID, NX_R_SEGDOLLAR, NX_R_PATH };
    return nx_pick(3, e, r);
  }
  case '.': {
    if (!path) {
      int64_t e[3] = { nx_opEnd(s, i), nx_floatEnd(s, i), i + 1 };
      static const int r[3] = { NX_R_OP, NX_R_FLOAT, NX_R_ANY };
      return nx_pick(3, e, r);
    }
    int64_t e[5] = { nx_opEnd(s, i), nx_floatEnd(s, i), nx_segDollarEnd(s, i), nx_pathEnd(s, i), i + 1 };
    static const int r[5] = { NX_R_OP, NX_R_FLOAT, NX_R_SEGDOLLAR, NX_R_PATH, NX_R_ANY };
    return nx_pick(5, e, r);
  }
  case '-': case '+': case '/': {
    if (!path && c != '/') { int64_t o = nx_opEnd(s, i); return o > i ? o << 4 | NX_R_OP : (i + 1) << 4 | NX_R_ANY; }
    int64_t cm = c == '/' && nx_at(s, i + 1) == '*' ? nx_commentEnd(s, i + 2, i) : i;
    int64_t e[5] = { nx_opEnd(s, i), nx_segDollarEnd(s, i), nx_pathEnd(s, i), cm, i + 1 };
    static const int r[5] = { NX_R_OP, NX_R_SEGDOLLAR, NX_R_PATH, NX_R_SKIP, NX_R_ANY };
    return nx_pick(5, e, r);
  }
  case '~': {
    int64_t e[3] = { nx_segDollarEnd(s, i), nx_hpathEnd(s, i), i + 1 };
    static const int r[3] = { NX_R_SEGDOLLAR, NX_R_PATH, NX_R_ANY };
    return nx_pick(3, e, r);
  }
  case '<': {
    int64_t e[3] = { nx_opEnd(s, i), nx_spathEnd(s, i), i + 1 };
    static const int r[3] = { NX_R_OP, NX_R_SPATH, NX_R_ANY };
    return nx_pick(3, e, r);
  }
  // no operator starts with these: {ANY}
  case ',': case ';': case '(': case ')': case '[': case ']': case ':': case '@': case '?': case '*': return (i + 1) << 4 | NX_R_ANY;
  case '=': return nx_at(s, i + 1) == '=' ? (i + 2) << 4 | NX_R_OP : (i + 1) << 4 | NX_R_ANY;
  case '$': return nx_at(s, i + 1) == '{' ? (i + 2) << 4 | NX_R_DOLLAR : (i + 1) << 4 | NX_R_ANY;
  case '{': return (i + 1) << 4 | NX_R_OPEN;
  case '}': return (i + 1) << 4 | NX_R_CLOSE;
  case '"': return (i + 1) << 4 | NX_R_QUOTE;
  case '\'': {
    if (nx_at(s, i + 1) != '\'') return (i + 1) << 4 | NX_R_ANY;
    // \'\'(\ *\n)?
    int64_t j = i + 2;
    while (nx_at(s, j) == ' ') j++;
    return (nx_at(s, j) == '\n' ? j + 1 : i + 2) << 4 | NX_R_INDOPEN;
  }
  default: {
    int64_t o = nx_opEnd(s, i);
    return o > i ? o << 4 | NX_R_OP : (i + 1) << 4 | NX_R_ANY;
  }
  }
}
// the end of the one skip lexeme at i (whitespace, # comment, closed /* comment: Nx__lxScan's NX_R_SKIP), or i
static inline int64_t nx_skipEnd(srs_str s, int64_t i) {
  int c = nx_at(s, i);
  if (nx_is(c, NX_WS)) return nx_wsRun(s, i);
  if (c == '#') return nx_lineEnd(s, i + 1);
  if (c == '/' && nx_at(s, i + 1) == '*') return nx_commentEnd(s, i + 2, i);
  return i;
}
// the end of the skip lexemes from i, one after the other
int64_t Nx__lxSkip(srs_str s, int64_t i) {
  while (i < s.n) {
    uint8_t c = s.p[i];
    if (nx_cls[c] & NX_WS) i = nx_wsRun(s, i);
    else if (c == '#') i = nx_lineEnd(s, i + 1);
    else if (c == '/' && i + 1 < s.n && s.p[i + 1] == '*') { int64_t e = nx_commentEnd(s, i + 2, i); if (e == i) break; i = e; }
    else break;
  }
  return i;
}
// the begin of the last skip lexeme in [i, j) (j = Nx__lxSkip(s, i)), or lb when there is none
int64_t Nx__lxLastSkip(srs_str s, int64_t i, int64_t j, int64_t lb) {
  while (i < j) { lb = i; i = nx_skipEnd(s, i); }
  return lb;
}
// <INPATH,INPATH_SLASH> {PATH}|{PATH_SEG}|{PATH_CHAR}+: the end of the longest of the three from i, or i
int64_t Nx__lxPathPieceEnd(srs_str s, int64_t i) {
  int64_t p = nx_pathEnd(s, i), g = nx_segEnd(s, i), c = nx_run(s, i, NX_PC);
  return p > g ? (p > c ? p : c) : (g > c ? g : c);
}

// ── the parser's attribute index (nx/parse.srs Nx.BSet): name -> entry word, the entries in the order their names
// were first put. Up to NX_ATLIN names are found by a scan, more by open addressing on nx_hash (slot = position + 1,
// 0 = free). A set under construction is updated in place. The indexes and their arrays are bumped from one pool
// (a file's sets: dead once it is parsed), emptied by Nx.atFreeAll
typedef struct nx_atab { srs_str* k; void** v; int32_t* slot; int64_t n, cap, scap; } nx_atab;
typedef struct nx_atchunk { struct nx_atchunk* next; size_t cap, used; } nx_atchunk;
enum { NX_ATCHUNK = 1 << 16, NX_ATLIN = 8 };
static nx_atchunk* g_atpool;
// n zeroed bytes from the pool
static void* nx_at_alloc(size_t n) {
  n = (n + 15) & ~(size_t)15;
  nx_atchunk* c = g_atpool;
  if (!c || c->used + n > c->cap) {
    size_t cap = n > NX_ATCHUNK ? n : NX_ATCHUNK;
    c = malloc(sizeof *c + 16 + cap);
    if (!c) { fputs("snx: out of memory\n", stderr); exit(70); }
    c->next = g_atpool; c->cap = cap; c->used = 0; g_atpool = c;
  }
  char* p = (char*)(c + 1) + 16 - ((uintptr_t)(c + 1) & 15) + c->used;
  c->used += n;
  return memset(p, 0, n);
}
void* Nx__atNew(void) { return nx_at_alloc(sizeof(nx_atab)); }
// every chunk goes but the first one made (its pages stay touched for the next file)
int64_t Nx__atFreeAll(void) {
  while (g_atpool && g_atpool->next) { nx_atchunk* c = g_atpool; g_atpool = c->next; free(c); }
  if (g_atpool) { if (g_atpool->cap > NX_ATCHUNK) { free(g_atpool); g_atpool = 0; } else g_atpool->used = 0; }
  return 0;
}
// the slot of name n: its entry's, or the free one where it goes
static int64_t nx_at_slot(nx_atab* t, srs_str n) {
  uint64_t m = (uint64_t)t->scap - 1, j = nx_hash(n) & m;
  for (; t->slot[j]; j = (j + 1) & m) { srs_str k = t->k[t->slot[j] - 1]; if (k.n == n.n && nx_same(k.p, n.p, n.n)) break; }
  return (int64_t)j;
}
// the position of name n, or -1
static int64_t nx_at_pos(nx_atab* t, srs_str n) {
  if (!t->scap) {
    for (int64_t i = 0; i < t->n; i++) if (t->k[i].n == n.n && nx_same(t->k[i].p, n.p, n.n)) return i;
    return -1;
  }
  return (int64_t)t->slot[nx_at_slot(t, n)] - 1;
}
// a slot table of 4 slots a name (at most half full until it doubles)
static void nx_at_rehash(nx_atab* t) {
  t->scap = t->scap ? 2 * t->scap : 4 * NX_ATLIN;
  t->slot = nx_at_alloc((size_t)t->scap * sizeof *t->slot);
  for (int64_t i = 0; i < t->n; i++) t->slot[nx_at_slot(t, t->k[i])] = (int32_t)(i + 1);
}
void* Nx__atFind(nx_atab* t, srs_str n) { int64_t i = nx_at_pos(t, n); return i < 0 ? 0 : t->v[i]; }
void* Nx__atEnt(void* e) { return e; }
int64_t Nx__atLen(nx_atab* t) { return t->n; }
void* Nx__atAt(nx_atab* t, int64_t i) { return t->v[i]; }
srs_str Nx__atName(nx_atab* t, int64_t i) { return t->k[i]; }
// x as the last entry, n being new
static void nx_at_append(nx_atab* t, srs_str n, void* x) {
  if (t->n == t->cap) {
    nx_atab o = *t; t->cap = o.cap ? 2 * o.cap : 4;
    t->k = nx_at_alloc((size_t)t->cap * sizeof *t->k); t->v = nx_at_alloc((size_t)t->cap * sizeof *t->v);
    if (o.n) { memcpy(t->k, o.k, (size_t)o.n * sizeof *t->k); memcpy(t->v, o.v, (size_t)o.n * sizeof *t->v); }
  }
  t->k[t->n] = n; t->v[t->n] = x; t->n++;
  if (t->scap) { if (4 * t->n > 2 * t->scap) nx_at_rehash(t); else t->slot[nx_at_slot(t, n)] = (int32_t)t->n; }
  else if (t->n > NX_ATLIN) nx_at_rehash(t);
}
// x as the entry of n: in place of n's entry, or a new last one
int64_t Nx__atPut(nx_atab* t, srs_str n, void* x) {
  int64_t i = nx_at_pos(t, n);
  if (i >= 0) t->v[i] = x; else nx_at_append(t, n, x);
  return 0;
}
// the entry of n; when there is none (0), x becomes it, the new last one
void* Nx__atAdd(nx_atab* t, srs_str n, void* x) {
  int64_t i = nx_at_pos(t, n);
  if (i >= 0) return t->v[i];
  nx_at_append(t, n, x);
  return 0;
}

// ── token vector (nx/lex.srs): the lexer's tokens in order; the parser reads them by index (Nx__tsBind). One array
// serves every file in turn (Nx.tvFree empties it), so its pages are touched once
typedef struct { void** p; int64_t n, cap; } nx_tv;
static nx_tv g_tv;
void* Nx__tvNew(void) { g_tv.n = 0; return &g_tv; }
void* Nx__tvPush(nx_tv* v, void* x) {
  if (v->n == v->cap) { v->cap = v->cap ? 2 * v->cap : 256; v->p = realloc(v->p, (size_t)v->cap * sizeof *v->p); if (!v->p) { fputs("snx: out of memory\n", stderr); exit(70); } }
  v->p[v->n++] = x;
  return v;
}
int64_t Nx__tvLen(nx_tv* v) { return v->n; }
void* Nx__tvAt(nx_tv* v, int64_t i) { return v->p[i]; }
// a file's tokens done: an array grown past 1M tokens (8 MB: one huge file) goes back rather than stay resident
int64_t Nx__tvFree(nx_tv* v) { v->n = 0; if (v->cap > 1 << 20) { free(v->p); v->p = 0; v->cap = 0; } return 0; }
static nx_tv* g_ts;
int64_t Nx__tsBind(nx_tv* v) { g_ts = v; return 0; }
void* Nx__tsAt(int64_t i) { return g_ts->p[i < g_ts->n ? i : g_ts->n - 1]; }
int64_t Nx__tsLen(void) { return g_ts->n; }
