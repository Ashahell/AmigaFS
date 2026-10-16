#!/usr/bin/env bash
# m28_strict_sees_bitmap_mismatch.sh -- the write fuzzer's check must be able
# to SEE a block the tree references being marked free (M28).
#
# THE FINDING. The write fuzzer's header claimed it asserted "bitmap
# agreement". It did not. It called afs_verify_memory(), which passes flags == 0,
# and bitmap agreement is the ONE thing AFS_VERIFY_STRICT adds. Measured: a
# volume whose live file header was marked free reported ZERO problems through
# that call, and ONE through the strict call:
#
#     [kind 4 block 2] block 2 is referenced by the tree but the bitmap marks it free
#
# That is M26's exact shape -- a block the tree references, marked free, so the
# next allocation hands it out and silently destroys live data -- invisible to
# the one harness whose whole job is to find exactly that.
#
# Two defects came out of this:
#   1. The harness now calls afs_verify_memory_ex(..., AFS_VERIFY_STRICT).
#   2. Its VOLUME had a broken bitmap page: it called afs_bitmap_set() (which
#      addresses an ASSEMBLED map, offset 0) on a raw PAGE, whose map data
#      starts at +4 because bytes 0..3 are the page's own checksum. Every bit
#      it meant to set went into the checksum word, so the allocator state the
#      fuzzer exercised was not the map any reader would see. Enabling the
#      check is what exposed that -- it reported blocks 880 and 881, the
#      rootblock and the bitmap page, as live-but-free.
#
# This gate pins the CAPABILITY, not the fix's presence: it builds the defect
# and requires strict to see it. A check that cannot detect the class it exists
# for is decorative, and that is precisely the failure this closes.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

fail=0
note() { printf '  %-52s %s\n' "$1" "$2"; }
bad()  { printf '  %-52s FAIL\n' "$1"; fail=1; }

WORK=$(mktemp -d /tmp/amigafs-m28.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

[ -d "$ROOT/src/lib" ] || { echo "SKIP: not an amigafs tree"; exit 77; }

cat > "$WORK/probe.c" <<'EOF'
/* Build the fuzzer's volume, create one file, then mark that file's live
 * header block FREE in the map -- M26's shape. Report what each verify entry
 * point sees. Prints: <everyday> <strict> */
#include <stdio.h>
#include <string.h>
#include "amigafs/be.h"
#include "amigafs/checksum.h"
#include "amigafs/dir.h"
#include "amigafs/file.h"
#include "amigafs/verify.h"
#include "amigafs/volume.h"

#define BS   512
#define NB   1760u
#define ROOT 880u
#define BMPG 881u

static uint8_t img[NB * BS];

static void bm_set(uint8_t *page, uint32_t blk) {
    page[4 + blk / 8] |= (uint8_t)(0x80 >> (blk % 8));   /* +4: page has a checksum long */
}

int main(void) {
    char err[256] = {0};
    afs_verify_report_t rep[16];
    afs_volume_t *v = NULL;
    uint32_t hdr = 0;

    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 1u);
    uint8_t *r = img + ROOT * BS;
    put_be32(r, 2);                                   /* T_HEADER */
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);
    put_be32(r + BS - 196, BMPG);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, 1);                           /* ST_ROOT */
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    uint8_t *bm = img + BMPG * BS;
    memset(bm, 0, BS);
    for (uint32_t i = 0; i < NB; i++)
        if (i != 0 && i != 1 && i != ROOT && i != BMPG) bm_set(bm, i);
    put_be32(bm, afs_block_checksum(bm, BS, 0));

    if (afs_volume_open_memory(img, sizeof img, BS, &v, err, sizeof err) != 0) {
        printf("OPENFAIL %s\n", err);
        return 1;
    }
    if (afs_file_create(v, ROOT, "victim", 6, &hdr, err, sizeof err) != 0) {
        printf("CREATEFAIL %s\n", err);
        return 1;
    }
    afs_volume_close(v);

    /* free the LIVE header in the map: the class of damage in question */
    bm = img + BMPG * BS;
    bm_set(bm, hdr);
    put_be32(bm, afs_block_checksum(bm, BS, 0));

    int n_plain  = afs_verify_memory(img, sizeof img, BS, rep, 16);
    int n_strict = afs_verify_memory_ex(img, sizeof img, BS, rep, 16,
                                        AFS_VERIFY_STRICT);
    printf("%d %d\n", n_plain, n_strict);
    return 0;
}
EOF

