# builtins.parseFlakeRef and flakeRefToString on the github-like schemes: parsed, then written again
map (s: let r = builtins.parseFlakeRef s; in { inherit r; back = builtins.flakeRefToString r; }) [
  "github:NixOS/nixpkgs/23.05?dir=lib"
  "github:a/b/release/v1.0"
  "github:a//b?ref=x%2Fy&host=h.org"
  "gitlab:a/b/0123456789012345678901234567890123456789"
  "sourcehut:~a/b/main"
  "nixpkgs/nixos-24.05"
  "flake:nixpkgs/foo?ref=ignored&dir=sub"
]
