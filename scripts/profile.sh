#!/usr/bin/env bash
# Profile the optimized book. Tries `perf` first (hardware counters are often unavailable under WSL2),
# then falls back to cachegrind, which simulates the cache hierarchy and always works.
#   bash scripts/profile.sh
export PATH=/usr/bin:/bin:/usr/local/bin
BIN="$HOME/me-build/release/bench_book"
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/results"
mkdir -p "$OUT"

if command -v perf >/dev/null && perf stat -e cycles true >/dev/null 2>&1; then
  echo "== perf stat"
  perf stat -e cycles,instructions,cache-misses,branch-misses,L1-dcache-load-misses "$BIN" --n 2000000 --cpu 2 2>&1 | tail -20
else
  echo "perf hardware counters unavailable here (expected under WSL2); using cachegrind instead"
fi

echo "== cachegrind (500k messages; simulated caches, ~50x slowdown)"
valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes --cachegrind-out-file="$OUT/cachegrind.out" \
  "$BIN" --n 500000 2>&1 | grep -E "I\s+refs|D\s+refs|D1\s+misses|LL\s+misses|Mispred|D1  miss rate|LL miss rate" | head -12
cg_annotate "$OUT/cachegrind.out" 2>/dev/null | head -40 > "$OUT/cachegrind_summary.txt"
echo "annotated summary: $OUT/cachegrind_summary.txt"
