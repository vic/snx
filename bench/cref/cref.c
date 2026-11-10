// cref: throwaway C reference of the snx eval-core design (vic/notes/20261110T1400).
// Cells = holder words; frames = parent + cell pointers; thunks VPending(env, code)
// forced in place with a blackhole; locals pre-resolved to (up, idx); attrsets sorted
// by symbol id (<=8 linear scan, else binary search); primops by id.
// -DREPR_B: 61-bit immediate ints (tag 001) + pointers. Default (A): every value boxed.
// -DNOMEMO: negative control, force never writes the result back.
// usage: cref {fib|attrs|lists|fixpoint} [fib-n]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static char *hp;
static inline void *alloc(size_t n) {
  void *p = hp;
  hp += (n + 7) & ~(size_t)7;
  return p;
}
static _Noreturn void die(const char *m) {
  fprintf(stderr, "error: %s\n", m);
  exit(1);
}

enum { T_INT, T_BOOL, T_NULL, T_STR, T_LIST, T_ATTRS, T_LAMBDA, T_PRIM, T_PRIMAPP,
       T_PENDING, T_APP, T_APP2, T_BUSY };

typedef struct Obj Obj;
typedef struct Cell Cell;
typedef struct Frame Frame;
typedef struct Expr Expr;
typedef struct { int64_t sym; Cell *c; } Attr;

#ifdef REPR_B
typedef uintptr_t V;
#define ISINT(v) ((v) & 1)
#define INTOF(v) ((int64_t)(v) >> 3)
#define MKINT(i) ((((uint64_t)(i)) << 3) | 1)
#define OBJ(v) ((Obj *)(v))
#define MKOBJ(o) ((V)(o))
#define TAG(v) (ISINT(v) ? T_INT : OBJ(v)->tag)
#define INT_MAX61 (((int64_t)1 << 60) - 1)
#define INT_MIN61 (-((int64_t)1 << 60))
#else
typedef Obj *V;
#define ISINT(v) ((v)->tag == T_INT)
#define INTOF(v) ((v)->i)
#define OBJ(v) (v)
#define MKOBJ(o) (o)
#define TAG(v) ((v)->tag)
#endif

struct Obj {
  int64_t tag;
  union {
    int64_t i;
    struct { int64_t len; const char *s; } str;
    struct { int64_t n; Cell **it; } list;
    struct { int64_t n; Attr *a; } attrs;
    struct { Frame *env; Expr *body; } lam;
    struct { int32_t id, arity, nargs; Cell **args; } prim;
    struct { Frame *env; Expr *code; } pend;
    struct { Cell *f, *a, *b; } app;
  };
};
struct Cell { V v; };
struct Frame { Frame *up; Cell *s[]; };

#ifndef REPR_B
static V MKINT(int64_t i) {
  Obj *o = alloc(16);  // srs-like: boxed int, tag + payload
  o->tag = T_INT;
  o->i = i;
  return o;
}
#endif

static Obj o_true = {T_BOOL, {.i = 1}}, o_false = {T_BOOL, {.i = 0}}, o_busy = {T_BUSY, {0}};
#define VTRUE MKOBJ(&o_true)
#define VFALSE MKOBJ(&o_false)
#define VBOOL(b) ((b) ? VTRUE : VFALSE)

static inline Obj *newobj(int64_t tag) {
  Obj *o = alloc(sizeof(Obj));
  o->tag = tag;
  return o;
}
static inline Cell *newcell(V v) {
  Cell *c = alloc(sizeof(Cell));
  c->v = v;
  return c;
}
static V mkstr(const char *s, int64_t len) {
  Obj *o = newobj(T_STR);
  o->str.len = len;
  o->str.s = s;
  return MKOBJ(o);
}

