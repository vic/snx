# git and tarball flake references
map (s: let r = builtins.parseFlakeRef s; in { inherit r; back = builtins.flakeRefToString r; }) [
  "git+https://github.com/a/b?ref=main&shallow=1&dir=x&foo=1"
  "git+ssh://git@x.org:22/~a/b.git?ref=refs/heads/x"
  "git+file:/x"
  "git://somewhere/repo?ref=branch"
  "https://u:p@x.org:080/a.tar.gz?b=c+d"
  "file+https://example.com/a?narHash=sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
  "hg+https://x.org/a?ref=b"
]
