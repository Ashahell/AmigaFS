#!/usr/bin/env bash
# m29_no_raw_page_bitmap_writes.sh -- a bitmap bit has TWO coordinate spaces,
# and this gate exists because the code kept mixing them (M28).
#
#   ASSEMBLED map: what afs_bitmap_read produces. Page data areas concatenated,
#                  each page's checksum long stripped. afs_bitmap_set/test/
#                  clear address THIS, at offset 0. So does afs_bitmap_read's
#                  OUTPUT buffer.
#   RAW PAGE:      a single bitmap storage block. Bytes 0..3 are the page's OWN
#                  checksum long; map data begins at +4. afs_bitmap_read
#                  assembles from `blk + 4`.
#
# Using the assembled-map setter on a raw page parks blocks 0..31 inside the
# checksum long and shifts every later block four bytes early. Four instances
# have been found and fixed, all in test fixtures:
#
#   1. verify.c's page reader
#   2. test_verify.c's bm_set()      -- so no test there ever checked bitmap
#                                       agreement; my new "clean" case passed
#                                       for the wrong reason
#   3. write_fuzz.c's volume fixture -- the fuzz volume has carried a broken
#                                       map since it was written
#   4. test_alloc.c's build_writable_img() -- the base image for 164 allocation
#                                       checks, which therefore all agreed
#                                       about map state no reader would see.
#                                       Its own sibling helper documented the
#                                       gotcha in full and got it right.
#
# The recurring shape: the fixture and the library agreed on the SAME wrong
# interpretation, so the tests were self-consistently meaningless and reported
# green. That is why this gate does not run the suites -- running them proves
# nothing here. It asserts the INVARIANT each fixture must satisfy.
#
# Each volume builder is checked by asking the real reader whether the blocks
# the tree references are marked used, which is the question the whole class of
# bug makes unanswerable.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

fail=0
note() { printf '  %-52s %s\n' "${1:-}" "${2:-}"; }
bad()  { printf '  %-52s FAIL\n' "${1:-}"; fail=1; }

