let b = builtins; d = ./fixtures; in [
  (b.readFile ./fixtures/w1-file.txt) (b.pathExists ./fixtures/w1-file.txt) (b.pathExists ./fixtures/nope)
  (b.pathExists (toString ./fixtures + "/")) (b.pathExists (toString ./fixtures/w1-file.txt + "/"))
  (b.readDir d) (b.readFileType d) (b.readFileType ./fixtures/w1-file.txt)
  (b.hashFile "sha256" ./fixtures/w1-file.txt)
  (b.scopedImport { x = 41; } ./fixtures/w1-scoped.nix)
  (b.unsafeGetAttrPos "foo" { foo = 1; }).line (b.unsafeGetAttrPos "nope" { foo = 1; })
  (b.findFile [ { path = toString d; prefix = ""; } ] "w1-file.txt" == ./fixtures/w1-file.txt)
]
