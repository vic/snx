# a name cell first made inside a call that returns a scalar (its words are released) is shared later (Nx.nameCell)
let
  empty = s: builtins.attrNames s == [ ];
  a = { zzNameCellA = 1; zzNameCellB = 2; };
  junk = n: builtins.length (builtins.genList (i: { a = i; b = [ i i ]; }) n);
in builtins.seq a [ (empty a) (junk 50) (builtins.attrNames { zzNameCellA = 3; zzNameCellB = 4; }) ]
