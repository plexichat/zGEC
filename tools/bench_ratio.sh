#!/usr/bin/env bash
# Compare zgec against zstd -4 / -6 on a single input file.
# Usage: tools/bench_ratio.sh <input-file> [zgec.exe]
set -u
IN="$1"
ZGEC="${2:-build/zgec.exe}"
ZSTD="${ZSTD:-/c/Users/dboyn/AppData/Local/Microsoft/WinGet/Links/zstd.exe}"
OUTDIR="$(mktemp -d)"
RAW=$(stat -c %s "$IN")

echo "input: $IN  raw=$RAW"

zc="$OUTDIR/z.zgec"
start=$(date +%s.%N)
"$ZGEC" c "$IN" "$zc" || { echo "zgec compress FAILED"; exit 1; }
end=$(date +%s.%N)
zsz=$(stat -c %s "$zc")
zt=$(echo "$end $start" | awk '{printf "%.2f", $1-$2}')

zd="$OUTDIR/z.dec"
start=$(date +%s.%N)
"$ZGEC" d "$zc" "$zd" || { echo "zgec decompress FAILED"; exit 1; }
end=$(date +%s.%N)
zdt=$(echo "$end $start" | awk '{printf "%.2f", $1-$2}')
if cmp -s "$IN" "$zd"; then rt=OK; else rt=BAD; fi

echo "zgec      : $zsz  ratio=$(echo "$RAW $zsz" | awk '{printf "%.4f", $1/$2}')  enc=${zt}s dec=${zdt}s roundtrip=$rt"

for lvl in 4 6; do
  o="$OUTDIR/z$lvl.zst"
  start=$(date +%s.%N)
  "$ZSTD" -q -$lvl -f "$IN" -o "$o" || { echo "zstd -$lvl FAILED"; continue; }
  end=$(date +%s.%N)
  s=$(stat -c %s "$o")
  t=$(echo "$end $start" | awk '{printf "%.2f", $1-$2}')
  echo "zstd -$lvl  : $s  ratio=$(echo "$RAW $s" | awk '{printf "%.4f", $1/$2}')  enc=${t}s"
done

rm -rf "$OUTDIR"
