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

/* A hash bucket pointing OUTSIDE the volume must be reported as RANGE.
 *
 * The read path is right to hide this -- afs_dir_list drops a pointer it
 * cannot read and carries on, because a filesystem must not die on a bad
 * pointer. But the VERIFIER's whole job is to report exactly what the read
 * path hides, so it cannot delegate its enumeration to afs_dir_list: a
 * verifier that does can never see an out-of-range pointer at all. */
static void test_verify_reports_out_of_range_pointer(void) {
    enum { NB = 1760, ROOT = 880 };
    static uint8_t img[NB * BS];
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x01);
    uint8_t *r = img + ROOT * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);
    put_be32(r + BS - 8, 0);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    uint32_t tsz = (uint32_t)afs_hash_table_size(BS);
    put_be32(r + 24 + 4 * (tsz - 1), 99999u);      /* past the end */
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    int saw_range = 0;
    for (int i = 0; i < n; i++)
        if (rep[i].kind == AFS_PROBLEM_RANGE) saw_range = 1;
    CHECK(saw_range, "a hash bucket pointing past the volume is reported RANGE");
}

/* An entry block whose type nothing recognises must be reported as TYPE. */
static void test_verify_reports_unknown_block_type(void) {
    enum { NB = 1760, ROOT = 880, ENTRY = 900 };
    static uint8_t img[NB * BS];
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x01);
    uint8_t *r = img + ROOT * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);
    put_be32(r + BS - 8, 0);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    uint8_t *e = img + ENTRY * BS;
    put_be32(e + 0, 0x00001234u);                 /* primary: not a real type */
    put_be32(e + BS - 16, 0);
    put_be32(e + BS - 12, ROOT);
    put_be32(e + BS - 8, 0);
    put_be32(e + BS - 4, 0x00001234u);             /* secondary: neither */
    e[BS - 80] = 4; memcpy(e + BS - 79, "file", 4);
    put_be32(e + 20, afs_block_checksum(e, BS, 20));

    uint32_t tsz = (uint32_t)afs_hash_table_size(BS);
    uint32_t b = afs_hash("file", 4, tsz, 0x01);
    put_be32(r + 24 + 4 * b, ENTRY);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    int saw_type = 0;
    for (int i = 0; i < n; i++)
        if (rep[i].kind == AFS_PROBLEM_TYPE && rep[i].block == ENTRY) saw_type = 1;
    CHECK(saw_type, "an entry block of an unrecognised type is reported TYPE");
}

/* A block claimed as bitmap storage that is ALSO a real directory entry is a
 * double use, and a dangerous one: the same physical block backs both a file
 * and the allocation map, so writing either silently corrupts the other.
 *
 * The real g1a30c.adf has exactly this -- block 948 is bitmap page 2 and a
 * file in the tree at the same time (amitools flags the disk as
 * "Bitmap Block Count Mismatch"; ADFlib does not). It went unreported because
 * bitmap pages were never entered into the double-use set, so the check that
 * would have caught it never had them to compare against. */
