let
  s = { a = 1;
        bb = 2;
          ccc = 3; };
  p = builtins.unsafeGetAttrPos "bb" s;
  q = builtins.unsafeGetAttrPos "ccc" s;
in [
  p.line p.column
  q.line q.column
  (builtins.isAttrs (builtins.unsafeGetAttrPos "a" s))
  (builtins.attrNames p)
  (builtins.unsafeGetAttrPos "zz" s)
]
