// snx runtime, wave 1: file system, bit and float services bound by `extern def` in nx/b_path.srs and nx/b_misc.srs.
// A string is srs_str (bytes + length, no NUL); length -1 = failure.
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct { const uint8_t* p; int64_t n; } srs_str;

static char* fs_cstr(srs_str s) {
  char* c = malloc((size_t)s.n + 1);
  if (!c) exit(70);
  memcpy(c, s.p, (size_t)s.n);
  c[s.n] = 0;
  return c;
}

// the kind of the file at path: 0 none, 1 regular, 2 directory, 3 symlink, 4 other (follow: stat, else lstat)
int64_t Nx__fsKind(srs_str path, bool follow) {
  char* c = fs_cstr(path);
  struct stat st;
  int r = follow ? stat(c, &st) : lstat(c, &st);
  free(c);
  if (r != 0) return 0;
  if (S_ISREG(st.st_mode)) return 1;
  if (S_ISDIR(st.st_mode)) return 2;
  if (S_ISLNK(st.st_mode)) return 3;
  return 4;
}

// the entry names of a directory, each followed by a NUL; length -1 when it cannot be read
srs_str Nx__dirNames(srs_str path) {
  char* c = fs_cstr(path);
  DIR* d = opendir(c);
  free(c);
  if (!d) return (srs_str){(const uint8_t*)"", -1};
  size_t cap = 256, n = 0;
  uint8_t* b = malloc(cap);
  if (!b) exit(70);
  struct dirent* e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    size_t k = strlen(e->d_name) + 1;
    while (n + k > cap) { cap *= 2; b = realloc(b, cap); if (!b) exit(70); }
    memcpy(b + n, e->d_name, k);
    n += k;
  }
  closedir(d);
  return (srs_str){b, (int64_t)n};
}

// the bytes of the file at path as PosixSourceAccessor::readFile reads them: st_size bytes of a regular file
srs_str Nx__fileBytes(srs_str path) {
  char* c = fs_cstr(path);
  FILE* f = fopen(c, "rb");
  free(c);
  if (!f) return (srs_str){(const uint8_t*)"", -1};
  struct stat st;
  if (fstat(fileno(f), &st) != 0 || S_ISDIR(st.st_mode)) { fclose(f); return (srs_str){(const uint8_t*)"", -1}; }
  size_t want = (size_t)st.st_size;
  uint8_t* b = malloc(want + 1);
  if (!b) exit(70);
  size_t got = fread(b, 1, want, f);
  fclose(f);
  return (srs_str){b, (int64_t)got};
}


static double fs_d(int64_t b) { double d; memcpy(&d, &b, sizeof d); return d; }

// floor (up = false) or ceil of a float as an integer; ok is false when the result is outside the NixInt range
bool Nx__roundOk(int64_t bits, bool up) {
  double r = up ? ceil(fs_d(bits)) : floor(fs_d(bits));
  return r >= -9223372036854775808.0 && r < 9223372036854775808.0;
}
int64_t Nx__roundInt(int64_t bits, bool up) { return (int64_t)(up ? ceil(fs_d(bits)) : floor(fs_d(bits))); }

srs_str Nx__cwd(void) {
  char* b = malloc(4096);
  if (!b || !getcwd(b, 4096)) return (srs_str){(const uint8_t*)"/", 1};
  return (srs_str){(const uint8_t*)b, (int64_t)strlen(b)};
}

bool Nx__hasNul(srs_str s) { return s.n > 0 && memchr(s.p, 0, (size_t)s.n) != NULL; }

// the path with every symlink resolved (resolveExprPath), or path itself when that fails
srs_str Nx__realPath(srs_str path) {
  char* c = fs_cstr(path);
  char* r = realpath(c, NULL);
  free(c);
  if (!r) return path;
  return (srs_str){(const uint8_t*)r, (int64_t)strlen(r)};
}

// ---- JSON and XML text (nlohmann dump, xml-writer.cc writeAttrs) ----------------------------------------------

static srs_str fs_mk(uint8_t* p, int64_t n) { return (srs_str){p, n}; }

// the UTF-8 bytes of code point cp
srs_str Nx__utf8(int64_t cp) {
  uint8_t* b = malloc(4);
  if (!b) exit(70);
  if (cp < 0x80) { b[0] = (uint8_t)cp; return fs_mk(b, 1); }
  if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 63); return fs_mk(b, 2); }
  if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 63); b[2] = 0x80 | (cp & 63); return fs_mk(b, 3); }
  b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 63); b[2] = 0x80 | ((cp >> 6) & 63); b[3] = 0x80 | (cp & 63);
  return fs_mk(b, 4);
}

