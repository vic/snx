// snx runtime: the evaluation memo of SNX_CACHE=<dir>, bound by `extern def` in nx/main.srs (Nx.cacheBegin, Nx.cacheEnd).
// A run is a function of the snx executable, argv, the working directory and what it observes. Every observation is one
// of the externs below; the linker routes each call to its __wrap_ here (-Wl,--wrap, tests/build.sh). A recording run
// keeps (op, argument, result: exact value or length + 128-bit digest of the bytes), each first observation once; at exit 0 the
// trace and the exact stdout and stderr bytes go to <dir>/<key>, key = sha256(executable bytes, argv, cwd, the env the
// C runtime reads). A later run with the same key repeats each observation in order and compares: all equal => it writes
// the stored output and exits with the stored code; anything else (no entry, a corrupt entry, a mismatch) => a normal,
// recording run. Never stored: a run that read the clock or opened a socket, a failed run, a run whose input changed under it.
// SNX_CACHE_STAT=1: a file content observation whose (dev, inode, size, mtime, ctime) is unchanged is not re-read; an entry
// takes that shortcut only when the file's ctime was at least 1 s before the recording run began (else it re-hashes).
// SNX_CACHE_DEBUG=1: one line on stderr, `snx-cache: hit|miss|store|skip ...`, written past the captured stderr.
// Store roots: an observation under the store dir (SNX_CACHE_STORE, default /nix/store) is stored as its store root, checked once
// by (dev, inode, ctime_ns); the executable is keyed by its path and (dev, inode, size, mtime_ns), not its bytes.
#define _GNU_SOURCE  // memfd_create (the miss run's stdout capture)
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/mman.h>

typedef struct { const uint8_t* p; int64_t n; } srs_str;

srs_str Nx__hashBytes(int64_t algo, srs_str s);
int64_t rt_argc(void);
srs_str rt_arg(int64_t i);
// srs rt/srs_rt.c: writes the srs stdout buffer to fd 1 (declared in srs rt/srs_cap.h, not on this build's include path)
int64_t rt_out_flush(void);

srs_str __real_Nx__readFile(srs_str path);
srs_str __real_rt_read_file(srs_str path);
bool __real_Nx__isDir(srs_str path);
srs_str __real_Nx__getEnv(srs_str name);
int64_t __real_Nx__timeNow(void);
int64_t __real_Nx__fsKind(srs_str path, bool follow);
srs_str __real_Nx__dirNames(srs_str path);
srs_str __real_Nx__fileBytes(srs_str path);
bool __real_Nx__fsExec(srs_str path);
srs_str __real_Nx__readLink(srs_str path);
srs_str __real_Nx__cwd(void);
srs_str __real_Nx__realPath(srs_str path);
int64_t __real_Nx__netOpen(srs_str path);

// the observations; T_* below 16 have an integer result, the rest a string result
enum { T_ISDIR = 1, T_FSKIND, T_LSKIND, T_FSEXEC, T_ROOT, T_READFILE = 16, T_RTREAD, T_FILEBYTES, T_GETENV, T_DIRNAMES, T_READLINK,
       T_CWD, T_REALPATH, T_LAST };
static bool t_str(int op) { return op >= T_READFILE; }
static bool t_content(int op) { return op == T_READFILE || op == T_RTREAD || op == T_FILEBYTES; }

// v = the integer result, or the string length (-1 = failed); d = sha256 of the string; st = dev, ino, size, mtime, ctime
typedef struct { uint8_t op, has_st, skip; uint32_t an; const uint8_t* a; int64_t v; uint8_t d[32]; int64_t st[5]; } obs;
typedef struct { uint8_t* p; size_t n, cap; } buf;

