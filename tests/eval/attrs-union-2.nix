# layered sets: x // y kept as leaves (nx/eval.srs Nx.update), against nix-instantiate --eval --strict
let
  b = builtins.listToAttrs (map (i: { name = "k${toString i}"; value = i; }) (builtins.genList (x: x) 30));
  step = s: i: s // { "k${toString (builtins.bitAnd (i * 7) 31)}" = i; "n${toString (builtins.bitAnd i 3)}" = i; };
  r = builtins.foldl' step b (builtins.genList (i: i) 100);
in [ (builtins.attrNames r) (builtins.attrValues r) r.k7 r.n2 (r ? k31) (r ? k40) ]