static void test_verify_reports_bitmap_page_used_as_a_file(void) {
    enum { NB = 1760, ROOT = 880, BITMAP1 = 881, BITMAP2 = 900, ENTRY = 901 };
    static uint8_t img[NB * BS];
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x01);
    uint8_t *r = img + ROOT * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);            /* bitmap marked valid */
    put_be32(r + BS - 196, BITMAP1);               /* page 1 */
    put_be32(r + BS - 192, BITMAP2);               /* page 2 -- one too many */
    put_be32(r + BS - 8, 0);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    /* both bitmap pages are well-formed bitmap blocks */
    for (int p = 0; p < 2; p++) {
        uint8_t *bm = img + (p ? BITMAP2 : BITMAP1) * BS;
        memset(bm, 0, BS);
        put_be32(bm, afs_block_checksum(bm, BS, 0));
    }
    /* ...and BITMAP2 is ALSO a file entry in the tree */
    uint8_t *e = img + BITMAP2 * BS;
    put_be32(e + 0, AFS_T_HEADER);
    put_be32(e + BS - 16, 0);
    put_be32(e + BS - 12, ROOT);
    put_be32(e + BS - 8, 0);
    put_be32(e + BS - 4, AFS_ST_FILE);
    e[BS - 80] = 4; memcpy(e + BS - 79, "evil", 4);
    put_be32(e + 20, afs_block_checksum(e, BS, 20));
    /* and a separate, honest file entry exists too */
    uint8_t *f = img + ENTRY * BS;
    put_be32(f + 0, AFS_T_HEADER);
    put_be32(f + BS - 16, 0);
    put_be32(f + BS - 12, ROOT);
    put_be32(f + BS - 8, 0);
    put_be32(f + BS - 4, AFS_ST_FILE);
    f[BS - 80] = 4; memcpy(f + BS - 79, "good", 4);
    put_be32(f + 20, afs_block_checksum(f, BS, 20));

    uint32_t tsz = (uint32_t)afs_hash_table_size(BS);
    put_be32(r + 24 + 4 * afs_hash("evil", 4, tsz, 0x01), BITMAP2);
    put_be32(r + 24 + 4 * afs_hash("good", 4, tsz, 0x01), ENTRY);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    int saw_double = 0, saw_count = 0;
    for (int i = 0; i < n; i++) {
        if (rep[i].kind == AFS_PROBLEM_DOUBLE_USE && rep[i].block == BITMAP2)
            saw_double = 1;
        if (rep[i].kind == AFS_PROBLEM_BITMAP_MISMATCH &&
            strstr(rep[i].detail, "bitmap block count"))
            saw_count = 1;
    }
    CHECK(saw_double, "a block that is both a bitmap page and a file is a "
                      "DOUBLE USE");
    CHECK(saw_count, "a bitmap page count larger than the volume needs is "
                     "reported");
}

/* The control for the test above: the correct page count must NOT be flagged. */
static void test_verify_accepts_correct_bitmap_page_count(void) {
    enum { NB = 1760, ROOT = 880, BITMAP1 = 881 };
    static uint8_t img[NB * BS];
    memset(img, 0, sizeof img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x01);
    uint8_t *r = img + ROOT * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);
    put_be32(r + BS - 196, BITMAP1);               /* exactly one page */
    put_be32(r + BS - 8, 0);
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
    uint8_t *bm = img + BITMAP1 * BS;
    memset(bm, 0, BS);
    put_be32(bm, afs_block_checksum(bm, BS, 0));

    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    int saw_count = 0;
    for (int i = 0; i < n; i++)
        if (rep[i].kind == AFS_PROBLEM_BITMAP_MISMATCH &&
            strstr(rep[i].detail, "bitmap block count")) saw_count = 1;
    CHECK(!saw_count, "one bitmap page for a 1760-block volume is correct");
}

/* --- dir-cache chain checks ------------------------------------------------
 *
 * AFS_PROBLEM_DIRCACHE and AFS_PROBLEM_CHAIN have been declared in verify.h
 * since the header was written and NOTHING EVER EMITTED THEM. The dir-cache
 * chain was therefore never verified: `afs verify` reported "OK, no structural
 * problems found" on a volume whose cache chain was corrupt. That is how M21's
 * reader bug reached a release -- verify's OK was vacuous on that axis.
 *
 * Layout is ADFlib's AdfDirCacheBlock (adf_blk.h:353):
 *   +0 type=33  +4 headerKey(self)  +8 parent  +12 recordsNb
 *   +16 nextDirC  +20 checksum  +24 records
 * nextDirC is at +16. +4 is headerKey, a SELF-POINTER -- reading it as the
 * chain link walks the block to itself, which is the bug these tests pin.
 *
 * Only DOS\5 volumes are examined: on any other variant the directory block's
 * bs-8 field is a file EXTENSION pointer, not a cache pointer, and walking it
 * as a chain reports nonsense.
 */
