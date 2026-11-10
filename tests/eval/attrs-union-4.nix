# layered sets: x // y kept as leaves (nx/eval.srs Nx.update), against nix-instantiate --eval --strict
let
  big = builtins.listToAttrs (map (i: { name = "a${toString i}"; value = i; }) (builtins.genList (x: x) 10));
  u = big // { x = 1; } // { y = 2; };
  v = big // { x = 1; } // { y = 2; };
in [ (u == v) (u == big) (big // { } == big) ({ } // u == u) (builtins.attrNames (u // big)) (builtins.toJSON u)
     (builtins.catAttrs "x" [ u { x = 3; } ]) (builtins.concatMap builtins.attrNames [ u ]) (rec { a = u.x; b = a; }).b ]
