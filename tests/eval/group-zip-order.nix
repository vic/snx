# groupBy and zipAttrsWith group by a stable sort of (name, value): each group keeps the input order
let
  big = builtins.listToAttrs (map (n: { name = "a${toString n}"; value = n; }) (builtins.genList (x: x) 12));
in [
  (builtins.groupBy (x: if x < 5 then "lo" else if x > 9 then "hi" else "mid") [ 7 1 12 3 9 10 5 0 ])
  (builtins.groupBy (x: x) [ ])
  (builtins.zipAttrsWith (n: vs: [ n vs ]) [ { b = 1; a = 2; } { } (big // { a0 = "x"; }) { b = 3; c = 4; } ])
  (builtins.zipAttrsWith (n: vs: vs) [ ])
]
