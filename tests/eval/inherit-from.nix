let s = { a = 1; b = 2; }; in { inherit (s) a b; c = 3; }
