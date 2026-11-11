let min = -9223372036854775807 - 1; max = 9223372036854775807; in
[ (min + max) (max + min) (min + 0) (min + 1) (max + -1) (min - 0) (min - min) (max - max) (max - 1) (-1 - max) (min - -1)
  (-1 * max) (max * -1) (1 * min) (min * 1) (3037000499 * 3037000499) (-3037000499 * 3037000499) (min * 0) (0 * min)
  (4611686018427387904 * -2) (-2 * 4611686018427387904) (max / -1) (min / 1)
  (builtins.bitAnd max (-1)) (builtins.bitOr min 1) (builtins.bitXor min (-1)) (builtins.bitXor (-6) 3) (builtins.bitAnd (-6) 3) ]
