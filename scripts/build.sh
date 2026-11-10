#!/usr/bin/env bash
# Build in WSL. Build dirs live on the Linux filesystem (fast).
#   bash scripts/build.sh              -> ~/me-build/release   (tests + benchmarks)
#   bash scripts/build.sh address      -> ~/me-build/address   (tests under ASan; also: undefined, thread)
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/local/bin
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SAN="${1:-}"
DIR="$HOME/me-build/${SAN:-release}"
cmake -S "$HERE" -B "$DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSANITIZE="$SAN" >/dev/null
cmake --build "$DIR" 2>&1 | grep -E "error|warning|Error|FAILED|Linking" | head -60 || true
ls "$DIR"/me_tests "$DIR"/bench_book "$DIR"/bench_pipeline "$DIR"/bench_spsc
