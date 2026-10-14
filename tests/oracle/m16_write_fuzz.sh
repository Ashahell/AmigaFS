#!/usr/bin/env bash
# M16 gate: write-path differential fuzz under ASan + UBSan.
#
# WHY A SEPARATE GATE FROM fuzz_gate.sh. That one drives the READ path over
# hostile images: "no crash, no hang". This one drives the WRITE path and
# asserts a much stronger property -- after every operation, the volume must
# still VERIFY (checksums, pointer ranges, block types, double-use, bitmap
# agreement, dir-cache vs hash table), the tree must contain exactly the set of
# entries an independent model predicts, and every file must read back
# byte-identical. A write that leaks a block, eats a live one, strands half a
# dir-cache record or mis-splices a hash chain shows up here and in nowhere
# else: the m13/m14/m15 gates each drive a handful of FIXED scenarios, and a
# bug needing "an extension block AND a chain relink AND a cache record in one
# operation" is invisible to a fixed script.
#
# TWO MODES:
#   clean   -- random op sequences from a pristine volume; every invariant must
#              hold after every op. Coverage floors apply.
#   corrupt -- the same sequences from a deliberately damaged image. Only
#              "no crash, no hang" is asserted (writes onto a corrupt structure
#              may legitimately be refused with a reason OR succeed); coverage
#              floors are relaxed because damage can refuse everything.
#
# WHY THE SANITIZERS ARE BUILT HERE RATHER THAN VIA CMAKE. "No crash" from a
# normal build only means "did not happen to segfault this run": an
# out-of-bounds read inside the splice code is silent and invisible to every
# other gate.
#
# DETERMINISM: every run prints its seed, and the default multi-seed sweep
# re-runs the default seed, so a failure reported here is reproducible with
#   ./write_fuzz --seed=<N>
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/amigafs-wfuzz"
mkdir -p "$OUT"

echo "M16 gate: write-path differential fuzz"

if ! cc -std=gnu99 -g -O1 \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wconversion -Werror \
      -I"$ROOT/include" \
      "$ROOT/tests/write_fuzz.c" "$ROOT"/src/lib/*.c \
      -o "$OUT/write_fuzz" 2>&1; then
    echo "M16 gate: FAIL -- sanitizer build did not compile"
    exit 1
fi

export ASAN_OPTIONS="detect_leaks=1"
export UBSAN_OPTIONS="print_stacktrace=1"

rc=0

# ---- 1. clean mode, several seeds ------------------------------------------
# More seeds than one is the point: a single sequence that happens to avoid the
# interesting interleavings proves nothing. Each seed is a fresh volume.
for seed in 1 2 3 4 5 6; do
    if ! timeout 600 "$OUT/write_fuzz" --seed=$seed --iters=300; then
        rc=1
        echo "  M16 gate: FAIL (clean mode, seed=$seed)"
    fi
done

# ---- 2. corrupt mode: writes onto a damaged volume --------------------------
for seed in 11 12 13; do
    if ! timeout 600 "$OUT/write_fuzz" --seed=$seed --iters=200 --corrupt; then
        rc=1
        echo "  M16 gate: FAIL (corrupt mode, seed=$seed)"
    fi
done

# ---- 3. determinism: the default seed reproduces its own verdict ------------
# A fuzz harness whose result changes run to run cannot be used to investigate
# a failure. Same seed twice must give the same pass/fail and the same tallies.
a=$("$OUT/write_fuzz" --seed=1 --iters=300 2>&1 | grep -E "^(write fuzz|  [a-z]+ +tried)" | md5sum)
b=$("$OUT/write_fuzz" --seed=1 --iters=300 2>&1 | grep -E "^(write fuzz|  [a-z]+ +tried)" | md5sum)
if [ "$a" != "$b" ]; then
    echo "  determinism check                   FAIL"
    rc=1
else
    echo "  determinism (seed 1 twice)         reproducible"
fi

echo
if [ $rc -eq 0 ]; then
    echo "M16 gate: PASS"
    exit 0
fi
echo "M16 gate: FAIL"
exit 1