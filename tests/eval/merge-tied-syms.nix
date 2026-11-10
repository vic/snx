# attrs merged from two literals, both names only ever quoted: they tie in creation order, which deepSeq visits.
# "s t" (a throw, caught) comes first; "q r" (a type error, not caught) must not be reached.
builtins.tryEval (builtins.deepSeq { a.c = 1; a = { "s t" = throw "two"; "q r" = 1 + {}; }; } 1)