static struct {
  bool on, dbg, stat;
  const char* store; size_t sl;  // the store dir (SNX_CACHE_STORE, default /nix/store): a path under it is trusted by its root
  size_t calls;             // observation calls, repeats included
  const char* taint;        // why this run is not stored, or 0
  obs* o; size_t n, cap;
  uint32_t* tab; size_t tcap;  // open addressing over (op, arg): index + 1 into o
  int64_t t0;               // the recording run's start, ns since the epoch
  uint8_t key[32];
  char* path;
  buf out, err;
  int out_fd, saved_fd;     // a miss run: the memfd fd 1 points at, and the fd 1 it replaced
} T;

static void* t_alloc(size_t n) { void* p = malloc(n ? n : 1); if (!p) { fputs("snx: out of memory\n", stderr); exit(70); } return p; }
static void b_add(buf* b, const void* p, size_t n) {
  if (b->n + n > b->cap) { size_t c = b->cap ? b->cap : 4096; while (c < b->n + n) c *= 2; b->p = realloc(b->p, c); if (!b->p) exit(70); b->cap = c; }
  memcpy(b->p + b->n, p, n); b->n += n;
}
static void b_u64(buf* b, uint64_t v) { b_add(b, &v, 8); }
static void dbg(const char* what, const char* why, size_t n) {
  if (!T.dbg) return;
  char line[512];
  int k = snprintf(line, sizeof line, "snx-cache: %s %s (%zu observations)\n", what, why, n);
  if (k > 0 && write(2, line, (size_t)k) < 0) return;
}
static void sha(srs_str s, uint8_t d[32]) { srs_str h = Nx__hashBytes(2, s); memcpy(d, h.p, 32); free((void*)h.p); }
// a 128-bit digest for change detection (bytes 16..31 of d zero), not a cryptographic one: whoever can write an input can
// change the answer anyway. Two 64-bit lanes take 16 bytes a step, each through a 64x64->128 multiply folded to 64 bits;
// the old lane value is added back, so a zero product cannot erase it. sha256 (SHA-NI) cost a cold run 2-3% here
static uint64_t fold(uint64_t a, uint64_t b) { __uint128_t r = (__uint128_t)a * b; return (uint64_t)r ^ (uint64_t)(r >> 64); }
static void fh(const uint8_t* p, size_t n, uint64_t h[2]) {
  const uint64_t k0 = 0xa0761d6478bd642full, k1 = 0xe7037ed1a0b428dbull, k2 = 0x8ebc6af09c88c6e3ull, k3 = 0x589965cc75374cc3ull;
  uint64_t a = k0 ^ n, b = k1 ^ (n << 1), x, y;
  for (; n >= 16; p += 16, n -= 16) {
    memcpy(&x, p, 8); memcpy(&y, p + 8, 8);
    a = fold(x ^ k2, y ^ a) + a;
    b = fold(y ^ k3, x ^ b) + b;
  }
  uint8_t t[16] = {0};
  memcpy(t, p, n);
  memcpy(&x, t, 8); memcpy(&y, t + 8, 8);
  a = fold(x ^ k2 ^ n, y ^ a) + a;
  b = fold(y ^ k3, x ^ b ^ n) + b;
  h[0] = fold(a ^ k0, b ^ k3) ^ a;
  h[1] = fold(b ^ k1, h[0] ^ k2) ^ b;
}
static void dig(srs_str s, uint8_t d[32]) { uint64_t h[2]; fh(s.p, (size_t)s.n, h); memcpy(d, h, 16); memset(d + 16, 0, 16); }
static int64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
static char* cstr(const uint8_t* p, size_t n) { char* c = t_alloc(n + 1); memcpy(c, p, n); c[n] = 0; return c; }

