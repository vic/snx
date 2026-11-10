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

// the bits of 0.0 - d for the bits of a double d, as the parse oracle folds a negated literal (nx/tree.srs, SPEC.md rule 3)
int64_t Nx__treeFloatNeg(int64_t bits) {
  double d; memcpy(&d, &bits, sizeof d);
  double r = 0.0 - d;
  int64_t out; memcpy(&out, &r, sizeof out);
  return out;
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

// ── cells: one word holding a value (nx/val.srs Nx.cell)
void* Nx__cell(void* v) { void** c = nx_words(1); *c = v; return c; }
void* Nx__get(void** c) { return *c; }
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
void* Nx__globals(void) { static void* g[16]; return g; }
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
typedef struct { int64_t sym, i; } nx_si;
static int nx_name_cmp(const void* a, const void* b) {
  srs_str x = g_names[((const nx_si*)a)->sym], y = g_names[((const nx_si*)b)->sym];
  int64_t m = x.n < y.n ? x.n : y.n;
  int c = memcmp(x.p, y.p, (size_t)m);
  return c ? c : (x.n < y.n ? -1 : x.n > y.n);
}
void* Nx__byName(const int64_t* syms, int64_t n) {
  nx_si* t = malloc((size_t)(n > 0 ? n : 1) * sizeof *t);
  if (!t) { fputs("snx: out of memory\n", stderr); exit(70); }
  for (int64_t i = 0; i < n; i++) { t[i].sym = syms[i]; t[i].i = i; }
  qsort(t, (size_t)n, sizeof *t, nx_name_cmp);
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
int64_t Nx__sbAdd(nx_sb* b, srs_str s) {
  if (b->n + s.n > b->cap) { int64_t cap = b->cap ? b->cap : 64; while (cap < b->n + s.n) cap *= 2; b->p = realloc(b->p, (size_t)cap); if (!b->p) { fputs("snx: out of memory\n", stderr); exit(70); } b->cap = cap; }
  memcpy(b->p + b->n, s.p, (size_t)s.n); b->n += s.n;
  return 0;
}
srs_str Nx__sbGet(nx_sb* b) { uint8_t* p = nx_alloc(b->n); memcpy(p, b->p, (size_t)b->n); return (srs_str){p, b->n}; }

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