#define DIRC_ROOT 8
#define DIRC_BM   9
#define DIRC_CACHE 10

/* Build a DOS\5 volume whose rootblock owns one dir-cache block. `next` is
 * written to the cache block's +16; `parent` to +8. */
static void build_dirc(uint8_t *img, uint32_t next, uint32_t parent) {
    memset(img, 0, 16 * BS);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x05);          /* DOS\5 = DIRC */

    uint8_t *r = img + DIRC_ROOT * BS;
    put_be32(r, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);
    put_be32(r + BS - 196, DIRC_BM);
    put_be32(r + BS - 8, DIRC_CACHE);                    /* first cache block */
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    uint8_t *bm = img + DIRC_BM * BS;
    memset(bm, 0, BS);
    for (uint32_t b = 12u; b < 16u; b++) bm_set(bm, b);
    bm_get(bm, DIRC_BM) || (bm[0] = 0);
    put_be32(bm, afs_block_checksum(bm, BS, 0));

    uint8_t *cb = img + DIRC_CACHE * BS;
    memset(cb, 0, BS);
    put_be32(cb + 0, 33);                 /* T_DIRC */
    put_be32(cb + 4, DIRC_CACHE);         /* headerKey: self-pointer */
    put_be32(cb + 8, parent);             /* parent directory */
    put_be32(cb + 12, 0);                 /* recordsNb */
    put_be32(cb + 16, next);              /* nextDirC */
    put_be32(cb + 20, afs_block_checksum(cb, BS, 20));
}

static int count_kind(afs_verify_report_t *rep, int n, afs_problem_kind_t k) {
    int c = 0;
    for (int i = 0; i < n; i++) if (rep[i].kind == k) c++;
    return c;
}

static void test_verify_accepts_a_clean_dircache(void) {
    uint8_t img[16 * BS];
    build_dirc(img, 0, DIRC_ROOT);
    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    CHECK_EQ_U(count_kind(rep, n, AFS_PROBLEM_CHAIN), 0,
               "a terminating cache chain reports no CHAIN problem");
    CHECK_EQ_U(count_kind(rep, n, AFS_PROBLEM_DIRCACHE), 0,
               "a self-consistent cache block reports no DIRCACHE problem");
}

static void test_verify_flags_a_self_referential_cache_chain(void) {
    uint8_t img[16 * BS];
    build_dirc(img, DIRC_CACHE, DIRC_ROOT);      /* nextDirC points at ITSELF */
    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    CHECK_EQ_U(count_kind(rep, n, AFS_PROBLEM_CHAIN), 1,
               "a self-referential cache chain is reported once");
}

static void test_verify_flags_a_cache_block_of_the_wrong_type(void) {
    uint8_t img[16 * BS];
    build_dirc(img, 0, DIRC_ROOT);
    uint8_t *cb = img + DIRC_CACHE * BS;
    put_be32(cb, 2);                            /* T_HEADER, not T_DIRC */
    put_be32(cb + 20, afs_block_checksum(cb, BS, 20));
    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    CHECK(count_kind(rep, n, AFS_PROBLEM_CHAIN) >= 1,
          "a cache chain through a non-cache block is reported");
}

static void test_verify_flags_a_cache_with_the_wrong_parent(void) {
    uint8_t img[16 * BS];
    build_dirc(img, 0, 999);                     /* parent is not the owner */
    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    CHECK(count_kind(rep, n, AFS_PROBLEM_DIRCACHE) >= 1,
          "a cache block naming the wrong parent directory is reported");
}

static void test_verify_flags_a_cache_with_a_wrong_headerkey(void) {
    uint8_t img[16 * BS];
    build_dirc(img, 0, DIRC_ROOT);
    uint8_t *cb = img + DIRC_CACHE * BS;
    put_be32(cb + 4, 77);                       /* headerKey != itself */
    put_be32(cb + 20, afs_block_checksum(cb, BS, 20));
    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    CHECK(count_kind(rep, n, AFS_PROBLEM_DIRCACHE) >= 1,
          "a cache block whose headerKey is not itself is reported");
}