// the length of the well-formed UTF-8 sequence at s[i], or 0 (RFC 3629: no overlongs, no surrogates, at most U+10FFFF)
static int fs_u8(const uint8_t* s, int64_t n, int64_t i) {
  uint8_t c = s[i];
  if (c < 0x80) return 1;
  int k = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
  if (!k || i + k > n) return 0;
  for (int j = 1; j < k; j++) if ((s[i + j] & 0xC0) != 0x80) return 0;
  if (c == 0xE0 && s[i + 1] < 0xA0) return 0;
  if (c == 0xED && s[i + 1] > 0x9F) return 0;
  if (c == 0xF0 && s[i + 1] < 0x90) return 0;
  if (c == 0xF4 && s[i + 1] > 0x8F) return 0;
  return k;
}

// a JSON string literal of s as nlohmann::json::dump writes it; length -1 when s is not UTF-8
srs_str Nx__jsonStr(srs_str s) {
  uint8_t* o = malloc((size_t)s.n * 6 + 3);
  if (!o) exit(70);
  int64_t n = 0;
  o[n++] = '"';
  for (int64_t i = 0; i < s.n;) {
    uint8_t c = s.p[i];
    if (c >= 0x80) {
      int k = fs_u8(s.p, s.n, i);
      if (!k) { free(o); return fs_mk((const uint8_t*)"", -1); }
      memcpy(o + n, s.p + i, (size_t)k); n += k; i += k; continue;
    }
    i++;
    switch (c) {
      case '"': o[n++] = '\\'; o[n++] = '"'; break;
      case '\\': o[n++] = '\\'; o[n++] = '\\'; break;
      case '\b': o[n++] = '\\'; o[n++] = 'b'; break;
      case '\f': o[n++] = '\\'; o[n++] = 'f'; break;
      case '\n': o[n++] = '\\'; o[n++] = 'n'; break;
      case '\r': o[n++] = '\\'; o[n++] = 'r'; break;
      case '\t': o[n++] = '\\'; o[n++] = 't'; break;
      default:
        if (c < 0x20) n += sprintf((char*)o + n, "\\u%04x", c);
        else o[n++] = c;
    }
  }
  o[n++] = '"';
  return fs_mk(o, n);
}

// a double as nlohmann::json::dump writes it: the shortest digits that read back, "null" for nan and inf
srs_str Nx__jsonFloat(int64_t bits) {
  double d; memcpy(&d, &bits, sizeof d);
  char* o = malloc(40);
  if (!o) exit(70);
  if (isnan(d) || isinf(d)) { memcpy(o, "null", 4); return fs_mk((uint8_t*)o, 4); }
  if (d == 0) { int n = signbit(d) ? 4 : 3; memcpy(o, signbit(d) ? "-0.0" : "0.0", (size_t)n); return fs_mk((uint8_t*)o, n); }
  char e[40]; int p;
  for (p = 0; p < 17; p++) { snprintf(e, sizeof e, "%.*e", p, d); if (strtod(e, NULL) == d) break; }
  // e = [-]D[.DDD]e[+-]XX: digits and decimal exponent
  char dg[24]; int nd = 0; const char* q = e; int neg = 0;
  if (*q == '-') { neg = 1; q++; }
  for (; *q && *q != 'e'; q++) if (*q != '.') dg[nd++] = *q;
  int ex = atoi(q + 1);
  while (nd > 1 && dg[nd - 1] == '0') nd--;
  int k = nd, nn = ex + 1, n = 0;   // value = 0.dg * 10^nn
  if (neg) o[n++] = '-';
  if (k <= nn && nn <= 16) { memcpy(o + n, dg, (size_t)k); n += k; for (int i = 0; i < nn - k; i++) o[n++] = '0'; o[n++] = '.'; o[n++] = '0'; }
  else if (0 < nn && nn <= 16) { memcpy(o + n, dg, (size_t)nn); n += nn; o[n++] = '.'; memcpy(o + n, dg + nn, (size_t)(k - nn)); n += k - nn; }
  else if (-5 < nn && nn <= 0) { o[n++] = '0'; o[n++] = '.'; for (int i = 0; i < -nn; i++) o[n++] = '0'; memcpy(o + n, dg, (size_t)k); n += k; }
  else {
    o[n++] = dg[0];
    if (k > 1) { o[n++] = '.'; memcpy(o + n, dg + 1, (size_t)(k - 1)); n += k - 1; }
    n += sprintf(o + n, "e%c%02d", nn - 1 < 0 ? '-' : '+', abs(nn - 1));
  }
  return fs_mk((uint8_t*)o, n);
}

// s with " < > & and newline escaped for an XML attribute value
srs_str Nx__xmlEsc(srs_str s) {
  uint8_t* o = malloc((size_t)s.n * 6 + 1);
  if (!o) exit(70);
  int64_t n = 0;
  for (int64_t i = 0; i < s.n; i++) {
    uint8_t c = s.p[i];
    const char* r = c == '"' ? "&quot;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : c == '\n' ? "&#xA;" : NULL;
    if (r) { size_t k = strlen(r); memcpy(o + n, r, k); n += (int64_t)k; } else o[n++] = c;
  }
  return fs_mk(o, n);
}

