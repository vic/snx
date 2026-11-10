let b = builtins; in [
  (b.bitAnd 12 10) (b.bitOr 12 10) (b.bitXor 12 10)
  (b.floor 2.7) (b.ceil 2.1) (b.floor (-2.5)) (b.ceil 3)
  (b.replaceStrings [ "oo" "a" "" ] [ "a" "i" "-" ] "foobar")
  (b.compareVersions "1.0pre1" "1.0") (b.compareVersions "2.3a" "2.3.1") (b.compareVersions "1.10" "1.9")
  (b.splitVersion "1.2.3pre4-x")
  (b.parseDrvName "hello-2.10-bin") (b.parseDrvName "foo")
  (b.groupBy (x: b.substring 0 1 x) [ "foo" "bar" "baz" ])
  (b.partition (x: x > 2) [ 1 3 2 4 ])
  (b.zipAttrsWith (n: vs: vs) [ { a = 1; b = 2; } { a = 3; } ])
  (b.baseNameOf "/a/b/c.nix") (b.baseNameOf "a//") (b.dirOf "/a/b/c") (b.dirOf "x") (b.dirOf "/x")
  (b.genericClosure { startSet = [ { key = 1; } ]; operator = e: if e.key < 4 then [ { key = e.key + 1; } { key = 1; } ] else [ ]; })
]