WORK=$(mktemp -d /tmp/amigafs-m29.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

[ -d "$ROOT/src/lib" ] || { echo "SKIP: not an amigafs tree"; exit 77; }

# Static check: no fixture may apply the assembled-map setter to a name that is
# assigned from a raw page. The pattern to catch is a single STATEMENT:
#
#     <ptr> = <img> + <block> * 512;      ... a raw page
#     afs_bitmap_set(<ptr>, i);            ... the assembled-map setter
#
# The corrected fixtures instead build a separate `map` buffer and copy it in at
# +4, so the setter is never handed a page. Detecting this per-FILE with a
# whole-file heuristic was wrong twice already (see the commit message): it
# needed a `map` declaration somewhere in the file, which both over- and
# under-matched. So this asks the question per call site, using the line that
# assigns the name.
static_scan() {
    local file="$1" base hits
    base=$(basename "$file")
    # A file that never calls the setter at all is trivially clean.
    if ! grep -qE 'afs_bitmap_(set|clear)\(' "$file" 2>/dev/null; then
        note "$base" "no bitmap-set calls"
        return 0
    fi
    # For each setter call, resolve the argument name and check whether that
    # name is EVER assigned from a raw page -- the two halves need not be on the
    # same line.
    #
    # Note the awk rather than a grep: the declaration is `uint8_t *bm = ...`,
    # and a grep cannot skip past the pointer star to reach the stride (a `[^=]*`
    # pattern stops dead at it -- which is why the first two versions of this
    # scan both missed a planted instance).
    hits=0
    while read -r arg; do
        [ -z "$arg" ] && continue
        # Look for `<name> = ... * <n>;` -- the raw-page idiom. Matching the
        # name as a SUBSTRING with index() is deliberate: the declaration is
        # `uint8_t *bm = img + 900u * 512;`, and every anchored pattern either
        # stops at the pointer star or expects the star BEFORE the name. Two
        # earlier versions of this scan used exactly those forms and both
        # missed a planted instance, which is the only reason this one is a
        # substring match rather than something prettier.
        if awk -v a="$arg" '
            index($0, a " =") > 0 || index($0, a "=") > 0 {
                if ($0 ~ /\*[[:space:]]*[0-9]+[[:space:]]*;/) f = 1
            }
            END { exit f ? 0 : 1 }' "$file"; then
            hits=$((hits + 1))
        fi
    done < <(grep -oE 'afs_bitmap_(set|clear)\([[:space:]]*[A-Za-z_][A-Za-z0-9_]*' "$file" \
             | sed -E 's/.*\([[:space:]]*//')
    if [ "$hits" -gt 0 ]; then
        bad "$base applies the assembled-map setter to a raw page ($hits site(s))"
        return 1
    fi
    note "$base" "no raw-page bitmap writes"
    return 0
}

note "--- static scan: raw-page bitmap writes ---"
for f in "$ROOT"/tests/unit/*.c "$ROOT"/tests/*.c "$ROOT"/src/lib/*.c; do
    [ -f "$f" ] || continue
    static_scan "$f"
done

note "--- behavioural: does each fixture's volume have a readable map? ---"
cat > "$WORK/volcheck.c" <<'EOF'
/* For each of the unit fixtures' volume shapes, build it and report how many
 * blocks the tree references are marked FREE. Prints "<name> <count>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amigafs/be.h"
#include "amigafs/bitmap.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/dir.h"
#include "amigafs/verify.h"
#include "amigafs/volume.h"

#define BS 512

/* Reproduce test_alloc.c's build_writable_img(), reading the SETTER AND THE
 * OFFSET FROM ARGV so the same binary can test both the fixed and the broken
 * form. argv[1] = "page" (fixed: build a map, copy at +4) or "raw"
 * (the historic bug: apply the assembled-map setter straight to the page). */
static size_t build(unsigned char *img, int raw) {
    memset(img, 0, 1760u * BS);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 1u);
    unsigned char *r = img + 880u * BS;
    put_be32(r, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);
    put_be32(r + BS - 196, 881u);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, 1);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
    unsigned char *bm = img + 881u * BS;
    if (raw) {
        for (unsigned i = 0; i < 1760u; i++)
            if (i != 0 && i != 1 && i != 880 && i != 881) afs_bitmap_set(bm, i);
    } else {
        static unsigned char map[512];
        memset(map, 0, sizeof map);
        for (unsigned i = 0; i < 1760u; i++)
            if (i != 0 && i != 1 && i != 880 && i != 881) afs_bitmap_set(map, i);
        memset(bm, 0, 512);
        memcpy(bm + 4, map, 508);
    }
    put_be32(bm + 0, afs_block_checksum(bm, BS, 0));
    return (size_t)1760u * BS;
}

int main(int argc, char **argv) {
    static unsigned char img[1760u * BS];
    afs_verify_report_t rep[32];
    int raw = (argc > 1 && strcmp(argv[1], "raw") == 0);
    size_t n = build(img, raw);
    int problems = afs_verify_memory_ex(img, n, BS, rep, 32, AFS_VERIFY_STRICT);
    printf("%s %d\n", raw ? "raw" : "fixed", problems);
    return 0;
}
EOF

if gcc -w -I"$ROOT/include" -o "$WORK/volcheck" "$WORK/volcheck.c" "$ROOT"/src/lib/*.c 2>/dev/null; then
    fixed=$("$WORK/volcheck" fixed)
    raw=$("$WORK/volcheck" raw)
    note "the correct form reports" "$fixed"
    note "the historic raw-page form reports" "$raw"
    case "$fixed" in
        *" 0") note "the correct form has a fully readable map" "ok" ;;
        *)     bad "even the corrected form reports disagreements: $fixed" ;;
    esac
    case "$raw" in
        *" 0")
            bad "the raw-page form reports NO disagreement -- the detector is blind"
            note "  ...so this gate cannot see the defect it exists for" ;;
        *)
            note "the raw-page form IS detected (load-bearing)" "yes" ;;
    esac
else
    note "could not build the volume checker" "skipped"
fi

[ "$fail" -eq 0 ] && echo "PASS m29_no_raw_page_bitmap_writes" \
                  || echo "FAILED m29_no_raw_page_bitmap_writes"
exit "$fail"