extern srs_str Nx__canonPath(srs_str s);

// resolveExprPath (eval.cc:3156): while the last component is a symlink (its ancestors resolved), continue with its target
// taken relative to the unresolved parent
srs_str Nx__followLast(srs_str path) {
  for (int hops = 0; hops < 1024; hops++) {
    path = Nx__canonPath(path);
    int64_t k = path.n;
    while (k > 0 && path.p[k - 1] != '/') k--;
    if (k <= 1) return path;
    srs_str parent = {path.p, k - 1};
    char* pc = fs_cstr(parent);
    char* rp = realpath(pc, NULL);
    free(pc);
    size_t pl = rp ? strlen(rp) : (size_t)parent.n;
    size_t bl = (size_t)(path.n - k);
    char* full = malloc(pl + 1 + bl + 1);
    if (!full) exit(70);
    memcpy(full, rp ? rp : (const char*)parent.p, pl);
    full[pl] = '/';
    memcpy(full + pl + 1, path.p + k, bl);
    full[pl + 1 + bl] = 0;
    free(rp);
    char tgt[4096];
    ssize_t t = readlink(full, tgt, sizeof tgt);
    free(full);
    if (t <= 0) return path;
    size_t tl = (size_t)t;
    if (tgt[0] == '/') {
      char* c = malloc(tl);
      if (!c) exit(70);
      memcpy(c, tgt, tl);
      path = (srs_str){(const uint8_t*)c, (int64_t)tl};
    } else {
      char* c = malloc((size_t)parent.n + 1 + tl);
      if (!c) exit(70);
      memcpy(c, parent.p, (size_t)parent.n);
      c[parent.n] = '/';
      memcpy(c + parent.n + 1, tgt, tl);
      path = (srs_str){(const uint8_t*)c, (int64_t)((size_t)parent.n + 1 + tl)};
    }
  }
  return path;
}

// ---- positions: the record of the source positions of the names of a set or of the formals of a function, by the
// address of the array of their symbols (nix/prim unsafeGetAttrPos)
static void** pos_k; static void** pos_v; static int64_t pos_cap, pos_n;
static uint64_t pos_h(void* p) { uint64_t h = (uint64_t)(uintptr_t)p; h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ull; return h ^ (h >> 32); }
void* Nx__posGet(void* key) {
  if (!pos_cap) return NULL;
  for (uint64_t i = pos_h(key) & (uint64_t)(pos_cap - 1);; i = (i + 1) & (uint64_t)(pos_cap - 1)) {
    if (!pos_k[i]) return NULL;
    if (pos_k[i] == key) return pos_v[i];
  }
}
int64_t Nx__posPut(void* key, void* val) {
  if (2 * (pos_n + 1) > pos_cap) {
    int64_t cap = pos_cap ? pos_cap * 2 : 1024;
    void** k = calloc((size_t)cap, sizeof(void*)); void** v = calloc((size_t)cap, sizeof(void*));
    if (!k || !v) exit(70);
    for (int64_t j = 0; j < pos_cap; j++) if (pos_k[j]) {
      uint64_t i = pos_h(pos_k[j]) & (uint64_t)(cap - 1);
      while (k[i]) i = (i + 1) & (uint64_t)(cap - 1);
      k[i] = pos_k[j]; v[i] = pos_v[j];
    }
    free(pos_k); free(pos_v); pos_k = k; pos_v = v; pos_cap = cap;
  }
  uint64_t i = pos_h(key) & (uint64_t)(pos_cap - 1);
  while (pos_k[i] && pos_k[i] != key) i = (i + 1) & (uint64_t)(pos_cap - 1);
  if (!pos_k[i]) pos_n++;
  pos_k[i] = key; pos_v[i] = val;
  return 0;
}