// ---- recording ------------------------------------------------------------------------------------------------------
static uint64_t h_key(int op, srs_str a) { uint64_t h[2]; fh(a.p, (size_t)a.n, h); return h[0] ^ (uint64_t)op; }
static void tab_put(size_t i) {
  obs* o = &T.o[i];
  size_t m = T.tcap - 1, j = h_key(o->op, (srs_str){o->a, o->an}) & m;
  while (T.tab[j]) j = (j + 1) & m;
  T.tab[j] = (uint32_t)(i + 1);
}
// the earlier observation of (op, a), or 0
static obs* tab_find(int op, srs_str a) {
  if (!T.tcap) return 0;
  size_t m = T.tcap - 1, j = h_key(op, a) & m;
  for (; T.tab[j]; j = (j + 1) & m) {
    obs* o = &T.o[T.tab[j] - 1];
    if (o->op == op && o->an == (uint64_t)a.n && memcmp(o->a, a.p, (size_t)a.n) == 0) return o;
  }
  return 0;
}
static obs* rec_new(int op, srs_str a) {
  if (T.n == T.cap) { T.cap = T.cap ? 2 * T.cap : 1024; T.o = realloc(T.o, T.cap * sizeof(obs)); if (!T.o) exit(70); }
  if (2 * (T.n + 1) > T.tcap) {
    free(T.tab); T.tcap = T.tcap ? 2 * T.tcap : 4096; T.tab = calloc(T.tcap, sizeof(uint32_t)); if (!T.tab) exit(70);
    for (size_t i = 0; i < T.n; i++) tab_put(i);
  }
  obs* o = &T.o[T.n];
  memset(o, 0, sizeof *o);
  o->op = (uint8_t)op; o->an = (uint32_t)a.n; o->a = (const uint8_t*)cstr(a.p, (size_t)a.n);
  tab_put(T.n++);
  return o;
}
// a path under the store dir stands for its store root <store>/<first component>, whose (dev, ino, ctime) is recorded once
// (T_ROOT, before the first observation under it); such an observation is not stored: the store is immutable, and a changed
// root (GC + re-add = new inode) is a miss. A root that cannot be lstat'ed keeps the per-path observation.
// shortcut: a symlink inside a root that leads out of the store is trusted like the rest of the root
static bool root_of(srs_str a) {
  size_t sl = T.sl;
  if (!sl || (size_t)a.n <= sl + 1 || memcmp(a.p, T.store, sl) != 0 || a.p[sl] != '/') return false;
  size_t e = sl + 1;
  while (e < (size_t)a.n && a.p[e] != '/') e++;
  size_t c = e - sl - 1;
  if (c == 0 || (c == 1 && a.p[sl + 1] == '.') || (c == 2 && a.p[sl + 1] == '.' && a.p[sl + 2] == '.')) return false;
  srs_str r = {a.p, (int64_t)e};
  if (tab_find(T_ROOT, r)) return true;
  char* path = cstr(a.p, e);
  struct stat s;
  int bad = lstat(path, &s);
  free(path);
  if (bad) return false;
  obs* o = rec_new(T_ROOT, r);
  o->has_st = 1;
  o->st[0] = (int64_t)s.st_dev; o->st[1] = (int64_t)s.st_ino;
  o->st[4] = (int64_t)s.st_ctim.tv_sec * 1000000000 + s.st_ctim.tv_nsec;
  return true;
}
static void rec_int(int op, srs_str a, int64_t v) {
  if (!T.on) return;
  T.calls++;
  obs* o = tab_find(op, a);
  if (o) { if (o->v != v) T.taint = "an input changed during the run"; return; }
  bool sk = root_of(a);
  obs* n = rec_new(op, a);
  n->v = v; n->skip = sk;
}
// the file at (p, n): 0 ok, -1 absent
static int stat_at(const uint8_t* p, size_t n, struct stat* s) { char* c = cstr(p, n); int r = stat(c, s); free(c); return r; }
static void st_tuple(const struct stat* s, int64_t st[5]) {
  st[0] = (int64_t)s->st_dev; st[1] = (int64_t)s->st_ino; st[2] = (int64_t)s->st_size;
  st[3] = (int64_t)s->st_mtim.tv_sec * 1000000000 + s->st_mtim.tv_nsec;
  st[4] = (int64_t)s->st_ctim.tv_sec * 1000000000 + s->st_ctim.tv_nsec;
}
static void rec_str(int op, srs_str a, srs_str r) {
  if (!T.on) return;
  T.calls++;
  uint8_t d[32] = {0};
  if (r.n >= 0) dig(r, d);
  obs* o = tab_find(op, a);
  if (o) { if (o->v != r.n || memcmp(o->d, d, 32) != 0) T.taint = "an input changed during the run"; return; }
  bool sk = root_of(a);
  o = rec_new(op, a);
  o->skip = sk;
  o->v = r.n; memcpy(o->d, d, 32);
  // a content read is stored only from a regular file of exactly the bytes read (no FIFO, device or /proc file: a replay
  // could block on it, consume it, or see other bytes under the same stat). Its stat tuple stands for the content when
  // its ctime is 1 s or more before the run began: a later write gives a new ctime
  if (t_content(op) && r.n >= 0) {
    struct stat s;
    if (stat_at(a.p, (size_t)a.n, &s) != 0 || !S_ISREG(s.st_mode) || s.st_size != r.n) { T.taint = "a file not regular or not of the size read was read"; return; }
    st_tuple(&s, o->st);
    o->has_st = o->st[4] < T.t0 - 1000000000;
  }
}

