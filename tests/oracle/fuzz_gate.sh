#!/usr/bin/env bash
# Fuzz / robustness gate: the core must survive hostile images.
#
# Built here rather than by CMake because the POINT is the sanitizers: the same
# core sources compiled with -fsanitize=address,undefined. Without them "no
# crash" only means "did not happen to segfault on this run" -- an out-of-bounds
# read inside the parser is silent and invisible to every other gate.
#
# The harness itself asserts it REACHED the parser (entry and file counters
# have minimums), so a green run cannot mean "every image was refused at the
# DOS id check and nothing was tested".
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/amigafs-fuzz"
mkdir -p "$OUT"

echo "Fuzz gate: hostile images under ASan + UBSan"

if ! cc -std=gnu99 -g -O1 \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wconversion -Werror \
      -I"$ROOT/include" -I"$ROOT/src/lib" \
      "$ROOT/tests/fuzz_test.c" "$ROOT"/src/lib/*.c \
      -o "$OUT/fuzz_test" 2>&1; then
    echo "Fuzz gate: FAIL -- sanitizer build did not compile"
    exit 1
fi

export ASAN_OPTIONS="detect_leaks=1"
export UBSAN_OPTIONS="print_stacktrace=1"

# A non-terminating walk must FAIL the gate rather than wedge the build.
rc=0
( cd "$ROOT" && timeout 600 "$OUT/fuzz_test" tests/fixtures "${1:-200}" ) || rc=$?

if [ $rc -eq 124 ]; then
    echo "Fuzz gate: FAIL -- the harness hit the 600s ceiling (a walk did not terminate)"
    exit 1
fi
if [ $rc -ne 0 ]; then
    echo "Fuzz gate: FAIL (rc=$rc)"
    exit 1
fi
echo "Fuzz gate: PASS"
exit 0
