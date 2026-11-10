# the word arena: a lambda call releases its words only when nothing it allocated stays reachable (rt/snx_rt.c)
let g = n: builtins.length l; l = builtins.genList (i: i * 2) 5; in (g 1) + (builtins.elemAt l 3)
