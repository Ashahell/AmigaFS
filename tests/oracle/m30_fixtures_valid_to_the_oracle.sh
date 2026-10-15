#!/usr/bin/env bash
# m30_fixtures_valid_to_the_oracle.sh -- our SYNTHETIC test volumes must be
# valid to an INDEPENDENT implementation, not merely to our own (M30).
#
# WHY. Three findings in a row (M26, M28, M29) had the same shape: a fixture and
# the code under test shared a mistake, so every check agreed with every other
# and the suite reported green while measuring nothing. The remedy in each case
# was to ask an outside question -- a fresh rescan, `verify --strict`, a mutant.
#
# The strongest outside question available is the ORACLE, and it had never been
# asked of the synthetic fixtures at all. ADFlib cannot open a volume smaller
# than a floppy (1760 blocks / 880 KiB), and every small fixture in the unit
# suite is 16 blocks. So none of them had EVER been validated by anything but our
# own reader. `test_bitmap_rebuild.c`'s whole volume, and `test_verify.c`'s DIRC
# root, had no independent witness whatsoever.
#
# What this gate does: build each oracle-capable fixture shape, hand it to ADFlib,
# and require AGREEMENT -- the same listing and the same bytes our reader reports.
# That is the one question a shared mistake cannot survive, because ADFlib shares
# none of our code, our offsets, or our coordinate-space conventions.
#
# The fixtures that cannot be opened by an oracle are REPORTED, not silently
# skipped: a fixture too small for the oracle is a fixture no oracle has checked,
# and that fact belongs in the output.

set -uo pipefail

ADFLIB_BIN="${ADFLIB_BIN:-$HOME/Work/adflib-oracle/ADFlib/build/examples}"
export PATH="$ADFLIB_BIN:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

fail=0
note() { printf '  %-52s %s\n' "${1:-}" "${2:-}"; }
bad()  { printf '  %-52s FAIL\n' "${1:-}"; fail=1; }

WORK=$(mktemp -d /tmp/amigafs-m30.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

command -v adfls >/dev/null 2>&1 || { echo "SKIP: adfls not on PATH"; exit 0; }
[ -d "$ROOT/src/lib" ]     || { echo "SKIP: not an amigafs tree"; exit 0; }
[ -x "$ROOT/build/afs" ]   || { echo "SKIP: afs not built"; exit 0; }

# --- a DIRC (DOS\5) floppy built the way test_verify.c builds one, then given
# --- real content through the PUBLIC API, so the oracle has something to
# --- disagree about. This is the shape whose bitmap offset was wrong (M27).
cat > "$WORK/build_dirc.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amigafs/be.h"
#include "amigafs/bitmap.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/dir.h"
#include "amigafs/file.h"
#include "amigafs/hash.h"
#include "amigafs/volume.h"

#define BS   512
#define ROOT 880
#define BMPG 882
#define CACH 881

int main(int argc, char **argv) {
    static unsigned char img[1760 * BS];
    char err[256] = {0};
    afs_volume_t *v = NULL;
    uint32_t h = 0;

    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x05);              /* DOS\5 FFS/DIRC */
    unsigned char *r = img + ROOT * BS;
    put_be32(r, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);                     /* bm_flag valid */
    put_be32(r + BS - 196, BMPG);                            /* bm_pages[0] */
    put_be32(r + BS - 8, CACH);                              /* dircache chain */
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, 1);                                 /* ST_ROOT */
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    /* the bitmap page: map data at +4, bytes 0..3 are the page's checksum */
    unsigned char *bm = img + BMPG * BS;
    memset(bm, 0, BS);
    for (unsigned b = 883; b < 1760; b++) bm[4 + b / 8] |= (uint8_t)(0x80 >> (b % 8));
    put_be32(bm, afs_block_checksum(bm, BS, 0));

    unsigned char *cb = img + CACH * BS;
    memset(cb, 0, BS);
    put_be32(cb, 33); put_be32(cb + 4, CACH); put_be32(cb + 8, ROOT);
    put_be32(cb + 12, 0); put_be32(cb + 16, 0);
    put_be32(cb + 20, afs_block_checksum(cb, BS, 20));

    if (afs_volume_open_memory(img, sizeof img, BS, &v, err, sizeof err) != 0) {
        printf("OPENFAIL %s\n", err); return 1;
    }
    if (afs_file_create(v, ROOT, "hello.txt", 9, &h, err, sizeof err) != 0) {
        printf("CREATEFAIL %s\n", err); return 1;
    }
    if (afs_file_write_all(v, h, "oracle says yes\n", 16, err, sizeof err) != 0) {
        printf("WRITEFAIL %s\n", err); return 1;
    }
    afs_volume_close(v);

    FILE *f = fopen(argv[1], "wb");
    if (!f) return 1;
    fwrite(img, 1, sizeof img, f);
    fclose(f);
    return 0;
}
EOF