srs_str __wrap_Nx__readFile(srs_str p) { srs_str r = __real_Nx__readFile(p); rec_str(T_READFILE, p, r); return r; }
srs_str __wrap_rt_read_file(srs_str p) { srs_str r = __real_rt_read_file(p); rec_str(T_RTREAD, p, r); return r; }
srs_str __wrap_Nx__fileBytes(srs_str p) { srs_str r = __real_Nx__fileBytes(p); rec_str(T_FILEBYTES, p, r); return r; }
srs_str __wrap_Nx__getEnv(srs_str p) { srs_str r = __real_Nx__getEnv(p); rec_str(T_GETENV, p, r); return r; }
srs_str __wrap_Nx__dirNames(srs_str p) { srs_str r = __real_Nx__dirNames(p); rec_str(T_DIRNAMES, p, r); return r; }
srs_str __wrap_Nx__readLink(srs_str p) { srs_str r = __real_Nx__readLink(p); rec_str(T_READLINK, p, r); return r; }
srs_str __wrap_Nx__cwd(void) { srs_str r = __real_Nx__cwd(); rec_str(T_CWD, (srs_str){(const uint8_t*)"", 0}, r); return r; }
srs_str __wrap_Nx__realPath(srs_str p) { srs_str r = __real_Nx__realPath(p); rec_str(T_REALPATH, p, r); return r; }
bool __wrap_Nx__isDir(srs_str p) { bool r = __real_Nx__isDir(p); rec_int(T_ISDIR, p, r); return r; }
bool __wrap_Nx__fsExec(srs_str p) { bool r = __real_Nx__fsExec(p); rec_int(T_FSEXEC, p, r); return r; }
int64_t __wrap_Nx__fsKind(srs_str p, bool follow) { int64_t r = __real_Nx__fsKind(p, follow); rec_int(follow ? T_FSKIND : T_LSKIND, p, r); return r; }
int64_t __wrap_Nx__timeNow(void) { T.taint = "the clock was read"; return __real_Nx__timeNow(); }
int64_t __wrap_Nx__netOpen(srs_str p) { T.taint = "a socket was opened"; return __real_Nx__netOpen(p); }

