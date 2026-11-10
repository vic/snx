# attribute sets: build, update, select (n forced each step: no thunk chain)
let
  step = s: i: let n = s.n + 1; in builtins.seq n (s // { "k${toString (builtins.bitAnd i 255)}" = i; inherit n; });
  r = builtins.foldl' step { n = 0; } (builtins.genList (i: i) 200000);
in r.n + r.k7
