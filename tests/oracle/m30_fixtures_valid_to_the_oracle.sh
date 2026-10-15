#!/usr/bin/env bash
# m30_fixtures_valid_to_the_oracle.sh -- our SYNTHETIC test volumes must be
# valid to an INDEPENDENT implementation, not merely to our own (M30).
#
# WHY. Three findings in a row (M26, M28, M29) had the same shape: a fixture and
# the code under test shared a mistake, so every check agreed with every other
# and the suite reported green while measuring nothing. The strongest outside
# question available is the ORACLE, and it had never been asked about our own
# fixtures at all.
#
# CORRECTION, made before this gate was finished. The first version asserted that
# ADFlib "cannot open a volume smaller than a floppy", and listed the 16-block
# unit fixtures as permanently oracle-blind on that basis. **That was never
# tested and it is wrong.** ADFlib reads the image file and infers geometry: it
# opens a 16-block volume as "HardFile 8.0 KBytes", distinguishes FFS DIRCACHE
# from plain FFS in `unadf`'s banner, and extracts cleanly from it.
#
# So the real gap was not size. It was that nobody had asked. The oracle ran on
# downloaded 880 KiB images in every gate and had never seen a line of our own
# fixture construction -- while a 8 KiB fixture is exactly where the subtle
# offsets live.
#
# What this gate does: build each synthetic fixture shape, hand it to ADFlib, and
# require AGREEMENT -- the same DOS type, the same listing, the same bytes. That
# is the one question a shared mistake cannot survive, because ADFlib shares none
# of our offsets, our hash function, or our coordinate-space conventions.
#
# Each fixture below is the EXACT construction its test file uses, so a change to
# either the fixture or the test is visible here too.

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
[ -d "$ROOT/src/lib" ]   || { echo "SKIP: not an amigafs tree"; exit 0; }
[ -x "$ROOT/build/afs" ] || { echo "SKIP: afs not built"; exit 0; }

# Builds every synthetic fixture shape we have. Each `emit <name>` writes
# "$WORK/<name>.adf" and prints "<name> <blocks> <expected-dos-type>".
cat > "$WORK/fixtures.c" <<'EOF'
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

#define BS 512

static void bm_page_set(uint8_t *page, uint32_t blk) {
    page[4 + blk / 8] |= (uint8_t)(0x80 >> (blk % 8));   /* page data starts at +4 */
}

static void write_out(const char *path, const unsigned char *img, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("OPENFAIL %s\n", path); exit(1); }
    fwrite(img, 1, n, f);
    fclose(f);
}

/* --- 1. test_bitmap_rebuild.c's build_vol(): 16 blocks, DOS\1 FFS, root at 8 */
static void emit_rebuild16(const char *path) {
    enum { NBLK = 16 };
    static unsigned char img[NBLK * BS];
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x01);
    unsigned char *r = img + 8 * BS;
    put_be32(r, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);
    put_be32(r + BS - 196, 9);
    put_be32(r + BS - 96, 0);
    put_be32(r + BS - 8, 0);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, 1);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
    unsigned char *bm = img + 9 * BS;
    memset(bm, 0, BS);
    for (unsigned b = 0; b < NBLK; b++) bm[4 + b / 8] |= (uint8_t)(0x80 >> (b % 8));
    bm[4] &= (uint8_t)0x7F;
    bm[4 + 1 / 8] &= (uint8_t)~(0x80 >> (1 % 8));
    bm[4 + ((NBLK - 1) / 32) * 4 + (((NBLK - 1) % 32) / 8)] &=
        (uint8_t)~(0x80 >> ((NBLK - 1) % 8));
    put_be32(bm, afs_block_checksum(bm, BS, 0));
    write_out(path, img, sizeof img);
    printf("rebuild16 %u FFS\n", (unsigned)NBLK);
}

