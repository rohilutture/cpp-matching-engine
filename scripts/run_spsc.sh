#!/usr/bin/env bash
# Queue-only throughput across core pairs.  bash scripts/run_spsc.sh [pairs]
export PATH=/usr/bin:/bin:/usr/local/bin
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bash "$HERE/scripts/build.sh" 2>&1 | grep -E "error|warning"
mkdir -p "$HERE/results"
"$HOME/me-build/release/bench_spsc" --pairs "${1:-2:4,2:3,0:1,8:9,8:10,2:10,16:17}" | tee "$HERE/results/bench_spsc.txt"
