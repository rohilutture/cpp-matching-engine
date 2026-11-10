#!/usr/bin/env bash
# Run the whole test suite under AddressSanitizer+UB and ThreadSanitizer (separate builds).
export PATH=/usr/bin:/bin:/usr/local/bin
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rc=0
for san in "address,undefined" thread; do
  name="${san//,/_}"
  DIR="$HOME/me-build/san-$name"
  cmake -S "$HERE" -B "$DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSANITIZE="$san" >/dev/null
  cmake --build "$DIR" --target me_tests 2>&1 | grep -E "error|FAILED" | head
  echo "=== $san"
  # TSan aborts with "unexpected memory mapping" under high ASLR entropy (WSL2 kernels); disable ASLR for it
  timeout 900 setarch "$(uname -m)" -R "$DIR/me_tests" 2>&1 | tail -25
  [ "${PIPESTATUS[0]}" -ne 0 ] && rc=1
done
exit $rc
