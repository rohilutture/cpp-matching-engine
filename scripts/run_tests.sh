#!/usr/bin/env bash
# Run each test group separately with a timeout (so a hang is attributable). Usage: run_tests.sh [build-dir-name]
export PATH=/usr/bin:/bin:/usr/local/bin
DIR="$HOME/me-build/${1:-release}"
cd "$DIR" || exit 1
rc_all=0
for t in bitmap id_map spsc book_ diff_wide diff_narrow diff_tiny diff_few diff_many; do
  echo "== $t"
  timeout 120 stdbuf -oL ./me_tests "$t" 2>&1 | tail -6
  rc=${PIPESTATUS[0]}
  echo "rc=$rc"
  [ "$rc" -ne 0 ] && rc_all=1
done
exit $rc_all
