# mapAttrs and zipAttrsWith thunks (Nx.CMapAt: env = the address of the value's slot): names and values by index,
# lazy values, a layered input
let
  big = builtins.listToAttrs (map (n: { name = "a${toString n}"; value = n; }) (builtins.genList (x: x) 20));
  s = big // { x = throw "never"; a3 = "new"; };
  m = builtins.mapAttrs (n: v: [ n (if n == "x" then null else v) ]) s;
in [ m.x m.a3 m.a19 m.a0 (builtins.attrNames (builtins.mapAttrs (n: v: throw "lazy") s))
  (builtins.zipAttrsWith (n: vs: [ n (builtins.length vs) ]) [ s { a1 = 0; zz = 1; } ]).a1
  (builtins.mapAttrs (n: v: v * 2) { b = 1; a = 2; }) ]