// ---- symbols: open-addressing intern table
#define SYMCAP (1 << 20)
static struct { const char *s; int64_t len; int64_t id; } symtab[SYMCAP];
static V symstr[SYMCAP];
static int64_t nsyms;
static int64_t intern(const char *s, int64_t len) {
  uint64_t h = 1469598103934665603ull;
  for (int64_t i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 1099511628211ull;
  for (uint64_t j = h & (SYMCAP - 1);; j = (j + 1) & (SYMCAP - 1)) {
    if (!symtab[j].s) {
      char *d = alloc(len + 1);
      memcpy(d, s, len);
      d[len] = 0;
      symtab[j].s = d;
      symtab[j].len = len;
      symtab[j].id = nsyms;
      symstr[nsyms] = mkstr(d, len);
      return nsyms++;
    }
    if (symtab[j].len == len && memcmp(symtab[j].s, s, len) == 0) return symtab[j].id;
  }
}
static int64_t sym(const char *s) { return intern(s, (int64_t)strlen(s)); }

// ---- IR
enum { K_CONST, K_LOCAL, K_LAM, K_CALL, K_LET, K_IF, K_ADD, K_SUB, K_MUL, K_LT, K_EQ,
       K_UPD, K_SEL, K_SELD, K_HAS, K_ATTRS, K_CAT };
struct Expr {
  int32_t k, n;
  int32_t up, idx;
  int64_t i;           // symbol
  Expr *a, *b, *c;
  Expr **xs;           // call args / let binds / attr values / cat parts
  Expr **names;        // attrs: dynamic name expr or NULL
  int64_t *syms;       // attrs: static symbol (sorted when all static)
  Cell *cell;          // const
};
static Expr *mk(int k) {
  Expr *e = calloc(1, sizeof(Expr));
  e->k = k;
  return e;
}

// ---- primops
enum { P_GENLIST, P_MAP, P_FILTER, P_FOLDL, P_BITAND, P_SEQ, P_LISTTOATTRS, P_MAPATTRS,
       P_ATTRNAMES, P_TOSTRING, P_N };
static const int prim_arity[P_N] = {2, 2, 2, 3, 2, 2, 1, 2, 1, 1};

static V eval(Frame *env, Expr *e);
static V call(V f, int n, Cell **args);
static V prim(int id, Cell **args);

#ifdef NOMEMO
#define MEMO(c, saved, r) ((c)->v = (saved))
#else
#define MEMO(c, saved, r) ((c)->v = (r))
#endif

static V force(Cell *c) {
  V v = c->v;
  if (ISINT(v)) return v;
  Obj *o = OBJ(v);
  if (o->tag < T_PENDING) return v;
  V r;
  switch (o->tag) {
  case T_PENDING:
    c->v = MKOBJ(&o_busy);
    r = eval(o->pend.env, o->pend.code);
    break;
  case T_APP: {
    c->v = MKOBJ(&o_busy);
    Cell *as[1] = {o->app.a};
    r = call(force(o->app.f), 1, as);
    break;
  }
  case T_APP2: {
    c->v = MKOBJ(&o_busy);
    Cell *as[2] = {o->app.a, o->app.b};
    r = call(force(o->app.f), 2, as);
    break;
  }
  default:
    die("infinite recursion encountered");
  }
  MEMO(c, v, r);
  return r;
}

static inline Cell *lookup(Frame *env, Expr *e) {
  for (int u = e->up; u; u--) env = env->up;
  return env->s[e->idx];
}

// Lazy position: local -> the frame cell itself; const -> new cell w/ value; else VPending.
static inline Cell *thunk(Frame *env, Expr *e) {
  if (e->k == K_LOCAL) return lookup(env, e);
  if (e->k == K_CONST) return newcell(e->cell->v);
  Obj *o = newobj(T_PENDING);
  o->pend.env = env;
  o->pend.code = e;
  return newcell(MKOBJ(o));
}

static inline int64_t want_int(V v) {
  if (!ISINT(v)) die("value is not an integer");
  return INTOF(v);
}
static inline Obj *want(V v, int64_t tag, const char *m) {
  if (TAG(v) != tag) die(m);
  return OBJ(v);
}
static inline V mkint_chk(int64_t r, int ovf) {
#ifdef REPR_B
  if (ovf || r > INT_MAX61 || r < INT_MIN61) die("integer overflow");
#else
  if (ovf) die("integer overflow");
#endif
  return MKINT(r);
}

static Cell *find(Obj *s, int64_t k) {
  Attr *a = s->attrs.a;
  int64_t n = s->attrs.n;
  if (n <= 8) {
    for (int64_t i = 0; i < n; i++)
      if (a[i].sym == k) return a[i].c;
    return NULL;
  }
  int64_t lo = 0, hi = n;
  while (lo < hi) {
    int64_t m = (lo + hi) >> 1;
    if (a[m].sym < k) lo = m + 1;
    else hi = m;
  }
  return lo < n && a[lo].sym == k ? a[lo].c : NULL;
}

static Obj *newattrs(int64_t n) {
  Obj *o = newobj(T_ATTRS);
  o->attrs.n = n;
  o->attrs.a = alloc(sizeof(Attr) * (size_t)n);
  return o;
}

static V update(V va, V vb) {
  Obj *a = want(va, T_ATTRS, "value is not a set"), *b = want(vb, T_ATTRS, "value is not a set");
  if (b->attrs.n == 0) return va;
  if (a->attrs.n == 0) return vb;
  Obj *o = newattrs(a->attrs.n + b->attrs.n);
  Attr *x = a->attrs.a, *y = b->attrs.a, *d = o->attrs.a;
  int64_t i = 0, j = 0, k = 0;
  while (i < a->attrs.n && j < b->attrs.n) {
    if (x[i].sym < y[j].sym) d[k++] = x[i++];
    else if (x[i].sym > y[j].sym) d[k++] = y[j++];
    else { d[k++] = y[j++]; i++; }
  }
  while (i < a->attrs.n) d[k++] = x[i++];
  while (j < b->attrs.n) d[k++] = y[j++];
  o->attrs.n = k;
  return MKOBJ(o);
}

static V eval(Frame *env, Expr *e) {
  switch (e->k) {
  case K_CONST:
    return force(e->cell);
  case K_LOCAL:
    return force(lookup(env, e));
  case K_LAM: {
    Obj *o = newobj(T_LAMBDA);
    o->lam.env = env;
    o->lam.body = e->a;
    return MKOBJ(o);
  }
  case K_CALL: {
    V f = eval(env, e->a);
    Cell *as[3];
    for (int i = 0; i < e->n; i++) as[i] = thunk(env, e->xs[i]);
    return call(f, e->n, as);
  }
  case K_LET: {
    Frame *fr = alloc(sizeof(Frame) + sizeof(Cell *) * (size_t)e->n);
    fr->up = env;
    for (int i = 0; i < e->n; i++) fr->s[i] = thunk(fr, e->xs[i]);
    return eval(fr, e->a);
  }
  case K_IF: {
    V c = eval(env, e->a);
    want(c, T_BOOL, "value is not a Boolean");
    return eval(env, OBJ(c)->i ? e->b : e->c);
  }
  case K_ADD: case K_SUB: case K_MUL: {
    int64_t x = want_int(eval(env, e->a)), y = want_int(eval(env, e->b)), r;
    int ovf = e->k == K_ADD ? __builtin_add_overflow(x, y, &r)
            : e->k == K_SUB ? __builtin_sub_overflow(x, y, &r)
                            : __builtin_mul_overflow(x, y, &r);
    return mkint_chk(r, ovf);
  }
  case K_LT:
    return VBOOL(want_int(eval(env, e->a)) < want_int(eval(env, e->b)));
  case K_EQ: {
    V x = eval(env, e->a), y = eval(env, e->b);
    if (ISINT(x) && ISINT(y)) return VBOOL(INTOF(x) == INTOF(y));
    die("== only on ints in cref");
  }
  case K_UPD:
    return update(eval(env, e->a), eval(env, e->b));
  case K_SEL: case K_SELD: case K_HAS: {
    Obj *s = want(eval(env, e->a), T_ATTRS, "value is not a set");
    int64_t k = e->i;
    if (e->k == K_SELD) {
      Obj *n = want(eval(env, e->b), T_STR, "value is not a string");
      k = intern(n->str.s, n->str.len);
    }
    Cell *c = find(s, k);
    if (e->k == K_HAS) return VBOOL(c != NULL);
    if (!c) die("attribute missing");
    return force(c);
  }
  case K_ATTRS: {
    Obj *o = newattrs(e->n);
    Attr *a = o->attrs.a;
    int dyn = 0;
    for (int i = 0; i < e->n; i++) {
      if (e->names[i]) {
        Obj *n = want(eval(env, e->names[i]), T_STR, "value is not a string");
        a[i].sym = intern(n->str.s, n->str.len);
        dyn = 1;
      } else a[i].sym = e->syms[i];
      a[i].c = thunk(env, e->xs[i]);
    }
    if (dyn)  // insertion sort: dynamic attrs are few per node
      for (int i = 1; i < e->n; i++)
        for (int j = i; j > 0 && a[j - 1].sym > a[j].sym; j--) {
          Attr t = a[j]; a[j] = a[j - 1]; a[j - 1] = t;
        }
    return MKOBJ(o);
  }
  case K_CAT: {
    V ps[8];
    int64_t len = 0;
    for (int i = 0; i < e->n; i++) {
      ps[i] = eval(env, e->xs[i]);
      len += want(ps[i], T_STR, "cannot coerce to string")->str.len;
    }
    char *d = alloc(len + 1), *p = d;
    for (int i = 0; i < e->n; i++) {
      memcpy(p, OBJ(ps[i])->str.s, OBJ(ps[i])->str.len);
      p += OBJ(ps[i])->str.len;
    }
    *p = 0;
    return mkstr(d, len);
  }
  }
  die("bad expr");
  return VFALSE;
}

static V call(V f, int n, Cell **args) {
  for (;;) {
    int64_t t = TAG(f);
    if (t == T_LAMBDA) {
      Frame *fr = alloc(sizeof(Frame) + sizeof(Cell *));
      fr->up = OBJ(f)->lam.env;
      fr->s[0] = args[0];
      f = eval(fr, OBJ(f)->lam.body);
      args++, n--;
    } else if (t == T_PRIM || t == T_PRIMAPP) {
      Obj *p = OBJ(f);
      int have = t == T_PRIMAPP ? p->prim.nargs : 0, need = p->prim.arity - have;
      if (n < need) {
        Obj *q = newobj(T_PRIMAPP);
        q->prim = p->prim;
        q->prim.nargs = have + n;
        q->prim.args = alloc(sizeof(Cell *) * (size_t)(have + n));
        for (int i = 0; i < have; i++) q->prim.args[i] = p->prim.args[i];
        for (int i = 0; i < n; i++) q->prim.args[have + i] = args[i];
        return MKOBJ(q);
      }
      if (have == 0) f = prim(p->prim.id, args);
      else {
        Cell *all[3];
        for (int i = 0; i < have; i++) all[i] = p->prim.args[i];
        for (int i = 0; i < need; i++) all[have + i] = args[i];
        f = prim(p->prim.id, all);
      }
      args += need, n -= need;
    } else die("attempt to call something which is not a function");
    if (n == 0) return f;
  }
}

static Obj *newlist(int64_t n) {
  Obj *o = newobj(T_LIST);
  o->list.n = n;
  o->list.it = alloc(sizeof(Cell *) * (size_t)n);
  return o;
}
static Cell *app(int64_t tag, Cell *f, Cell *a, Cell *b) {
  Obj *o = newobj(tag);
  o->app.f = f;
  o->app.a = a;
  o->app.b = b;
  return newcell(MKOBJ(o));
}
typedef struct { int64_t sym, ord; Cell *c; } SymOrd;
static int cmp_symord(const void *x, const void *y) {
  const SymOrd *a = x, *b = y;
  return a->sym != b->sym ? (a->sym < b->sym ? -1 : 1) : (a->ord < b->ord ? -1 : a->ord > b->ord);
}
static int cmp_name(const void *x, const void *y) {
  Obj *a = OBJ((*(Cell *const *)x)->v), *b = OBJ((*(Cell *const *)y)->v);
  int64_t n = a->str.len < b->str.len ? a->str.len : b->str.len;
  int r = memcmp(a->str.s, b->str.s, (size_t)n);
  return r ? r : (a->str.len > b->str.len) - (a->str.len < b->str.len);
}

static V prim(int id, Cell **args) {
  switch (id) {
  case P_GENLIST: {
    int64_t n = want_int(force(args[1]));
    Obj *l = newlist(n);
    for (int64_t i = 0; i < n; i++) l->list.it[i] = app(T_APP, args[0], newcell(MKINT(i)), NULL);
    return MKOBJ(l);
  }
  case P_MAP: {
    Obj *xs = want(force(args[1]), T_LIST, "value is not a list"), *l = newlist(xs->list.n);
    for (int64_t i = 0; i < xs->list.n; i++) l->list.it[i] = app(T_APP, args[0], xs->list.it[i], NULL);
    return MKOBJ(l);
  }
  case P_FILTER: {
    V f = force(args[0]);
    Obj *xs = want(force(args[1]), T_LIST, "value is not a list"), *l = newlist(xs->list.n);
    int64_t k = 0;
    for (int64_t i = 0; i < xs->list.n; i++) {
      V b = call(f, 1, &xs->list.it[i]);
      if (OBJ(want(b, T_BOOL, "value is not a Boolean"))->i) l->list.it[k++] = xs->list.it[i];
    }
    l->list.n = k;
    return MKOBJ(l);
  }
  case P_FOLDL: {
    V f = force(args[0]);
    Obj *xs = want(force(args[2]), T_LIST, "value is not a list");
    Cell *acc = args[1];
    for (int64_t i = 0; i < xs->list.n; i++) {
      Cell *as[2] = {acc, xs->list.it[i]};
      acc = newcell(call(f, 2, as));
    }
    return force(acc);
  }
  case P_BITAND:
    return MKINT(want_int(force(args[0])) & want_int(force(args[1])));
  case P_SEQ:
    force(args[0]);
    return force(args[1]);
  case P_TOSTRING: {
    V v = force(args[0]);
    if (TAG(v) == T_STR) return v;
    char *d = alloc(24);
    return mkstr(d, snprintf(d, 24, "%lld", (long long)want_int(v)));
  }
  case P_LISTTOATTRS: {
    Obj *xs = want(force(args[0]), T_LIST, "value is not a list");
    int64_t nm = sym("name"), vl = sym("value"), n = xs->list.n;
    SymOrd *t = malloc(sizeof(SymOrd) * (size_t)(n ? n : 1));
    for (int64_t i = 0; i < n; i++) {
      Obj *e = want(force(xs->list.it[i]), T_ATTRS, "value is not a set");
      Cell *cn = find(e, nm), *cv = find(e, vl);
      if (!cn || !cv) die("attribute missing");
      Obj *s = want(force(cn), T_STR, "value is not a string");
      t[i] = (SymOrd){intern(s->str.s, s->str.len), i, cv};
    }
    qsort(t, (size_t)n, sizeof(SymOrd), cmp_symord);
    Obj *o = newattrs(n);
    int64_t k = 0;
    for (int64_t i = 0; i < n; i++)
      if (k == 0 || o->attrs.a[k - 1].sym != t[i].sym) o->attrs.a[k++] = (Attr){t[i].sym, t[i].c};
    o->attrs.n = k;
    free(t);
    return MKOBJ(o);
  }
  case P_MAPATTRS: {
    Obj *s = want(force(args[1]), T_ATTRS, "value is not a set"), *o = newattrs(s->attrs.n);
    for (int64_t i = 0; i < s->attrs.n; i++)
      o->attrs.a[i] = (Attr){s->attrs.a[i].sym,
                             app(T_APP2, args[0], newcell(symstr[s->attrs.a[i].sym]), s->attrs.a[i].c)};
    return MKOBJ(o);
  }
  case P_ATTRNAMES: {
    Obj *s = want(force(args[0]), T_ATTRS, "value is not a set"), *l = newlist(s->attrs.n);
    for (int64_t i = 0; i < s->attrs.n; i++) l->list.it[i] = newcell(symstr[s->attrs.a[i].sym]);
    qsort(l->list.it, (size_t)l->list.n, sizeof(Cell *), cmp_name);
    return MKOBJ(l);
  }
  }
  die("bad primop");
  return VFALSE;
}

// ---- hand-built IR constructors
static Expr *L(int up, int idx) { Expr *e = mk(K_LOCAL); e->up = up; e->idx = idx; return e; }
static Expr *C(V v) { Expr *e = mk(K_CONST); e->cell = newcell(v); return e; }
static Expr *I(int64_t i) { return C(MKINT(i)); }
static Expr *S(const char *s) { return C(mkstr(s, (int64_t)strlen(s))); }
static Expr *B(int id) {
  Obj *o = newobj(T_PRIM);
  o->prim.id = id;
  o->prim.arity = prim_arity[id];
  return C(MKOBJ(o));
}
static Expr *LAM(Expr *body) { Expr *e = mk(K_LAM); e->a = body; return e; }
static Expr **vec(int n, Expr **src) {
  Expr **v = calloc((size_t)n, sizeof(Expr *));
  memcpy(v, src, sizeof(Expr *) * (size_t)n);
  return v;
}
#define ARR(...) ((Expr *[]){__VA_ARGS__}), (int)(sizeof((Expr *[]){__VA_ARGS__}) / sizeof(Expr *))
static Expr *CALLv(Expr *f, Expr **xs, int n) { Expr *e = mk(K_CALL); e->a = f; e->n = n; e->xs = vec(n, xs); return e; }
#define CALL(f, ...) CALLv(f, ARR(__VA_ARGS__))
static Expr *LETv(Expr *body, Expr **xs, int n) { Expr *e = mk(K_LET); e->a = body; e->n = n; e->xs = vec(n, xs); return e; }
#define LET(body, ...) LETv(body, ARR(__VA_ARGS__))
static Expr *CATv(Expr **xs, int n) { Expr *e = mk(K_CAT); e->n = n; e->xs = vec(n, xs); return e; }
#define CAT(...) CATv(ARR(__VA_ARGS__))
static Expr *BIN(int k, Expr *a, Expr *b) { Expr *e = mk(k); e->a = a; e->b = b; return e; }
static Expr *IF(Expr *c, Expr *t, Expr *f) { Expr *e = mk(K_IF); e->a = c; e->b = t; e->c = f; return e; }
static Expr *SEL(Expr *a, const char *s) { Expr *e = mk(K_SEL); e->a = a; e->i = sym(s); return e; }
static Expr *SELD(Expr *a, Expr *n) { Expr *e = mk(K_SELD); e->a = a; e->b = n; return e; }
static Expr *HAS(Expr *a, const char *s) { Expr *e = mk(K_HAS); e->a = a; e->i = sym(s); return e; }
// attrs from (name-or-NULL, dyn-name-or-NULL, value) triples; static ones sorted by symbol id
typedef struct { const char *name; Expr *dyn, *val; } Bind;
static Expr *ATTRSv(Bind *bs, int n) {
  Expr *e = mk(K_ATTRS);
  e->n = n;
  e->xs = calloc((size_t)n, sizeof(Expr *));
  e->names = calloc((size_t)n, sizeof(Expr *));
  e->syms = calloc((size_t)n, sizeof(int64_t));
  for (int i = 0; i < n; i++) {
    e->xs[i] = bs[i].val;
    e->names[i] = bs[i].dyn;
    e->syms[i] = bs[i].name ? sym(bs[i].name) : -1;
  }
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && !e->names[j] && !e->names[j - 1] && e->syms[j - 1] > e->syms[j]; j--) {
      int64_t ts = e->syms[j]; e->syms[j] = e->syms[j - 1]; e->syms[j - 1] = ts;
      Expr *tx = e->xs[j]; e->xs[j] = e->xs[j - 1]; e->xs[j - 1] = tx;
    }
  return e;
}
#define ATTRS(...) ATTRSv((Bind[]){__VA_ARGS__}, (int)(sizeof((Bind[]){__VA_ARGS__}) / sizeof(Bind)))

