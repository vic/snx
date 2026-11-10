# attrNames in byte order (rt/snx_rt.c Nx__byName: the first eight bytes as a key, then the rest): shared prefixes,
# a name that is a prefix of another, bytes above 0x7f, the empty name, more than 16 names
let
  s = { "python3Packages" = 1; "python3PackagesB" = 2; "python3Package" = 3; "python3Pack" = 4; "" = 5; "a" = 6; "A" = 7;
    "é" = 8; "z" = 9; "zz" = 10; "abcdefgh" = 11; "abcdefgg" = 12; "abcdefgh\n" = 13; "abcdefghi" = 14; "_" = 15;
    "0" = 16; "~" = 17; "ab" = 18; };
in [ (builtins.attrNames s) (builtins.attrValues s) (builtins.toJSON s) ]
