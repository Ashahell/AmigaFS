#!/bin/bash
# M39 -- libFuzzer over blockio, the one place the core touches storage.
#
# The plan's "hostile images" open risk asks for exactly this: a fuzz target over
# blockio with a memory backend. blockio is a security boundary and had unit
# tests but no target, so the only hostile inputs it ever saw were the ones a
# test author chose.
#
# The property is narrow and is this layer's whole job: an out-of-range block
# number must be REFUSED, never read or written out of bounds. The harness asks
# for 0, the last block, last+1, UINT32_MAX and values either side of the 32-bit
# wrap, and lets ASan judge the answer.
#
# The coverage floor matters more here than in most gates. A libFuzzer run that
# immediately dies, builds a broken binary, or finds a corpus-free input space
# with no reachable code can all exit 0 and mean nothing. So the gate asserts:
#   - the binary BUILT (a build failure must not read as a clean run)
#   - the run COMPLETED (libFuzzer exits non-zero on a finding)
#   - it actually EXECUTED at least RUNS inputs, and libFuzzer's own summary
#     agrees -- a floor asserted two independent ways
#   - it did not simply time out
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${FUZZ_CC:-clang}
RUNS=${FUZZ_RUNS:-20000}

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "M39 SKIP: $CC not on PATH"
    exit 0
fi

W=$(mktemp -d /tmp/amigafs-m39.XXXXXX)
trap 'rm -rf "$W"' EXIT
BIN=$W/fuzz_blockio
CORP=$W/corpus
mkdir -p "$CORP"

# Seed with real images so the first runs are not pure noise: blockio's reject
# paths are reachable immediately, but a formatter-shaped input reaches the
# in-range sweep too. Seeds come from the fixtures when they are present.
seeded=0
for fx in "$ROOT"/tests/fixtures/*.adf; do
    [ -f "$fx" ] || continue
    # a slice, not the whole image: blockio cares about geometry, not content,
    # and a 900 KiB seed would dominate the corpus
    head -c 65536 "$fx" > "$CORP/$(basename "$fx").seed" 2>/dev/null && seeded=1
    break
done
[ "$seeded" = 1 ] || head -c 4096 /dev/urandom > "$CORP/random.seed"

echo "M39: building with $CC -fsanitize=fuzzer,address,undefined"
if ! "$CC" -fsanitize=fuzzer,address,undefined -g -O1 \
        -I"$ROOT/include" \
        "$ROOT/tests/fuzz/fuzz_blockio.c" "$ROOT/src/lib/blockio.c" \
        -o "$BIN" 2>"$W/build.log"; then
    echo "M39 gate: FAIL -- the target did not build"
    head -12 "$W/build.log" | sed 's/^/    /'
    exit 1
fi

echo "M39: running $RUNS inputs"
# -artifact_prefix keeps libFuzzer's crash/minimum/timeout artifacts OUT of the
# repository. By default libFuzzer writes them to the current directory, and the
# first mutant run of this gate left a 400-byte crash-* file in the repo root,
# which `git add amigafs/` then committed. A gate that finds something must leave
# its evidence somewhere deliberate, not in the tree.
timeout 900 "$BIN" "$CORP" -runs="$RUNS" -max_len=65536 \
    -timeout=10 -rss_limit_mb=2048 -print_final_stats=1 \
    >"$W/out.log" 2>&1
rc=$?

if [ $rc -ne 0 ]; then
    echo "M39 gate: FAIL -- the run exited $rc (libFuzzer exits non-zero on a finding)"
    tail -30 "$W/out.log" | sed 's/^/    /'
    exit 1
fi

# Coverage floor, asserted two independent ways.
#
#   - `Done N runs` proves the run COMPLETED rather than dying early.
#   - `new_units_added` proves it EXPLORED. This is the one that matters: a
#     fuzzer that executes 20000 inputs and discovers no new coverage has
#     measured nothing, and `number_of_executed_units` would happily report
#     20000 and look like success. The floor is deliberately on the measure that
#     can distinguish the two. (The stats block only appears at all with
#     -print_final_stats=1 -- without it every field reads as absent, which is
#     how the first version of this gate reported "0 coverage units" on a run
#     that was in fact fine.)
done_runs=$(grep -oE "Done [0-9]+ runs" "$W/out.log" | tail -1 | grep -oE "[0-9]+")
units=$(grep -oE "stat::new_units_added: *[0-9]+" "$W/out.log" \
       | tail -1 | grep -oE "[0-9]+$")
[ -n "$done_runs" ] || done_runs=0
[ -n "$units" ] || units=0

echo "M39: libFuzzer reported 'Done $done_runs runs'"
echo "M39: $units new coverage units discovered"

if [ "$done_runs" -lt "$RUNS" ]; then
    echo "M39 gate: FAIL -- only $done_runs of $RUNS inputs ran; the run did not complete"
    tail -20 "$W/out.log" | sed 's/^/    /'
    exit 1
fi
if [ "$units" -le 0 ]; then
    echo "M39 gate: FAIL -- zero new coverage units; a run that explores nothing"
    echo "  cannot report green, however many inputs it claims to have executed"
    exit 1
fi

echo
echo "M39 gate: PASS ($done_runs inputs, $units new coverage units, no out-of-bounds access)"
exit 0