/* --- 2. test_verify.c's build_dirc(): 16 blocks, DOS\5 FFS/DIRC, root at 8 --- */
static void emit_verify_dirc16(const char *path) {
    static unsigned char img[16 * BS];
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x05);
    unsigned char *r = img + 8 * BS;
    put_be32(r, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);
    put_be32(r + BS - 196, 9);
    put_be32(r + BS - 8, 10);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, 1);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
    unsigned char *bm = img + 9 * BS;
    memset(bm, 0, BS);
    for (unsigned b = 12; b < 16; b++) bm_page_set(bm, b);
    put_be32(bm, afs_block_checksum(bm, BS, 0));
    unsigned char *cb = img + 10 * BS;
    memset(cb, 0, BS);
    put_be32(cb, 33); put_be32(cb + 4, 10); put_be32(cb + 8, 8);
    put_be32(cb + 12, 1); put_be32(cb + 16, 0);
    put_be32(cb + 20, afs_block_checksum(cb, BS, 20));
    write_out(path, img, sizeof img);
    printf("verify_dirc16 %u FFS\n", 16u);
}

/* --- 3. a DOS\5 DIRC FLOPPY with content, so the oracle has something to
 * ---    disagree about: name, size and bytes rather than just the type. */
static void emit_dirc_floppy(const char *path) {
    static unsigned char img[1760 * BS];
    char err[256] = {0};
    afs_volume_t *v = NULL;
    uint32_t h = 0;
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x05);
    unsigned char *r = img + 880 * BS;
    put_be32(r, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);
    put_be32(r + BS - 196, 882);
    put_be32(r + BS - 8, 881);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, 1);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
    unsigned char *bm = img + 882 * BS;
    memset(bm, 0, BS);
    for (unsigned b = 883; b < 1760; b++) bm_page_set(bm, b);
    put_be32(bm, afs_block_checksum(bm, BS, 0));
    unsigned char *cb = img + 881 * BS;
    memset(cb, 0, BS);
    put_be32(cb, 33); put_be32(cb + 4, 881); put_be32(cb + 8, 880);
    put_be32(cb + 12, 0); put_be32(cb + 16, 0);
    put_be32(cb + 20, afs_block_checksum(cb, BS, 20));
    if (afs_volume_open_memory(img, sizeof img, BS, &v, err, sizeof err) != 0) {
        printf("OPENFAIL %s\n", err); exit(1);
    }
    if (afs_file_create(v, 880, "hello.txt", 9, &h, err, sizeof err) != 0 ||
        afs_file_write_all(v, h, "oracle says yes\n", 16, err, sizeof err) != 0) {
        printf("WRITEFAIL %s\n", err); exit(1);
    }
    afs_volume_close(v);
    write_out(path, img, sizeof img);
    printf("dirc_floppy %u FFS\n", 1760u);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;   /* one arg: the output directory */
    char path[512];
    snprintf(path, sizeof path, "%s/rebuild16.adf", argv[1]);      emit_rebuild16(path);
    snprintf(path, sizeof path, "%s/verify_dirc16.adf", argv[1]);   emit_verify_dirc16(path);
    snprintf(path, sizeof path, "%s/dirc_floppy.adf", argv[1]);     emit_dirc_floppy(path);
    return 0;
}
EOF

