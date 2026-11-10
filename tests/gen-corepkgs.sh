#!/usr/bin/env bash
# Rewrites the embedded corepkgs strings of nx/b_core.srs and nx/b_flake.srs from their readable sources in
# nx/corepkgs/: the line after `def Nx.NAME(): Str =` becomes the file as one string literal (\ " and newline escaped).
# OUT = where the rewritten .srs files go (default nx/, in place); tests/corepkgs.sh diffs a copy against nx/.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$here/nx}
gen() { # srs file, def name, corepkgs file
  local src=$out/$1
  [[ -e $src ]] || cp "$here/nx/$1" "$src"
  LIT=$(sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' "$here/nx/corepkgs/$3" | awk 'BEGIN { ORS = "" } { print $0 "\\n" }') \
    awk -v def="def Nx.$2(): Str =" '{ print } $0 == def { getline; print "  \"" ENVIRON["LIT"] "\"" }' "$src" > "$src.tmp"
  mv "$src.tmp" "$src"
}
gen b_core.srs coreFetchurl fetchurl.nix
gen b_core.srs coreDerivation derivation.nix
gen b_flake.srs coreCallFlake call-flake.nix
gen b_flake.srs coreFlake flake.nix