// let fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2); in fib 27
static Expr *p_fib(int64_t n) {
  Expr *fib = LAM(IF(BIN(K_LT, L(0, 0), I(2)), L(0, 0),
                     BIN(K_ADD, CALL(L(1, 0), BIN(K_SUB, L(0, 0), I(1))),
                         CALL(L(1, 0), BIN(K_SUB, L(0, 0), I(2))))));
  return LET(CALL(L(0, 0), I(n)), fib);
}

// let step = s: i: let n = s.n + 1; in builtins.seq n (s // { "k${toString (builtins.bitAnd i 255)}" = i; inherit n; });
//     r = builtins.foldl' step { n = 0; } (builtins.genList (i: i) 200000);
// in r.n + r.k7
static Expr *p_attrs(void) {
  // frames inside step's let: n=(0,0) i=(1,0) s=(2,0)
  Expr *step = LAM(LAM(LET(
      CALL(B(P_SEQ), L(0, 0),
           BIN(K_UPD, L(2, 0),
               ATTRS({NULL, CAT(S("k"), CALL(B(P_TOSTRING), CALL(B(P_BITAND), L(1, 0), I(255)))), L(1, 0)},
                     {"n", NULL, L(0, 0)}))),
      BIN(K_ADD, SEL(L(2, 0), "n"), I(1)))));
  Expr *r = CALL(B(P_FOLDL), L(0, 0), ATTRS({"n", NULL, I(0)}), CALL(B(P_GENLIST), LAM(L(0, 0)), I(200000)));
  return LET(BIN(K_ADD, SEL(L(0, 1), "n"), SEL(L(0, 1), "k7")), step, r);
}