// ---- replay ---------------------------------------------------------------------------------------------------------
// the observation made again now equals the recorded one
static bool replay1(const obs* o) {
  srs_str a = {o->a, o->an};
  if (o->op == T_ROOT) {
    struct stat s;
    char* c = cstr(o->a, o->an);
    int bad = lstat(c, &s);
    free(c);
    if (bad) return false;
    int64_t st[5] = {0};
    st_tuple(&s, st);
    st[2] = st[3] = 0;
    return memcmp(st, o->st, sizeof st) == 0;
  }
  if (t_content(o->op)) {
    // never read a FIFO or a device (it could block or be consumed); a recorded read needs a regular file of its size
    struct stat s;
    int e = stat_at(o->a, o->an, &s);
    if (e == 0 && !S_ISREG(s.st_mode) && !S_ISDIR(s.st_mode)) return false;
    if (o->v >= 0 && (e != 0 || !S_ISREG(s.st_mode) || s.st_size != o->v)) return false;
    int64_t st[5] = {0};
    if (o->v >= 0) st_tuple(&s, st);
    if (T.stat && o->has_st && memcmp(st, o->st, sizeof st) == 0) return true;
  }
  srs_str r;
  switch (o->op) {
    case T_ISDIR: return __real_Nx__isDir(a) == o->v;
    case T_FSEXEC: return __real_Nx__fsExec(a) == o->v;
    case T_FSKIND: return __real_Nx__fsKind(a, true) == o->v;
    case T_LSKIND: return __real_Nx__fsKind(a, false) == o->v;
    case T_READFILE: r = __real_Nx__readFile(a); break;
    case T_RTREAD: r = __real_rt_read_file(a); break;
    case T_FILEBYTES: r = __real_Nx__fileBytes(a); break;
    case T_GETENV: r = __real_Nx__getEnv(a); break;
    case T_DIRNAMES: r = __real_Nx__dirNames(a); break;
    case T_READLINK: r = __real_Nx__readLink(a); break;
    case T_CWD: r = __real_Nx__cwd(); break;
    case T_REALPATH: r = __real_Nx__realPath(a); break;
    default: return false;
  }
  if (r.n != o->v) return false;
  if (r.n < 0) return true;
  uint8_t d[32]; dig(r, d);
  if (t_content(o->op)) free((void*)r.p);  // malloc'd by the extern; the other results may be shared (cwd "/", a path)
  return memcmp(d, o->d, 32) == 0;
}

// ---- the entry file: "SNXC0003" | u64 body length | digest(body) | body. body = key | t0 | code | stdout | stderr | obs
// (u64 count, then op u8, has_st u8, arg u32 + bytes, v i64, digest of a string op, st of has_st)
static const char MAGIC[8] = {'S', 'N', 'X', 'C', '0', '0', '0', '3'};

typedef struct { const uint8_t* p; size_t n, i; bool bad; } rd;
static const uint8_t* r_take(rd* r, size_t k) { if (r->bad || r->n - r->i < k) { r->bad = true; return 0; } const uint8_t* q = r->p + r->i; r->i += k; return q; }
static uint64_t r_u64(rd* r) { uint64_t v = 0; const uint8_t* q = r_take(r, 8); if (q) memcpy(&v, q, 8); return v; }

typedef struct { int64_t code; const uint8_t* out; size_t outn; const uint8_t* err; size_t errn; obs* o; size_t n; } entry;

static uint8_t* slurp(const char* path, size_t* n) {
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  buf b = {0}; uint8_t tmp[65536]; size_t k;
  while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) b_add(&b, tmp, k);
  int bad = ferror(f); fclose(f);
  if (bad) { free(b.p); return 0; }
  *n = b.n;
  return b.p ? b.p : t_alloc(1);
}