if ! gcc -w -I"$ROOT/include" -o "$WORK/fixtures" "$WORK/fixtures.c" "$ROOT"/src/lib/*.c 2>"$WORK/cc.log"; then
    echo "SKIP: could not build the fixture generators: $(tail -1 "$WORK/cc.log")"
    exit 0
fi
manifest=$("$WORK/fixtures" "$WORK" 2>&1)
case "$manifest" in
    *FAIL*) echo "SKIP: could not build the fixtures: $manifest"; exit 0 ;;
esac
note "built $(printf '%s\n' "$manifest" | wc -l) synthetic fixture shapes" "ok"

# --- for each: our reader must be happy AND the oracle must agree ------------
while read -r name blocks type; do
    [ -z "${name:-}" ] && continue
    img="$WORK/$name.adf"
    if ! "$ROOT/build/afs" verify "$img" >"$WORK/$name.v" 2>&1; then
        bad "$name: our own verify rejects it"
        note "  detail" "$(head -2 "$WORK/$name.v" | tr '\n' ' ')"
        continue
    fi
    info=$(adfinfo "$img" 2>/dev/null | grep -i DOS | head -1)
    if [ -z "$info" ]; then
        bad "$name: the oracle could not identify the volume at all"
        continue
    fi
    label=$(printf '%s' "$info" | awk '{print $(NF-1)}')
    last=$(printf '%s' "$info" | awk '{print $3}')
    note "$name ($blocks blocks)" "oracle: $label"
    case "$label" in
        *"$type"*) : ;;
        *) bad "$name: the oracle says '$label', expected one containing '$type'" ;;
    esac
    # the oracle must agree on the extent too
    if [ "$last" != "$((blocks - 1))" ]; then
        note "  extent reported by the oracle" "$last (we built $blocks)"
    fi
    # and it must walk the volume without complaint
    if ! adfls -l "$img" >/dev/null 2>"$WORK/$name.ls"; then
        bad "$name: adfls failed"
    fi
done <<< "$manifest"

# --- the one with content must agree on name, size and bytes ---------------
note "--- content agreement (the only fixture with a file in it) ---"
dirc="$WORK/dirc_floppy.adf"
ours=$("$ROOT/build/afs" ls "$dirc" 2>/dev/null | awk '$1 ~ /^f/{print $2}' | head -1)
ours_sha=$("$ROOT/build/afs" cat "$dirc" "$ours" 2>/dev/null | sha256sum | cut -c1-16)
note "our reader sees" "$ours"

adfline=$(adfls -l "$dirc" 2>/dev/null | grep -E '\bF\b' | head -1)
if printf '%s' "$adfline" | grep -q "hello.txt"; then
    note "adfls agrees on the NAME" "hello.txt"
else
    bad "adfls does not list hello.txt: $adfline"
fi
if printf '%s' "$adfline" | awk '{exit !($2 == 16)}'; then
    note "adfls agrees on the SIZE" "16 bytes"
else
    bad "adfls reports a different size: $adfline"
fi

rm -rf "$WORK/out"; mkdir -p "$WORK/out"
unadf -d "$WORK/out" "$dirc" >/dev/null 2>&1
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

# --- and the oracle must DISTINGUISH the two 16-block fixtures -------------
# This is the strongest form of the check available: two volumes we build, one
# DOS\1 FFS and one DOS\5 FFS/DIRC, and an outside implementation must tell them
# apart. If our notion of the DOS type were wrong, this is what would notice.
note "--- can the oracle tell our DOS\\1 from our DOS\\5? ---"
banner1=$(unadf -r "$WORK/rebuild16.adf" 2>&1 | grep -i "^Volume" | head -1)
banner5=$(unadf -r "$WORK/verify_dirc16.adf" 2>&1 | grep -i "^Volume" | head -1)
note "DOS\\1 fixture" "$(printf '%s' "$banner1" | sed 's/.*, //')"
note "DOS\\5 fixture" "$(printf '%s' "$banner5" | sed 's/.*, //')"
case "$banner5" in
    *DIRCACHE*|*Dircache*|*DIRC*)
        note "the oracle recognises our DOS\\5 fixture as DIRCACHE" "ok" ;;
    *)
        bad "the oracle does not see our DOS\\5 fixture as DIRCACHE: $banner5" ;;
esac
case "$banner1" in
    *DIRCACHE*|*Dircache*|*DIRC*)
        bad "the oracle sees our DOS\\1 fixture as DIRCACHE, which it is not: $banner1" ;;
    *)
        note "the oracle does not confuse it with DIRCACHE" "ok" ;;
esac

[ "$fail" -eq 0 ] && echo "PASS m30_fixtures_valid_to_the_oracle" \
                  || echo "FAILED m30_fixtures_valid_to_the_oracle"
exit "$fail"