// let xs = builtins.genList (i: i) 1000000; ys = map (x: x * 3) xs;
//     zs = builtins.filter (x: builtins.bitAnd x 1 == 0) ys;
// in builtins.foldl' (a: b: a + b) 0 zs
static Expr *p_lists(void) {
  Expr *xs = CALL(B(P_GENLIST), LAM(L(0, 0)), I(1000000));
  Expr *ys = CALL(B(P_MAP), LAM(BIN(K_MUL, L(0, 0), I(3))), L(0, 0));
  Expr *zs = CALL(B(P_FILTER), LAM(BIN(K_EQ, CALL(B(P_BITAND), L(0, 0), I(1)), I(0))), L(0, 1));
  return LET(CALL(B(P_FOLDL), LAM(LAM(BIN(K_ADD, L(1, 0), L(0, 0)))), I(0), L(0, 2)), xs, ys, zs);
}

// let mk = i: { name = "m${toString i}"; value = { a = i; b = i * 2; }; };
//     set = builtins.listToAttrs (builtins.genList mk 50000);
//     fix = f: let x = f x; in x;
//     cfg = fix (self: builtins.mapAttrs (n: v: v // { c = v.a + (if self ? m0 then self.m0.b else 0); }) set);
// in builtins.foldl' (acc: n: acc + cfg.${n}.c) 0 (builtins.attrNames cfg)
static Expr *p_fixpoint(void) {
  Expr *mkf = LAM(ATTRS({"name", NULL, CAT(S("m"), CALL(B(P_TOSTRING), L(0, 0)))},
                        {"value", NULL, ATTRS({"a", NULL, L(0, 0)}, {"b", NULL, BIN(K_MUL, L(0, 0), I(2))})}));
  Expr *set = CALL(B(P_LISTTOATTRS), CALL(B(P_GENLIST), L(0, 0), I(50000)));
  Expr *fix = LAM(LET(L(0, 0), CALL(L(1, 0), L(0, 0))));
  // inside v: v=(0,0) n=(1,0) self=(2,0)
  Expr *fn = LAM(LAM(BIN(K_UPD, L(0, 0),
                         ATTRS({"c", NULL, BIN(K_ADD, SEL(L(0, 0), "a"),
                                               IF(HAS(L(2, 0), "m0"), SEL(SEL(L(2, 0), "m0"), "b"), I(0)))}))));
  Expr *cfg = CALL(L(0, 2), LAM(CALL(B(P_MAPATTRS), fn, L(1, 1))));
  // inside n: n=(0,0) acc=(1,0) cfg=(2,3)
  Expr *body = CALL(B(P_FOLDL), LAM(LAM(BIN(K_ADD, L(1, 0), SEL(SELD(L(2, 3), L(0, 0)), "c")))), I(0),
                    CALL(B(P_ATTRNAMES), L(0, 3)));
  return LET(body, mkf, set, fix, cfg);
}

int main(int argc, char **argv) {
  hp = mmap(NULL, (size_t)32 << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (hp == MAP_FAILED) die("mmap");
#ifdef HUGE
  madvise(hp, (size_t)32 << 30, MADV_HUGEPAGE);  // no-free arena: 2 MB pages cut fault count 512x
#endif
  if (argc < 2) die("usage: cref {fib|attrs|lists|fixpoint} [fib-n]");
  Expr *p = !strcmp(argv[1], "fib")     ? p_fib(argc > 2 ? atoll(argv[2]) : 27)
          : !strcmp(argv[1], "attrs")   ? p_attrs()
          : !strcmp(argv[1], "lists")   ? p_lists()
          : !strcmp(argv[1], "fixpoint") ? p_fixpoint()
                                        : NULL;
  if (!p) die("unknown program");
  printf("%lld\n", (long long)want_int(eval(NULL, p)));
  return 0;
}
