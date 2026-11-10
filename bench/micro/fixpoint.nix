# a module-system-like fixpoint: rec sets, with, functions over sets
let
  mk = i: { name = "m${toString i}"; value = { a = i; b = i * 2; }; };
  set = builtins.listToAttrs (builtins.genList mk 50000);
  fix = f: let x = f x; in x;
  cfg = fix (self: builtins.mapAttrs (n: v: v // { c = v.a + (if self ? m0 then self.m0.b else 0); }) set);
in builtins.foldl' (acc: n: acc + cfg.${n}.c) 0 (builtins.attrNames cfg)