// the entry at T.path, or why there is none
static const char* load(entry* e, uint8_t** keep) {
  size_t n = 0;
  uint8_t* f = slurp(T.path, &n);
  if (!f) return "no entry";
  *keep = f;
  if (n < 48 || memcmp(f, MAGIC, 8) != 0) return "corrupt entry (header)";
  uint64_t bn; memcpy(&bn, f + 8, 8);
  if (bn != n - 48) return "corrupt entry (length)";
  uint8_t d[32]; dig((srs_str){f + 48, (int64_t)bn}, d);
  if (memcmp(d, f + 16, 32) != 0) return "corrupt entry (digest)";
  rd r = {f + 48, bn, 0, false};
  const uint8_t* k = r_take(&r, 32);
  if (!k || memcmp(k, T.key, 32) != 0) return "corrupt entry (key)";
  r_u64(&r);
  e->code = (int64_t)r_u64(&r);
  e->outn = r_u64(&r); e->out = r_take(&r, e->outn);
  e->errn = r_u64(&r); e->err = r_take(&r, e->errn);
  uint64_t no = r_u64(&r);
  if (r.bad || no > bn / 14) return "corrupt entry (count)";
  e->o = t_alloc(no * sizeof(obs)); e->n = no;
  for (size_t i = 0; i < no; i++) {
    obs* o = &e->o[i];
    const uint8_t* h = r_take(&r, 6);
    if (!h) return "corrupt entry (observation)";
    o->op = h[0]; o->has_st = h[1]; memcpy(&o->an, h + 2, 4);
    o->a = r_take(&r, o->an);
    o->v = (int64_t)r_u64(&r);
    if (t_str(o->op) && o->v >= 0) { const uint8_t* q = r_take(&r, 32); if (q) memcpy(o->d, q, 32); }
    if (o->has_st) for (int j = 0; j < 5; j++) o->st[j] = (int64_t)r_u64(&r);
    if (r.bad || o->op == 0 || o->op >= T_LAST || (o->op > T_ROOT && o->op < T_READFILE) || o->has_st > 1) return "corrupt entry (observation)";
  }
  if (r.i != r.n) return "corrupt entry (trailing bytes)";
  return 0;
}

static bool write_all(int fd, const uint8_t* p, size_t n) {
  while (n > 0) { ssize_t k = write(fd, p, n); if (k < 0) { if (errno == EINTR) continue; return false; } p += k; n -= (size_t)k; }
  return true;
}

static const char* store(int64_t code) {
  buf b = {0};
  b_add(&b, T.key, 32); b_u64(&b, (uint64_t)T.t0); b_u64(&b, (uint64_t)code);
  b_u64(&b, T.out.n); b_add(&b, T.out.p, T.out.n);
  b_u64(&b, T.err.n); b_add(&b, T.err.p, T.err.n);
  size_t m = 0;
  for (size_t i = 0; i < T.n; i++) m += !T.o[i].skip;
  b_u64(&b, m);
  for (size_t i = 0; i < T.n; i++) {
    obs* o = &T.o[i];
    if (o->skip) continue;
    b_add(&b, &o->op, 1); b_add(&b, &o->has_st, 1); b_add(&b, &o->an, 4); b_add(&b, o->a, o->an);
    b_u64(&b, (uint64_t)o->v);
    if (t_str(o->op) && o->v >= 0) b_add(&b, o->d, 32);
    if (o->has_st) for (int j = 0; j < 5; j++) b_u64(&b, (uint64_t)o->st[j]);
  }
  uint8_t d[32]; dig((srs_str){b.p, (int64_t)b.n}, d);
  size_t pl = strlen(T.path);
  char* tmp = t_alloc(pl + 32);
  snprintf(tmp, pl + 32, "%s.tmp%ld", T.path, (long)getpid());
  FILE* f = fopen(tmp, "wb");
  if (!f) { free(b.p); free(tmp); return "cannot write the entry"; }
  uint64_t bn = b.n;
  bool ok = fwrite(MAGIC, 1, 8, f) == 8 && fwrite(&bn, 1, 8, f) == 8 && fwrite(d, 1, 32, f) == 32 && fwrite(b.p, 1, b.n, f) == b.n;
  ok = fclose(f) == 0 && ok;
  ok = ok && rename(tmp, T.path) == 0;
  if (!ok) unlink(tmp);
  free(b.p); free(tmp);
  return ok ? 0 : "cannot write the entry";
}

// ---- output capture: stdout and stderr still go to fds 1 and 2, and are kept for the entry --------------------------
typedef struct { int fd; buf* b; } tee;
static ssize_t tee_write(void* c, const char* p, size_t n) {
  tee* t = c;
  if (!write_all(t->fd, (const uint8_t*)p, n)) return -1;
  b_add(t->b, p, n);
  return (ssize_t)n;
}
static FILE* tee_open(int fd, buf* b, int mode) {
  tee* t = t_alloc(sizeof *t); t->fd = fd; t->b = b;
  FILE* f = fopencookie(t, "w", (cookie_io_functions_t){0, tee_write, 0, 0});
  if (f) setvbuf(f, 0, mode, mode == _IONBF ? 0 : 65536);
  return f;
}

