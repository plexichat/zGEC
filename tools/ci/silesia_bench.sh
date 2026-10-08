#!/usr/bin/env bash
# Silesia sweep driver (own implementation, no dependency on tools/bench).
#
#   silesia_bench.sh OUT_CSV ZGEC_BIN REPS MODE FILE...
#
# MODE is 1t (single-threaded) or nt (multi-threaded: one worker per core).
# Measures, per input file:
#   zgec  levels 1..9            (--tier left at the level preset)
#   zstd  levels 3, 4, 5, 6
#   xz    level 6 (default)
# each as encode + decode, verifying the round trip with cmp.
# Times are best-of-REPS wall seconds from date +%s%N (GNU coreutils).
#
# CSV columns:
#   corpus,file,tool,config,threads,op,input_bytes,output_bytes,ratio,seconds,mbps,notes
set -euo pipefail

OUT="$1"
ZGEC="$2"
REPS="$3"
MODE="$4"
shift 4

# 1t leaves zstd's thread flag off entirely rather than passing -T1: with
# -T1 zstd builds the multi-worker job queue and runs it with one worker,
# which is the same output at about half the speed, so -T1 would report a
# single-thread number no single-threaded caller can reproduce. A bare
# zstd invocation is already single-threaded.
if [ "$MODE" = "1t" ]; then
  ZT=1; ST=""; XT=""; THR=1
else
  ZT=0; ST=0; XT="-T0"; THR=n
fi
XZ_REPS=1  # xz encode is slow; single sample, decode still uses REPS

if [ ! -s "$OUT" ]; then
  echo "corpus,file,tool,config,threads,op,input_bytes,output_bytes,ratio,seconds,mbps,notes" > "$OUT"
fi

# best SECONDS seconds of running CMD... (discards stdout, keeps stderr on failure)
best_of() {
  local reps=$1
  shift
  local best="" i t0 t1 s
  for ((i = 0; i < reps; i++)); do
    t0=$(date +%s%N)
    "$@" > /dev/null
    t1=$(date +%s%N)
    s=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.6f", (b - a) / 1e9 }')
    if [ -z "$best" ] || [ "$(awk -v x="$s" -v y="$best" 'BEGIN { print (x < y) }')" = "1" ]; then
      best=$s
    fi
  done
  printf '%s' "$best"
}

row() { # file tool config op in out secs notes
  local file=$1 tool=$2 config=$3 op=$4 in=$5 out=$6 secs=$7 notes=$8
  awk -v c="silesia" -v f="$file" -v t="$tool" -v g="$config" -v th="$THR" \
      -v o="$op" -v i="$in" -v u="$out" -v s="$secs" -v n="$notes" \
      'BEGIN {
        r = (u > 0) ? i / u : 0;
        m = (s > 0) ? (i / 1048576) / s : 0;
        printf "%s,%s,%s,%s,%s,%s,%d,%d,%.4f,%.3f,%.1f,%s\n", c, f, t, g, th, o, i, u, r, s, m, n
      }' >> "$OUT"
}

sweep_zgec() { # file level
  local file=$1 lvl=$2 base enc dec
  base=$(basename "$file")
  enc=$(mktemp); dec=$(mktemp)
  trap 'rm -f "$enc" "$dec"' RETURN
  local esec dsec enote="roundtrip-ok" dnote="roundtrip-ok" isize csize
  isize=$(stat -c%s "$file")
  esec=$(best_of "$REPS" "$ZGEC" c -l "$lvl" -T "$ZT" --quiet "$file" "$enc")
  "$ZGEC" c -l "$lvl" -T "$ZT" --quiet "$file" "$enc"
  csize=$(stat -c%s "$enc")
  dsec=$(best_of "$REPS" "$ZGEC" d --quiet "$enc" "$dec")
  "$ZGEC" d --quiet "$enc" "$dec" || dnote="decode-error"
  cmp -s "$file" "$dec" || dnote="MISMATCH"
  row "$base" zgec "-l$lvl" encode "$isize" "$csize" "$esec" "$enote"
  row "$base" zgec "-l$lvl" decode "$isize" "$csize" "$dsec" "$dnote"
}

sweep_zstd() { # file level
  local file=$1 lvl=$2 base enc dec
  base=$(basename "$file")
  enc=$(mktemp); dec=$(mktemp)
  trap 'rm -f "$enc" "$dec"' RETURN
  local esec dsec dnote="roundtrip-ok" isize csize
  isize=$(stat -c%s "$file")
  # `best_of` fails the job when the command fails, so these calls have to
  # actually succeed. `-f` is required because mktemp created both paths:
  # without it zstd refuses to overwrite an existing output file ("already
  # exists; not overwritten") and exits 1, which failed every bench job.
  # `-c` is left out because it asks for stdout and `-o` names a file, and
  # zstd honours `-o`; asking for both is ambiguous.
  if [ -n "$ST" ]; then
    esec=$(best_of "$REPS" zstd -f "-$lvl" -T"$ST" "$file" -o "$enc")
    zstd -f "-$lvl" -T"$ST" -q "$file" -o "$enc"
  else
    esec=$(best_of "$REPS" zstd -f "-$lvl" "$file" -o "$enc")
    zstd -f "-$lvl" -q "$file" -o "$enc"
  fi
  csize=$(stat -c%s "$enc")
  dsec=$(best_of "$REPS" zstd -df "$enc" -o "$dec")
  zstd -df "$enc" -o "$dec" -q
  cmp -s "$file" "$dec" || dnote="MISMATCH"
  row "$base" zstd "-$lvl" encode "$isize" "$csize" "$esec" "roundtrip-ok"
  row "$base" zstd "-$lvl" decode "$isize" "$csize" "$dsec" "$dnote"
}

sweep_xz() { # file
  local file=$1 base enc dec
  base=$(basename "$file")
  enc=$(mktemp); dec=$(mktemp)
  trap 'rm -f "$enc" "$dec"' RETURN
  local esec dsec dnote="roundtrip-ok" isize csize
  isize=$(stat -c%s "$file")
  # shellcheck disable=SC2086
  esec=$(best_of "$XZ_REPS" xz -c -6 $XT "$file")
  # shellcheck disable=SC2086
  xz -c -6 $XT "$file" > "$enc"
  csize=$(stat -c%s "$enc")
  dsec=$(best_of "$REPS" xz -dc "$enc")
  xz -dc "$enc" > "$dec"
  cmp -s "$file" "$dec" || dnote="MISMATCH"
  row "$base" xz "-6" encode "$isize" "$csize" "$esec" "roundtrip-ok"
  row "$base" xz "-6" decode "$isize" "$csize" "$dsec" "$dnote"
}

for f in "$@"; do
  for l in 1 2 3 4 5 6 7 8 9; do sweep_zgec "$f" "$l"; done
  for l in 3 4 5 6; do sweep_zstd "$f" "$l"; done
  sweep_xz "$f"
done

echo "rows: $(( $(wc -l < "$OUT") - 1 )) -> $OUT"
