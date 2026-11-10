#!/usr/bin/env bash
# Where does the pipeline's per-message cost go, and does batch popping help?  Unthrottled runs.
export PATH=/usr/bin:/bin:/usr/local/bin
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bash "$HERE/scripts/build.sh" 2>&1 | grep -E "error|warning"
B="$HOME/me-build/release"
"$B/me_tests" spsc | tail -3
for batch in 1 4 16 64; do
  echo "--- engine, batch=$batch (prod cpu 2, cons cpu 4)"
  "$B/bench_pipeline" --n 4000000 --prod-cpu 2 --cons-cpu 4 --rates 0 --batch $batch | tail -1
done
echo "--- no engine, batch=16"
"$B/bench_pipeline" --n 4000000 --prod-cpu 2 --cons-cpu 4 --rates 0 --no-engine --batch 16 | tail -1
