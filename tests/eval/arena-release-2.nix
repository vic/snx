# the word arena: a lambda call releases its words only when nothing it allocated stays reachable (rt/snx_rt.c)
let f = x: let y = [ x x ]; in builtins.length y; in builtins.foldl' (a: b: a + f b) 0 (builtins.genList (i: i) 1000)
