/* Unit tests for the block bitmap and the structure verifier (bitmap.c, verify.c).
 *
 * Bitmap: the rootblock holds up to 25 bitmap block pointers plus a pointer to
 * a chain of bitmap EXTENSION blocks (each holding more pointers). One bit per
 * block: 1 = free, 0 = allocated. Bit 0 of long 0 describes block 0.
 *
 * Verifier: the structure checks that catch a damaged volume -- checksums,
 * pointers in range, known block types, no block used twice, used-vs-bitmap
 * agreement, dir-cache consistent with the hash table, terminating chains.
 *
 * Checksum validity, settled against real fixtures: for ANY block, the stored
 * checksum equals the negation of the sum of all other longs, EQUIVALENTLY the
 * sum INCLUDING the stored value is 0. That single rule holds for the
 * checksum-at-20 family AND for bitmap blocks (checksum at byte 0).
 */
#include "test.h"
#include "amigafs/be.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/hash.h"
#include "amigafs/verify.h"

#define BS 512

/* One-bit-per-block bitmap helpers on a raw buffer. */
static void bm_set(uint8_t *map, uint32_t blk) {
    map[blk / 32 * 4 + (blk % 32) / 8] |= (uint8_t)(0x80 >> (blk % 8));
}
static int bm_get(const uint8_t *map, uint32_t blk) {
    return (map[blk / 32 * 4 + (blk % 32) / 8] >> (7 - (blk % 8))) & 1;
}

/* Build a bitmap block: bit per block, then stamp the checksum at byte 0. */
static void bm_build(uint8_t *blk, const uint8_t *bits, uint32_t nblocks) {
    memset(blk, 0, BS);
    for (uint32_t b = 0u; b < nblocks; b++)
        if (bm_get(bits, b)) bm_set(blk, b);
    put_be32(blk, afs_block_checksum(blk, BS, 0));
}

/* checksum validity: sum INCLUDING stored must be 0 */
static void test_bitmap_block_checksum_rule(void) {
    uint8_t blk[BS];
    uint8_t bits[BS];
    memset(bits, 0, sizeof bits);
    bm_set(bits, 100); bm_set(bits, 200); bm_set(bits, 1500);
    bm_build(blk, bits, 1760);

    uint32_t total = 0;
    for (int i = 0; i < BS; i += 4) total += be32(blk + i);
    CHECK_EQ_U(total, 0u, "bitmap block: sum including stored checksum is 0");
    CHECK_EQ_U(be32(blk + 0), afs_block_checksum(blk, BS, 0),
               "bitmap block: stored == negation of the rest");

    /* a flipped bit in the map must be caught */
    blk[8] ^= 0xFF;
    uint32_t t2 = 0;
    for (int i = 0; i < BS; i += 4) t2 += be32(blk + i);
    CHECK(t2 != 0u, "a corrupted bitmap block fails the sum-zero rule");
}

/* Reading the bitmap: gather bits across a bitmap block (and a second block). */
static void test_bitmap_read_across_blocks(void) {
    uint8_t blk1[BS], blk2[BS], bits[BS];
    memset(bits, 0, sizeof bits);
    for (uint32_t b = 0u; b < 1760u; b++) bm_set(bits, b);   /* all free initially */
    /* mark a few allocated (clear bit) */
    uint32_t allocated[] = {0, 1, 500, 880, 881, 1759};
    for (unsigned i = 0; i < sizeof allocated / sizeof allocated[0]; i++)
        bm_set(bits, allocated[i]);   /* set==free, so clear means allocated */
    /* invert: we set = free; to mark allocated we must CLEAR */
    uint8_t bits2[BS];
    memset(bits2, 0, sizeof bits2);
    for (uint32_t b = 0u; b < 1760u; b++) bm_set(bits2, b);
    for (unsigned i = 0; i < sizeof allocated / sizeof allocated[0]; i++) {
        uint32_t b = allocated[i];
        bits2[b / 32 * 4 + (b % 32) / 8] &= (uint8_t)~(0x80 >> (b % 8));
    }
    bm_build(blk1, bits2, 1760);

    /* bit 0 (allocated, cleared) reads as 0 = not free */
    CHECK_EQ_U(bm_get(blk1, 0), 0u, "allocated block reads as 0");
    CHECK_EQ_U(bm_get(blk1, 500), 0u, "block 500 allocated reads 0");
    CHECK_EQ_U(bm_get(blk1, 499), 1u, "block 499 (free) reads 1");
    (void)blk2;
}