if ! gcc -w -I"$ROOT/include" -o "$WORK/probe" "$WORK/probe.c" "$ROOT"/src/lib/*.c 2>/dev/null; then
    echo "SKIP: could not build the probe"; exit 77
fi

out=$("$WORK/probe" 2>/dev/null)
case "$out" in
    *FAIL*) echo "SKIP: probe could not set up ($out)"; exit 77 ;;
esac

everyday=$(printf '%s' "$out" | awk '{print $1}')
strict=$(printf '%s' "$out" | awk '{print $2}')
note "a live block marked free: everyday reports" "$everyday"
note "a live block marked free: strict reports"  "$strict"

# The capability this gate exists to pin: STRICT must see it. If strict ever
# stops seeing this class, every harness that relies on it becomes decorative.
if [ "${strict:-0}" -gt 0 ]; then
    note "strict CAN detect referenced-but-free blocks" "ok"
else
    bad "strict cannot see a referenced-but-free block -- the safety net is decorative"
fi

# And the reason the fuzzer needed switching: the everyday check does NOT see
# it. This is not a defect in the everyday check (a stale map is ordinary on a
# real Amiga disk) -- it is the reason a WRITE harness must use strict.
if [ "${everyday:-0}" -eq 0 ]; then
    note "the everyday check cannot (documented, not a defect)" "0 problems"
else
    note "the everyday check now sees it too" "$everyday"
fi

# The harness must actually USE strict, and write page map data at +4.
#
# Asserted by BUILDING it and running it, not by grepping its source. The first
# version of this gate grepped for "AFS_VERIFY_STRICT" and went GREEN under both
# mutants, because the file's own COMMENT names that token -- so the check
# passed on the very defect it was written to catch. A grep can only confirm
# that a word is present; only running the harness can confirm what it does.
#
# The volume-build half is checked the same way: build the harness's volume
# layout, then ask verify --strict whether the rootblock and the bitmap page
# look live-but-free. With the +4 bug they do; with the fix they do not.
if gcc -w -I"$ROOT/include" -I"$ROOT/tests" -o "$WORK/wf" \
       "$ROOT/tests/write_fuzz.c" "$ROOT"/src/lib/*.c 2>"$WORK/wf.log"; then
    # one iteration, clean mode: enough to reach the initial volume check
    out=$(cd "$WORK" && timeout 120 "$WORK/wf" --seed=1 --iters=2 2>&1)
    if printf '%s' "$out" | grep -q "block 880\|block 881"; then
        bad "the harness's volume has a broken bitmap page (880/881 look free)"
    elif printf '%s' "$out" | grep -q "verify --strict after op: initial"; then
        bad "the harness fails its OWN initial strict check"
        note "  detail" "$(printf '%s' "$out" | head -3 | tr '\n' ' ')"
    else
        note "the harness builds a volume that passes strict" "ok"
    fi

    # Reverting the harness to the everyday call does NOT break its own volume:
    # its map is correct either way, so running it proves nothing about WHICH
    # check it calls. The only sound observable is the call itself -- and a
    # single-line grep cannot see it, because the flag sits on the next line
    # (and the file's comment names the token, so a loose grep matches either
    # way -- which is how the first version of this gate went green under the
    # very mutant it existed to catch).
    if tr '\n' ' ' < "$ROOT/tests/write_fuzz.c" \
       | grep -qE 'afs_verify_memory_ex *\( *g_img,([^)]*)AFS_VERIFY_STRICT'; then
        note "write_fuzz.c calls afs_verify_memory_ex(..., AFS_VERIFY_STRICT)" "ok"
    else
        bad "write_fuzz.c does NOT call the strict entry point"
    fi
else
    bad "could not build the write fuzzer: $(tail -1 "$WORK/wf.log")"
fi

[ "$fail" -eq 0 ] && echo "PASS m28_strict_sees_bitmap_mismatch" \
                  || echo "FAILED m28_strict_sees_bitmap_mismatch"
exit "$fail"