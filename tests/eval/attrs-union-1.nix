# layered sets: x // y kept as leaves (nx/eval.srs Nx.update), against nix-instantiate --eval --strict
let
  b = builtins.listToAttrs (map (i: { name = "a${toString i}"; value = i; }) (builtins.genList (x: x) 20));
  u = b // { a3 = 100; z = 1; } // { a3 = 200; } // { q = 5; };
in [ u.a3 u.a4 u.z u.q (u ? a3) (u ? nope) (u.nope or 7) (builtins.attrNames u == builtins.attrNames (u // { }))
     (builtins.length (builtins.attrNames u)) (u == (b // { a3 = 200; z = 1; q = 5; })) (builtins.typeOf u) (builtins.isAttrs u)
     (builtins.getAttr "a3" u) (builtins.hasAttr "q" u) (builtins.attrValues (removeAttrs u [ "a1" "a2" "z" ]))
     (builtins.toJSON (builtins.intersectAttrs { a3 = 0; q = 0; } u)) (builtins.mapAttrs (n: v: v + 1) (b // { a0 = 50; })) ]
