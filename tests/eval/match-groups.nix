[
  (builtins.match "a(b)(c)?" "ab")
  (builtins.match "ab" "abc")
  (builtins.match "abc" "abc")
  (builtins.match "[[:space:]]+([[:upper:]]+)[[:space:]]+" "  FOO   ")
]