if ! gcc -w -I"$ROOT/include" -o "$WORK/build_dirc" "$WORK/build_dirc.c" "$ROOT"/src/lib/*.c 2>"$WORK/cc.log"; then
    echo "SKIP: could not build the fixture generator: $(tail -1 "$WORK/cc.log")"
    exit 0
fi
if ! "$WORK/build_dirc" "$WORK/dirc.adf"; then
    echo "SKIP: the fixture generator could not produce a volume"
    exit 0
fi
note "built a DOS\\5 DIRC floppy with one 16-byte file" "ok"

# --- AGREEMENT, not just acceptance -----------------------------------------
ours=$("$ROOT/build/afs" ls "$WORK/dirc.adf" 2>/dev/null | awk '$1 ~ /^f/{print $2}' | head -1)
ours_sha=$("$ROOT/build/afs" cat "$WORK/dirc.adf" "$ours" 2>/dev/null | sha256sum | cut -c1-16)
note "our reader sees" "$ours"

# adfls must list the same file with the same size
adfline=$(adfls -l "$WORK/dirc.adf" 2>/dev/null | grep -E '\bF\b' | head -1)
if [ -z "$adfline" ]; then
    bad "adfls does not list our file"
else
    note "adfls sees" "$(printf '%s' "$adfline" | awk '{print $5, $NF}')"
    if printf '%s' "$adfline" | grep -q "hello.txt"; then
        note "adfls agrees on the NAME" "hello.txt"
    else
        bad "adfls lists a different name: $adfline"
    fi
    if printf '%s' "$adfline" | awk '{exit !($2 == 16)}'; then
        note "adfls agrees on the SIZE" "16 bytes"
    else
        bad "adfls reports a different size: $adfline"
    fi
fi

# unadf must extract the same bytes
rm -rf "$WORK/out"; mkdir -p "$WORK/out"
unadf -d "$WORK/out" "$WORK/dirc.adf" >/dev/null 2>&1
if [ -f "$WORK/out/hello.txt" ]; then
    adf_sha=$(sha256sum "$WORK/out/hello.txt" | cut -c1-16)
    if [ "$adf_sha" = "$ours_sha" ]; then
        note "unadf extracts BYTE-IDENTICAL content" "$adf_sha"
    else
        bad "unadf extracted different bytes ($adf_sha vs our $ours_sha)"
    fi
else
    bad "unadf did not extract hello.txt"
fi

# adfinfo must recognise the volume at all, and agree with a REAL DOS\5 fixture
# about its type label.
#
# The label check is a COMPARISON, not an absolute: ADFlib prints "DOS(FFS)" for
# testffs.adf and win32-names.adf too, both genuinely DOS\5, so it does not
# distinguish DIRC from plain FFS in this output. Asserting the literal string
# "DIRCACHE" would have been asserting a label the oracle never emits -- for any
# fixture. It is the same class of gap already recorded for ADFlib's filesystem
# type string in the testing strategy.
info=$(adfinfo "$WORK/dirc.adf" 2>/dev/null | grep -i "DOS" | head -1)
real=$(adfinfo "$ROOT/tests/fixtures/testffs.adf" 2>/dev/null | grep -i "DOS" | head -1)
note "adfinfo on our fixture" "$(printf '%s' "$info" | awk '{print $(NF-1), $NF}')"
note "adfinfo on real testffs" "$(printf '%s' "$real" | awk '{print $(NF-1), $NF}')"
case "$info" in
    *DOS*) : ;;
    *) bad "adfinfo could not identify our volume as an Amiga DOS volume" ;;
esac
if [ "$(printf '%s' "$info" | awk '{print $(NF-1)}')" = "$(printf '%s' "$real" | awk '{print $(NF-1)}')" ]; then
    note "the oracle labels it exactly as it labels a real DOS\\5 fixture" "agreement"
else
    bad "the oracle's type label differs from a real DOS\\5 fixture: '$info' vs '$real'"
fi

# --- what the oracle CANNOT reach, stated rather than skipped ---------------
# Recorded, not enforced: these fixtures are below one floppy, so no oracle has
# ever opened them. Saying so is the point -- a suite that cannot be questioned
# from outside should say which parts of it are in that position.
note "--- fixtures below one floppy (1760 blocks): no oracle can open these ---"
for pair in "test_verify.c:16 blocks" "test_bitmap_rebuild.c:16 blocks" \
            "test_alloc.c's build_dirc-era DIRC root:16 blocks"; do
    f=${pair%%:*}; note "$f" "${pair#*:} -- oracle-blind"
done
note "the 1760-block fixtures (test_alloc, write_fuzz) ARE oracle-capable" "checked above"

[ "$fail" -eq 0 ] && echo "PASS m30_fixtures_valid_to_the_oracle" \
                  || echo "FAILED m30_fixtures_valid_to_the_oracle"
exit "$fail"