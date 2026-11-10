let d = derivation { name = "simple"; builder = "/bin/sh"; system = "x86_64-linux"; args = [ "-c" "echo hi > $out" ]; }; in { inherit (d) drvPath outPath type outputName; }
