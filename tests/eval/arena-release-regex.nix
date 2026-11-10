# the regex stack grows inside a call that returns a scalar; the grown stack is kept by the cached regex, so the
# call must not give its words back (rt/snx_rt.c stack discipline, nx/regex.srs Nx.reGrow): else the sets below
# take those words and the next match writes its stack over them
let
  re = "((a|b)(a|b))*c";
  big = builtins.concatStringsSep "" (builtins.genList (i: "ab") 3000) + "c";
  f = s: builtins.match re s == null;
  sets = builtins.genList (i: { x = i; y = [ i i ]; }) 20000;
  w = f "c";
  a = builtins.seq w (builtins.seq big (f big));
  keep = builtins.seq a (builtins.deepSeq sets sets);
  b = builtins.seq keep (f big);
  sum = builtins.foldl' (acc: e: acc + e.x + builtins.elemAt e.y 1) 0 keep;
in [ w a b sum ]