// the key: the executable's bytes, argv, the working directory and the variables the C runtime reads itself
static bool make_key(void) {
  buf b = {0};
  // the executable: its path and (dev, ino, size, mtime_ns); a rebuilt or replaced binary differs in the tuple
  char exe[4096];
  ssize_t el = readlink("/proc/self/exe", exe, sizeof exe);
  struct stat es;
  if (el <= 0 || (size_t)el >= sizeof exe || stat("/proc/self/exe", &es) != 0) { free(b.p); return false; }
  int64_t et[5];
  st_tuple(&es, et);
  b_add(&b, "snx-cache-4", 12); b_u64(&b, (uint64_t)el); b_add(&b, exe, (size_t)el);
  for (int j = 0; j < 4; j++) b_u64(&b, (uint64_t)et[j]);
  int64_t ac = rt_argc();
  b_u64(&b, (uint64_t)ac);
  for (int64_t i = 0; i < ac; i++) { srs_str a = rt_arg(i); b_u64(&b, (uint64_t)a.n); b_add(&b, a.p, (size_t)a.n); }
  char cwd[4096];
  if (!getcwd(cwd, sizeof cwd)) { free(b.p); return false; }
  b_u64(&b, strlen(cwd)); b_add(&b, cwd, strlen(cwd));
  struct rlimit rl;  // the stack limit the evaluator runs under (after the big-stack re-exec): a smaller one can crash what a hit answers
  b_u64(&b, getrlimit(RLIMIT_STACK, &rl) == 0 ? (uint64_t)rl.rlim_cur : 0);
  static const char* env[] = {"SNX_PROBE", "SNX_RELSEQ", "SNX_SHA_PORTABLE", "SNX_CACHE_STORE", "SNX_STACK_SET", "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT"};
  for (size_t i = 0; i < sizeof env / sizeof *env; i++) {
    const char* v = getenv(env[i]);
    b_u64(&b, v ? strlen(v) + 1 : 0); if (v) b_add(&b, v, strlen(v));
  }
  sha((srs_str){b.p, (int64_t)b.n}, T.key); free(b.p);
  return true;
}

static bool out_end(void);

// an exit before Nx__cacheEnd (exit(70) on out of memory): fd 1 goes back and the captured bytes are written; nothing is stored.
// srs's own atexit flush was registered later, so it has already run
static void out_atexit(void) {
  if (!T.on) return;
  T.on = false;
  fflush(stdout); rt_out_flush(); out_end();
}

