# layered sets: x // y kept as leaves (nx/eval.srs Nx.update), against nix-instantiate --eval --strict
let
  big = builtins.listToAttrs (map (i: { name = "a${toString i}"; value = i; }) (builtins.genList (x: x) 12));
  u = big // { x = 1; };
  s = big // { __toString = self: "hi${toString self.a1}"; };
  f = big // { __functor = self: x: x + self.a2; };
  o = big // { outPath = "/nix/store/x"; };
in [ [ u u ] (toString s) "${s}" (f 5) (with (big // { zz = 3; }); zz + a1) (({ a1, a2, ... }: a1 + a2) (big // { a1 = 10; }))
     "${o}" (builtins.deepSeq u 1) (builtins.seq (big // { y = throw "no"; }) 2) ((big // { a = { b = 1; }; } // { c = 2; }).a.b) ]
