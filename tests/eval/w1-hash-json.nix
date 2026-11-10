let b = builtins; h = b.hashString "sha256" "abc"; in [
  (b.hashString "md5" "abc") (b.hashString "sha1" "abc") h (b.hashString "sha512" "")
  (b.convertHash { hash = h; toHashFormat = "nix32"; hashAlgo = "sha256"; })
  (b.convertHash { hash = h; toHashFormat = "sri"; hashAlgo = "sha256"; })
  (b.convertHash { hash = "sha256-ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0="; toHashFormat = "base16"; })
  (b.hashString "blake3" "abc")
  (b.fromJSON ''{"a": [1, 2.5, -3, true, null, "xé\n"], "b": {"c": {}}, "d": []}'')
  (b.toJSON { z = 1; a = [ "q\"" 1.5 null ]; m = { __toString = s: "str"; }; })
  (b.toXML { a = [ 1 "s" true null 2.5 ]; f = x: x; g = { x, y ? 1, ... }: x; })
]