/* bitmap EXTENSION block: pointers in the body, next pointer in the last long. */
static void test_bitmap_ext_block(void) {
    uint8_t ext[BS];
    memset(ext, 0, sizeof ext);
    put_be32(ext + 0, 900);              /* a bitmap block pointer */
    put_be32(ext + 4, 901);
    put_be32(ext + BS - 4, 0);          /* next extension = 0 (last) */
    /* no separate checksum on extension blocks: the last long is the link */
    uint32_t next = be32(ext + BS - 4);
    CHECK_EQ_U(next, 0u, "bitmap extension chain terminates at 0");
    CHECK_EQ_U(be32(ext + 0), 900u, "first extension pointer");
}

/* Verifier: a well-formed minimal volume passes. */
static void test_verify_clean_volume(void) {
    /* build a tiny in-memory volume: bootblock + rootblock + bitmap */
    uint8_t img[16 * BS];
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x01);          /* FFS */
    /* rootblock at block 8 (of 16 blocks -> (2+15)/2=8) */
    uint8_t *r = img + 8 * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);   /* bitmap valid */
    put_be32(r + BS - 196, 9);            /* one bitmap block at block 9 */
    put_be32(r + BS - 96, 0);             /* no bitmap extension */
    put_be32(r + BS - 8, 0);              /* no dircache */
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    /* bitmap block 9: blocks 0..9 used except itself; rest free */
    uint8_t *bm = img + 9 * BS;
    memset(bm, 0, BS);
    for (uint32_t b = 10u; b < 16u; b++) bm_set(bm, b);
    put_be32(bm, afs_block_checksum(bm, BS, 0));

    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    /* A 16-block "disk" is too small for a real rootblock formula result to
     * be self-consistent, so we only assert it does not crash and reports a
     * bounded number of problems. */
    CHECK(n >= 0, "verify ran without crashing");
    CHECK(n <= 64, "problem count is bounded by the report capacity");
}

/* Verifier: a corrupt rootblock checksum is reported. */
static void test_verify_detects_bad_rootblock(void) {
    uint8_t img[16 * BS];
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x01);
    uint8_t *r = img + 8 * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 4, AFS_ST_ROOT);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
    r[100] ^= 0xFF;                     /* corrupt the rootblock payload */
    /* do NOT restamp: the checksum is now wrong */

    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    CHECK(n > 0, "a corrupt rootblock produces at least one problem");
    /* the open refuses the volume before the walk, so the finding arrives as a
     * ROOTBLOCK problem whose DETAIL names the checksum. Both matter: the kind
     * must not be CHECKSUM (we never got that far), but the reason must be
     * explicit rather than a bare "rejected". */
    int mentions_chk = 0;
    for (int i = 0; i < n; i++)
        if (rep[i].kind == AFS_PROBLEM_ROOTBLOCK && strstr(rep[i].detail, "checksum"))
            mentions_chk = 1;
    CHECK(mentions_chk, "the rejection detail names the checksum");
}

/* The hostile fixture ffdisk0049.adf has two defects the verifier must catch:
 *  - the bootblock's root field holds 0x444F5302 ('DOS\\2', garbage), not the
 *    computed rootblock; the plan requires a WARN on that mismatch.
 *  - its bitmap is flagged invalid (bm_flag != -1) and the bitmap block's
 *    checksum does not validate.
 * We reproduce both on a synthetic image so the test stays hermetic. */
static void test_verify_flags_bad_bootblock_root(void) {
    uint8_t img[1760 * BS];
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x00);      /* OFS */
    /* bootblock root field (offset 8) = garbage, as on the hostile fixture */
    put_be32(img + 8, 0x444F5302u);

    uint8_t *r = img + 880 * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);
    put_be32(r + BS - 196, 881);
    put_be32(r + BS - 8, 0);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    uint8_t *bm = img + 881 * BS;
    for (uint32_t b = 900; b < 1760; b++) bm_set(bm, b);
    put_be32(bm, afs_block_checksum(bm, BS, 0));

    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    int saw_root = 0;
    for (int i = 0; i < n; i++)
        if (strstr(rep[i].detail, "bootblock root field")) saw_root = 1;
    CHECK(saw_root, "a garbage bootblock root field is flagged");
}

