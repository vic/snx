#!/usr/bin/env bash
# census run: prints the SNX_CENSUS region lines, maxRSS and the gap. Usage: run.sh [census-binary]
# without $1 it builds one (SRS = srs census ws, B = build dir).
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
X=${1:-}
if [ -z "$X" ]; then X=$(SRS=${SRS:?set SRS} B=${B:-/tmp/snx-census} SNX_CFLAGS=-DSNX_CENSUS timeout 300 "$here/tests/build.sh" | tail -1); fi
cd "${XDG_RUNTIME_DIR:-/tmp}"; ulimit -s unlimited
out=$(SNX_STACK_SET=1 timeout 60 /run/current-system/sw/bin/time -v "$X" eval /home/vic/hk/iets/bench/workloads/nixos-server.nix -I nixpkgs=/nix/store/zqznszbq59yjk0cm3ray4pfyglaps6qb-source 2>&1 || true)
echo "$out" | grep -E '^(census|"?/nix/store)' || true
rss=$(echo "$out" | awk '/Maximum resident/ {printf "%.1f", $NF/1024}')
sum=$(echo "$out" | awk '/^census sample sum=/ {sub("census sample sum=","");v=$0} END{print v}')  # last sample
echo "maxRSS=$rss"
awk -v s="$sum" -v r="$rss" 'BEGIN{printf "gap=%.1f MiB (%.2f%%)\n", s-r, (s-r)/r*100}'
