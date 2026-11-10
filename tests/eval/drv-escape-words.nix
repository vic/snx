# ATerm escaping eight bytes at a time (rt/snx_rt.c Nx__sbAddEsc): specials at word edges, runs, short tails
let d = derivation {
  name = "esc"; builder = "/bin/sh"; system = "x86_64-linux";
  a = "\"bcdefgh"; b = "abcdefg\\"; c = "abcdefgh\n"; d = "abcdefghijklmno\tp"; e = "abcdefghijklmnopqrstuvwxyz0123456789";
  f = "x\r"; g = ""; h = "12345678"; i = "1234567\r\n89abcdef\"\"\\\\ghijklmnop"; j = "x\r\r~\t"; k = "été — ok\n";
}; in d.drvPath