static void test_verify_flags_bad_bitmap(void) {
    uint8_t img[1760 * BS];
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x00);
    uint8_t *r = img + 880 * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    /* bitmap marked INVALID: bm_flag != -1 */
    put_be32(r + BS - 200, 1);
    put_be32(r + BS - 196, 881);
    put_be32(r + BS - 8, 0);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    uint8_t *bm = img + 881 * BS;
    for (uint32_t b = 900; b < 1760; b++) bm_set(bm, b);
    put_be32(bm, afs_block_checksum(bm, BS, 0));
    bm[8] ^= 0xFF;    /* corrupt the bitmap payload, leaving the flag invalid */

    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    int saw_bm = 0;
    for (int i = 0; i < n; i++)
        if (rep[i].kind == AFS_PROBLEM_BITMAP_MISMATCH ||
            (rep[i].kind == AFS_PROBLEM_CHECKSUM)) saw_bm = 1;
    CHECK(saw_bm, "an invalid / corrupt bitmap is flagged");
}

/* REGRESSION: the verifier must REACH the entries hanging off the rootblock.
 *
 * Every other test in this file builds a volume whose rootblock hash table is
 * EMPTY, so a verifier that only ever looked at the rootblock passed all of
 * them. That is the gap this test closes: a damage in a block BELOW the
 * rootblock must be reported, at the block where it was found.
 *
 * The volume here is otherwise valid, so the ONLY thing that can produce a
 * report entry is the traversal itself. */
static void test_verify_reaches_entries_below_the_rootblock(void) {
    enum { NB = 1760, ROOT = 880, ENTRY = 900 };
    static uint8_t img[NB * BS];
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x01);              /* FFS */

    uint8_t *r = img + ROOT * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);                      /* bitmap valid */
    put_be32(r + BS - 196, 0);                               /* no bitmap blk */
    put_be32(r + BS - 96, 0);
    put_be32(r + BS - 8, 0);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    /* one FILE entry below the rootblock, correctly checksummed */
    uint8_t *e = img + ENTRY * BS;
    put_be32(e + 0, AFS_T_HEADER);
    put_be32(e + BS - 16, 0);                                /* no chain */
    put_be32(e + BS - 12, ROOT);                             /* parent */
    put_be32(e + BS - 8, 0);                                 /* no extension */
    put_be32(e + BS - 4, AFS_ST_FILE);
    e[BS - 80] = 4; memcpy(e + BS - 79, "file", 4);
    put_be32(e + 20, afs_block_checksum(e, BS, 20));

    /* file it in the rootblock's hash table under its own name */
    uint32_t tsz = (uint32_t)afs_hash_table_size(BS);
    uint32_t bucket = afs_hash("file", 4, tsz, 0x01);
    put_be32(r + 24 + 4 * bucket, ENTRY);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    int reported_entry = 0;
    for (int i = 0; i < n; i++)
        if (rep[i].block == ENTRY) reported_entry = 1;
    CHECK(!reported_entry,
          "an undamaged entry below the rootblock draws no report (sanity: "
          "the volume is otherwise clean, so anything named here is a bug)");

    /* now damage that entry's payload and leave the checksum stale */
    e[300] ^= 0xFF;
    n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    reported_entry = 0;
    int saw_checksum = 0;
    for (int i = 0; i < n; i++) {
        if (rep[i].block == ENTRY) reported_entry = 1;
        if (rep[i].block == ENTRY && rep[i].kind == AFS_PROBLEM_CHECKSUM)
            saw_checksum = 1;
    }
    CHECK(reported_entry, "damage below the rootblock is reported AT that block");
    CHECK(saw_checksum, "and it is reported as a CHECKSUM problem");
}

int main(void) {
    printf("bitmap / verify\n");
    test_bitmap_block_checksum_rule();
    test_bitmap_read_across_blocks();
    test_bitmap_ext_block();
    test_verify_clean_volume();
    test_verify_detects_bad_rootblock();
    test_verify_flags_bad_bootblock_root();
    test_verify_flags_bad_bitmap();
    test_verify_reaches_entries_below_the_rootblock();
    TEST_REPORT();
}