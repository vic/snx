# the word arena: a lambda call releases its words only when nothing it allocated stays reachable (rt/snx_rt.c)
let s = { a = 1; }; h = n: builtins.seq t n; t = s // { b = 2; }; in h 1 + t.b + t.a
