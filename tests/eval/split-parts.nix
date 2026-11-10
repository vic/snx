[
  (builtins.split "(a)|(c)" "abc")
  (builtins.split "x" "abc")
  (builtins.split "[[:space:]]+" "a  b c")
]
