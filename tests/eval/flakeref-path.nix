# path flake references, and the attribute sets that make them
[
  (map builtins.parseFlakeRef [ "/foo//bar/./baz/" "/a b?rev=x&revCount=+3&dir=d" "path:a//b/" "path:" "path:/a%20b" ])
  (map builtins.flakeRefToString [
    { type = "path"; path = "/a b"; rev = "x"; revCount = 3; narHash = "y"; }
    { type = "path"; path = ""; }
    { type = "github"; owner = "a"; repo = "b"; rev = "ABCDEF0123456789012345678901234567890123"; dir = "d e"; }
    { type = "git"; url = "git@github.com:a/b.git"; ref = "main"; submodules = true; }
  ])
]
