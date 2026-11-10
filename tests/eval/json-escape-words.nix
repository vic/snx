# toJSON strings eight bytes at a time (rt/snx_fs.c Nx__jsonStr): specials and UTF-8 at word edges, short tails
builtins.toJSON [ "\"bcdefgh" "abcdefg\\" "abcdefgh\n" "abcdefghijklmno\tp" "abcdefghijklmnopqrstuvwxyz0123456789" "x\r" "" "12345678"
  "1234567é89abcdef" "abcdefgh—ijklmnop" "1234567\r\n89abcdef\"\"\\\\ghijklmnop" "~~~~~~~~ ~~~~~~~ !" ]
