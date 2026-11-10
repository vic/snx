let
  a = derivation { name = "a"; builder = "/bin/sh"; system = "x86_64-linux"; outputs = [ "out" "dev" ]; };
  b = derivation { name = "b"; builder = "/bin/sh"; system = "x86_64-linux"; dep = "${a.dev}/lib ${a}"; };
in { ctx = builtins.getContext "${b} ${a.drvPath}"; inherit (b) drvPath outPath; }
