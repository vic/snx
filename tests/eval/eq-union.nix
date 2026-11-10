# == on layered sets: a non-set is unequal without merging; derivations compare by outPath, found leaf by leaf
let
  big = builtins.listToAttrs (map (n: { name = "a${toString n}"; value = n; }) (builtins.genList (x: x) 40));
  l1 = big // { a3 = "new"; };
  l2 = big // { a3 = "new"; };
  drvA = big // { type = "derivation"; outPath = "/p"; x = 1; };
  drvB = big // { type = "derivation"; outPath = "/p"; x = 2; };
in [ (l1 == null) (null == l1) (l1 == 1) (l1 == l2) (l1 == big) (big == l1) (drvA == drvB) (drvA == (drvB // { outPath = "/q"; })) ]
