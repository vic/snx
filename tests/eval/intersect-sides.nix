# intersectAttrs walks the smaller side and looks names up in the other; a layered e2 is looked up leaf by leaf
let
  big = builtins.listToAttrs (map (n: { name = "a${toString n}"; value = n; }) (builtins.genList (x: x) 40));
  layered = big // { a3 = "new"; b = 1; } // { a5 = "newer"; };
in [
  (builtins.intersectAttrs { a1 = 0; a3 = 0; zz = 0; } big)
  (builtins.intersectAttrs big { a2 = "x"; q = 1; })
  (builtins.intersectAttrs { a3 = 0; a5 = 0; b = 0; c = 0; } layered)
  (builtins.intersectAttrs layered { a5 = 1; b = 2; c = 3; })
  (builtins.intersectAttrs { } layered)
  (builtins.intersectAttrs { nope = 1; } layered)
  (builtins.attrNames (builtins.intersectAttrs layered big))
]
