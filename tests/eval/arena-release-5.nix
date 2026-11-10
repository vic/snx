# the word arena: a lambda call releases its words only when nothing it allocated stays reachable (rt/snx_rt.c)
let f = n: if n == 0 then 0 else (let a = { x = n; }; in a.x) + f (n - 1); big = builtins.genList (i: { z = i; }) 50; g = i: (builtins.elemAt big i).z; in f 2000 + builtins.foldl' (a: i: a + g i) 0 (builtins.genList (i: i) 50) + (builtins.elemAt big 7).z
