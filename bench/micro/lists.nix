# lists: genList, map, filter, foldl'
let xs = builtins.genList (i: i) 1000000;
    ys = map (x: x * 3) xs;
    zs = builtins.filter (x: builtins.bitAnd x 1 == 0) ys;
in builtins.foldl' (a: b: a + b) 0 zs