// -1: evaluate (recording when SNX_CACHE is set); else the exit code of a stored run whose output was just written
int64_t Nx__cacheBegin(void) {
  const char* dir = getenv("SNX_CACHE");
  if (!dir || !*dir) return -1;
  const char* v;
  T.dbg = (v = getenv("SNX_CACHE_DEBUG")) && (!strcmp(v, "1") || !strcmp(v, "2"));
  bool prof = v && !strcmp(v, "2");
  T.stat = (v = getenv("SNX_CACHE_STAT")) && !strcmp(v, "1");
  T.store = (v = getenv("SNX_CACHE_STORE")) && *v ? v : "/nix/store";
  T.sl = strlen(T.store);
  int64_t t_key = now_ns();
  if (!make_key()) { dbg("skip", "no key", 0); return -1; }
  t_key = now_ns() - t_key;
  size_t dl = strlen(dir);
  T.path = t_alloc(dl + 66);
  memcpy(T.path, dir, dl); T.path[dl] = '/';
  for (int i = 0; i < 32; i++) snprintf(T.path + dl + 1 + 2 * i, 3, "%02x", T.key[i]);
  entry e = {0}; uint8_t* keep = 0;
  int64_t t_load = now_ns();
  const char* why = load(&e, &keep);
  t_load = now_ns() - t_load;
  if (prof) { char l[128]; int k = snprintf(l, sizeof l, "snx-cache: key %.1f ms, load %.1f ms\n", t_key / 1e6, t_load / 1e6); if (write(2, l, (size_t)k) < 0) return -1; }
  if (!why) {
    size_t i = 0;
    // SNX_CACHE_DEBUG=2: per op, the count, result bytes and time of the replay
    int64_t pn[T_LAST] = {0}, pb[T_LAST] = {0}, pt[T_LAST] = {0};
    while (i < e.n) {
      int64_t t = prof ? now_ns() : 0;
      bool same = replay1(&e.o[i]);
      if (prof) { int op = e.o[i].op; pn[op]++; pb[op] += e.o[i].v > 0 ? e.o[i].v : 0; pt[op] += now_ns() - t; }
      if (!same) break;
      i++;
    }
    for (int op = 1; prof && op < T_LAST; op++)
      if (pn[op]) { char l[128]; int k = snprintf(l, sizeof l, "snx-cache: op %d: %ld obs, %ld bytes, %.1f ms\n", op, (long)pn[op], (long)pb[op], pt[op] / 1e6); if (write(2, l, (size_t)k) < 0) break; }
    if (i == e.n) {
      bool ok = write_all(1, e.out, e.outn) && write_all(2, e.err, e.errn);
      dbg("hit", ok ? "" : "(write failed)", e.n);
      if (ok) return e.code;
      why = "output write failed";
    } else why = "an observation differs";
  }
  dbg("miss", why, e.n);
  free(e.o); free(keep);
  mkdir(dir, 0777);
  T.t0 = now_ns();
  FILE* er = tee_open(2, &T.err, _IONBF);
  if (!er) { dbg("skip", "no capture", 0); return -1; }
  // fd 1 becomes a memfd: srs writes stdout with write(1) at its flush points, so a FILE tee on stdout sees none of it
  T.saved_fd = dup(1);
  T.out_fd = memfd_create("snx-stdout", 0);
  if (T.saved_fd < 0 || T.out_fd < 0 || dup2(T.out_fd, 1) < 0) {
    if (T.saved_fd >= 0) close(T.saved_fd);
    if (T.out_fd >= 0) close(T.out_fd);
    dbg("skip", "no capture", 0); return -1;
  }
  fflush(stdout); stderr = er;
  T.on = true;
  static bool hooked;
  if (!hooked) { hooked = true; atexit(out_atexit); }
  return -1;
}

// fd 1 back to the saved fd; the bytes the run wrote to the memfd go to T.out, then to fd 1
static bool out_end(void) {
  bool ok = lseek(T.out_fd, 0, SEEK_SET) == 0;
  uint8_t tmp[65536];
  while (ok) {
    ssize_t k = read(T.out_fd, tmp, sizeof tmp);
    if (k == 0) break;
    if (k < 0) { if (errno == EINTR) continue; ok = false; break; }
    b_add(&T.out, tmp, (size_t)k);
  }
  ok = dup2(T.saved_fd, 1) >= 0 && ok;
  close(T.saved_fd); close(T.out_fd);
  return ok && write_all(1, T.out.p, T.out.n);
}

// code, after the run is stored when it may be
int64_t Nx__cacheEnd(int64_t code) {
  if (!T.on) return code;
  fflush(stdout); rt_out_flush(); fflush(stderr);
  T.on = false;
  if (!out_end()) { dbg("skip", "cannot write stdout", T.n); return 70; }
  const char* why = code != 0 ? "the run failed" : T.taint ? T.taint : store(code);
  if (T.dbg) { char l[96]; int k = snprintf(l, sizeof l, "snx-cache: %zu observation calls\n", T.calls); if (write(2, l, (size_t)k) < 0) return code; }
  if (why) dbg("skip", why, T.n); else dbg("store", T.path, T.n);
  return code;
}
