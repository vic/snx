# `a ? x.y` forces the sets on the path, never the value at its end (ExprOpHasAttr::eval eval.cc:1490-1502)
let s = { b = s.c; c = s ? b && s ? d.e; d.e = s.c; }; in [ s.c s.b s.d.e ]
