[ (builtins.tryEval (throw "x")) (builtins.tryEval (assert false; 1)) (builtins.tryEval 5) ]