// ---- BLAKE3 (default hash mode, 32 bytes), after the reference implementation of the specification ----------------
static const uint32_t b3_iv[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
static const uint8_t b3_perm[16] = {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8};
enum { B3_START = 1, B3_END = 2, B3_PARENT = 4, B3_ROOT = 8 };
static uint32_t b3_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void b3_g(uint32_t* s, int a, int b, int c, int d, uint32_t mx, uint32_t my) {
  s[a] += s[b] + mx; s[d] = b3_ror(s[d] ^ s[a], 16);
  s[c] += s[d]; s[b] = b3_ror(s[b] ^ s[c], 12);
  s[a] += s[b] + my; s[d] = b3_ror(s[d] ^ s[a], 8);
  s[c] += s[d]; s[b] = b3_ror(s[b] ^ s[c], 7);
}
// the 16 output words of one compression
static void b3_compress(const uint32_t cv[8], const uint32_t blk[16], uint64_t ctr, uint32_t len, uint32_t flags, uint32_t out[16]) {
  uint32_t s[16], m[16], t[16];
  memcpy(s, cv, 32); memcpy(s + 8, b3_iv, 16);
  s[12] = (uint32_t)ctr; s[13] = (uint32_t)(ctr >> 32); s[14] = len; s[15] = flags;
  memcpy(m, blk, 64);
  for (int r = 0; r < 7; r++) {
    b3_g(s, 0, 4, 8, 12, m[0], m[1]); b3_g(s, 1, 5, 9, 13, m[2], m[3]);
    b3_g(s, 2, 6, 10, 14, m[4], m[5]); b3_g(s, 3, 7, 11, 15, m[6], m[7]);
    b3_g(s, 0, 5, 10, 15, m[8], m[9]); b3_g(s, 1, 6, 11, 12, m[10], m[11]);
    b3_g(s, 2, 7, 8, 13, m[12], m[13]); b3_g(s, 3, 4, 9, 14, m[14], m[15]);
    if (r < 6) { for (int i = 0; i < 16; i++) t[i] = m[b3_perm[i]]; memcpy(m, t, 64); }
  }
  for (int i = 0; i < 8; i++) { out[i] = s[i] ^ s[i + 8]; out[i + 8] = s[i + 8] ^ cv[i]; }
}
typedef struct { uint32_t cv[8], blk[16]; uint64_t ctr; uint32_t len, flags; } b3_out;
static void b3_words(const uint8_t* p, int64_t n, uint32_t w[16]) {
  uint8_t b[64] = {0}; memcpy(b, p, (size_t)n);
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4*i] | (uint32_t)b[4*i+1] << 8 | (uint32_t)b[4*i+2] << 16 | (uint32_t)b[4*i+3] << 24;
}
// the output of chunk number ctr (data p, n bytes): its last block, not yet compressed; its earlier blocks are absorbed
static b3_out b3_chunk(const uint8_t* p, int64_t n, uint64_t ctr) {
  b3_out o; memcpy(o.cv, b3_iv, 32); o.ctr = ctr;
  int64_t nb = n == 0 ? 1 : (n + 63) / 64;
  for (int64_t i = 0; i < nb; i++) {
    int64_t len = n - i * 64 > 64 ? 64 : n - i * 64;
    uint32_t fl = (i == 0 ? B3_START : 0) | (i == nb - 1 ? B3_END : 0);
    b3_words(p + i * 64, len, o.blk); o.len = (uint32_t)len; o.flags = fl;
    if (i < nb - 1) { uint32_t out[16]; b3_compress(o.cv, o.blk, ctr, o.len, fl, out); memcpy(o.cv, out, 32); }
  }
  return o;
}
static void b3_cv(const b3_out* o, uint32_t cv[8]) { uint32_t out[16]; b3_compress(o->cv, o->blk, o->ctr, o->len, o->flags, out); memcpy(cv, out, 32); }
static b3_out b3_parent(const uint32_t l[8], const uint32_t r[8]) {
  b3_out o; memcpy(o.cv, b3_iv, 32); memcpy(o.blk, l, 32); memcpy(o.blk + 8, r, 32); o.ctr = 0; o.len = 64; o.flags = B3_PARENT; return o;
}
srs_str Nx__blake3(srs_str s) {
  uint32_t stack[54][8]; int sp = 0;
  int64_t chunks = s.n == 0 ? 1 : (s.n + 1023) / 1024;
  for (int64_t i = 0; i < chunks - 1; i++) {
    b3_out o = b3_chunk(s.p + i * 1024, 1024, (uint64_t)i);
    uint32_t cv[8]; b3_cv(&o, cv);
    uint64_t total = (uint64_t)i + 1;
    while ((total & 1) == 0) { b3_out p = b3_parent(stack[--sp], cv); b3_cv(&p, cv); total >>= 1; }
    memcpy(stack[sp++], cv, 32);
  }
  b3_out o = b3_chunk(s.p + (chunks - 1) * 1024, s.n - (chunks - 1) * 1024, (uint64_t)(chunks - 1));
  while (sp > 0) { uint32_t cv[8]; b3_cv(&o, cv); o = b3_parent(stack[--sp], cv); }
  uint32_t out[16]; b3_compress(o.cv, o.blk, 0, o.len, o.flags | B3_ROOT, out);
  uint8_t* r = malloc(32); if (!r) exit(70);
  for (int i = 0; i < 8; i++) { r[4*i] = (uint8_t)out[i]; r[4*i+1] = (uint8_t)(out[i] >> 8); r[4*i+2] = (uint8_t)(out[i] >> 16); r[4*i+3] = (uint8_t)(out[i] >> 24); }
  return fs_mk(r, 32);
}
