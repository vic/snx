# positions after \n, \r\n and a lone \r (nx/ast.srs Nx.lineCol)
let s = {
  a = 1;  b = 2;
    c = 3; };
in [ (builtins.unsafeGetAttrPos "a" s).line (builtins.unsafeGetAttrPos "b" s).line (builtins.unsafeGetAttrPos "c" s).line
  (builtins.unsafeGetAttrPos "c" s).column __curPos.line __curPos.column ]