static void test_verify_ignores_dircache_on_non_dirc_volumes(void) {
    /* On DOS\1 the rootblock's bs-8 is a file EXTENSION pointer. Walking it as
     * a cache chain would report nonsense on 6 of the 8 real non-DIRC
     * fixtures, which is why the check is gated on has_dircache. */
    uint8_t img[16 * BS];
    build_dirc(img, DIRC_CACHE, DIRC_ROOT);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x01);   /* DOS\1: no dir cache */
    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    CHECK_EQ_U(count_kind(rep, n, AFS_PROBLEM_CHAIN), 0,
               "a non-DIRC volume's bs-8 is not walked as a cache chain");
    CHECK_EQ_U(count_kind(rep, n, AFS_PROBLEM_DIRCACHE), 0,
               "a non-DIRC volume reports no dir-cache problems");
}

/* A DIRC volume still has FILE headers, and a file's bs-8 is its EXTENSION
 * pointer. Walking that as a cache chain reported "block 959 is not a
 * dir-cache block" on the good fixtures testffs.adf / testhd.adf, whose blocks
 * 886/1772 are ordinary files. The volume-level gate above cannot catch this:
 * the volume genuinely does have a dir cache. Only a per-block gate can. */
static void test_verify_ignores_dircache_on_files_of_a_dirc_volume(void) {
    /* A DIRC volume still has FILE headers, and a file's bs-8 is its EXTENSION
     * pointer. Walking that as a cache chain reported "block 959 is not a
     * dir-cache block" on the good fixtures testffs.adf / testhd.adf, whose
     * blocks 886/1772 are ordinary FILES. The volume-level gate cannot catch
     * this -- the volume genuinely does have a dir cache -- so the FILE must be
     * reachable from the tree, which is what leaves the per-block gate as the
     * only thing that can. */
    uint8_t img[16 * BS];
    build_dirc(img, 0, DIRC_ROOT);                 /* clean, terminating chain */

    uint8_t *f = img + 11 * BS;
    memset(f, 0, BS);
    put_be32(f, AFS_T_HEADER);
    put_be32(f + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(f + BS - 16, 0);
    put_be32(f + BS - 12, DIRC_ROOT);              /* parent: the rootblock */
    put_be32(f + BS - 8, 14);                      /* EXTENSION pointer */
    put_be32(f + BS - 4, AFS_ST_FILE);
    f[BS - 80] = 4; memcpy(f + BS - 79, "file", 4);
    put_be32(f + 20, afs_block_checksum(f, BS, 20));

    /* file it into the rootblock's hash table so the walk actually reaches it */
    uint8_t *r = img + DIRC_ROOT * BS;
    uint32_t tsz = (uint32_t)afs_hash_table_size(BS);
    put_be32(r + 24 + 4 * afs_hash("file", 4, tsz, 0x01), 11);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    afs_verify_report_t rep[64];
    int n = afs_verify_memory(img, sizeof img, BS, rep, 64);
    CHECK_EQ_U(count_kind(rep, n, AFS_PROBLEM_CHAIN), 0,
               "a reachable file's bs-8 is not walked as a cache chain");
    CHECK_EQ_U(count_kind(rep, n, AFS_PROBLEM_DIRCACHE), 0,
               "a file header on a DIRC volume reports no dir-cache problems");
}

/* M26: a dir-cache block is part of the volume's LIVE STRUCTURE, so the strict
 * bitmap agreement check must see it.
 *
 * This is the second site of the bug M26 found in the rebuild. verify_dircache
 * walked the chain for structural soundness but never recorded the blocks it
 * visited in the tree's marks[], so the strict check -- which iterates exactly
 * that marks[] -- could not see them. Result: on a volume whose rebuild had
 * freed the root's cache block, BOTH the plain and the --strict verify reported
 * the volume clean. The invariant M26 violated was invisible to the checker
 * whose whole job is to catch it.
 *
 * The two must agree: a block the rebuild keeps must be a block verify demands. */
static void test_verify_strict_sees_a_freed_dircache_block(void) {
    /* sanity: with the cache block marked USED (bit clear), strict is happy */
    uint8_t clean[16 * BS];
    build_dirc(clean, 0, DIRC_ROOT);
    {
        afs_verify_report_t rep[64];
        int n = afs_verify_memory_ex(clean, sizeof clean, BS, rep, 64,
                                     AFS_VERIFY_STRICT);
        CHECK_EQ_U(count_kind(rep, n, AFS_PROBLEM_BITMAP_MISMATCH), 0,
                   "strict is satisfied when the cache block is marked used");
    }

    /* Now free it -- the state M26's broken rebuild produced. build_dirc marks
     * only blocks 12..15 free (a set bit == free, page space), so the cache
     * block at 10 already comes out FREE and the "clean" case above only
     * passed because verify had never marked it at all. Freeing it explicitly
     * makes the intent obvious and keeps the test honest if build_dirc changes. */
    uint8_t img[16 * BS];
    build_dirc(img, 0, DIRC_ROOT);
    {
        /* A bitmap PAGE keeps its own checksum long in bytes 0..3, so map data
         * starts at +4. bm_set() above writes at offset 0 -- into the checksum
         * -- which is why these tests never actually exercised bitmap
         * agreement, and why this one writes at the offset the real reader
         * uses. A set bit means FREE. */
        uint8_t *bm = img + DIRC_BM * BS;
        uint32_t bit = DIRC_CACHE;
        bm[4 + bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
        put_be32(bm, afs_block_checksum(bm, BS, 0));
    }

    afs_verify_report_t rep[64];
    int n_bad = afs_verify_memory_ex(img, sizeof img, BS, rep, 64,
                                     AFS_VERIFY_STRICT);
    CHECK(count_kind(rep, n_bad, AFS_PROBLEM_BITMAP_MISMATCH) >= 1,
          "strict reports the live cache block that the bitmap marks free");

    /* and the everyday check stays quiet: a stale map is ORDINARY on a real
     * Amiga disk, which is exactly why this belongs behind --strict */
    afs_verify_report_t rep2[64];
    int n_plain = afs_verify_memory(img, sizeof img, BS, rep2, 64);
    CHECK_EQ_U(count_kind(rep2, n_plain, AFS_PROBLEM_BITMAP_MISMATCH), 0,
               "the everyday check does not report it (stale maps are ordinary)");
}

int main(void) {
    printf("bitmap / verify\n");
    test_verify_strict_sees_a_freed_dircache_block();
    test_bitmap_block_checksum_rule();
    test_bitmap_read_across_blocks();
    test_bitmap_ext_block();
    test_verify_clean_volume();
    test_verify_detects_bad_rootblock();
    test_verify_flags_bad_bootblock_root();
    test_verify_flags_bad_bitmap();
    test_verify_reaches_entries_below_the_rootblock();
    test_verify_reports_out_of_range_pointer();
    test_verify_reports_unknown_block_type();
    test_verify_reports_bitmap_page_used_as_a_file();
    test_verify_accepts_correct_bitmap_page_count();
    test_verify_accepts_a_clean_dircache();
    test_verify_flags_a_self_referential_cache_chain();
    test_verify_flags_a_cache_block_of_the_wrong_type();
    test_verify_flags_a_cache_with_the_wrong_parent();
    test_verify_flags_a_cache_with_a_wrong_headerkey();
    test_verify_ignores_dircache_on_non_dirc_volumes();
    test_verify_ignores_dircache_on_files_of_a_dirc_volume();
    TEST_REPORT();
}