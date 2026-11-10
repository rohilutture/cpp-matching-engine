#!/usr/bin/env bash
# Sensitivity of the optimized book to pool/id-table capacity (cache footprint).  bash scripts/sweep_capacity.sh
export PATH=/usr/bin:/bin:/usr/local/bin
BIN="$HOME/me-build/release/bench_book"
for mo in 1048576 262144 131072; do
  echo "################ max_orders=$mo"
  "$BIN" --n 4000000 --seed 1 --cpu 2 --max-orders "$mo" | grep -E "max_orders|optimized \| all|reference \| all|optimized \| new|optimized \| cancel|throughput|agree|events"
done
