# the word arena: a lambda call releases its words only when nothing it allocated stays reachable (rt/snx_rt.c)
let mk = n: { v = n; }; cells = builtins.genList (i: (mk i).v) 300; k = x: builtins.elemAt cells x; in builtins.foldl' (a: i: a + k i + k (299 - i)) 0 (builtins.genList (i: i) 300)
