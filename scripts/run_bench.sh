#!/usr/bin/env bash
# Run every benchmark and save output under results/.   bash scripts/run_bench.sh
# Run it on a quiet machine, and never together with the sanitizer builds.
export PATH=/usr/bin:/bin:/usr/local/bin
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$HOME/me-build/release"
mkdir -p "$HERE/results"
R="$HERE/results"

{
  echo "# environment"; uname -sr; lscpu | grep -E "Model name|^CPU\(s\)|Thread"; gcc --version | head -1
  echo; echo "# bench_book (single thread, cpu 2): default capacity max_orders=262144"
  "$BIN/bench_book" --n 4000000 --seed 1 --cpu 2
  for mo in 1048576 131072; do
    echo; echo "# capacity sensitivity: max_orders=$mo"
    "$BIN/bench_book" --n 4000000 --seed 1 --cpu 2 --max-orders "$mo" | grep -E "max_orders|optimized \| (all|new|cancel)|reference \| all|throughput|agree"
  done
} 2>&1 | tee "$R/bench_book.txt"

{
  echo "# bench_spsc (queue only, 32-byte messages)"
  "$BIN/bench_spsc" --pairs 2:4,2:3,8:9,2:10
  echo; echo "# bench_pipeline, paced + unthrottled (producer cpu 2, matching cpu 4, batch 16)"
  "$BIN/bench_pipeline" --n 2000000 --prod-cpu 2 --cons-cpu 4 --batch 16
  echo; echo "# unthrottled throughput, 5 repeats per batch size (shows run-to-run noise)"
  for b in 1 16 64; do
    echo "batch=$b:"
    for i in 1 2 3 4 5; do "$BIN/bench_pipeline" --n 4000000 --prod-cpu 2 --cons-cpu 4 --rates 0 --batch $b | tail -1 | awk -F'|' '{printf "  %s Mmsg/s\n", $3}'; done
  done
} 2>&1 | tee "$R/bench_pipeline.txt"
