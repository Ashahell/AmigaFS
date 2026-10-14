/* Unit tests for afs_bitmap_rebuild (bitmap.c).
 *
 * A rebuild is a FULL unconditional rescan: mark every block the tree
 * references, free the rest. That is what AmigaDOS does -- bm_Flags in the
 * rootblock is documented as "TRUE if Bitmap on disk is valid", and the
 * historical repair was a boot-time disk validator that write-protected the
 * drive and scanned. ADFlib's adfReconstructBitmap (our pinned oracle, read at
 * src/adf_bitm.c:443) is the same policy: no pre-verification, no consult of
 * bm_Flags, all-or-nothing on a read failure.
 *
 * Because the pass FREES everything it does not mark, the safety of the whole
 * operation rests on one thing: which blocks the free pass is even allowed to
 * touch. ADFlib gets that by construction --
 *
 *     for (i = 2; i < lastBlock - firstBlock; i++) adfSetBlockFree(vol, i);
 *
 * -- the bounds make blocks 0, 1 and the volume-tail mirror structurally
 * unreachable, rather than relying on the walk remembering to mark them. The
 * walk marks the rootblock and the bitmap pages as used (ADFlib lines 572-578),
 * but it never marks the mirror. These tests pin the bounds, because a mutant
 * that lowers the lower bound from 2 to 0 is precisely the change that turns a
 * stale map into destroyed data.
 *
 * Deliberately NOT tested here: the soft-link target. ADFlib marks a link's own
 * block but not its target, so a dangling soft link loses its target on a
 * rebuild. That is AmigaDOS behaviour, matched deliberately and documented in
 * the README rather than "fixed" here.
 */
#include "test.h"
#include "amigafs/be.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/hash.h"
#include "amigafs/volume.h"

#define BS 512
#define NBLK 16

/* Declared here rather than via bitmap.h so this file compiles (and FAILS)
 * against a tree that does not have the function yet. */
typedef struct {
    uint32_t total;
    uint32_t used;
    uint32_t free_blocks;
    uint32_t reclaimed;
} afs_bitmap_rebuild_stats_t;

extern int afs_bitmap_rebuild(afs_volume_t *v,
                              afs_bitmap_rebuild_stats_t *stats,
                              char *errbuf, size_t errlen);

static void bm_set(uint8_t *map, uint32_t blk) {
    map[blk / 32 * 4 + (blk % 32) / 8] |= (uint8_t)(0x80 >> (blk % 8));
}
static int bm_get(const uint8_t *map, uint32_t blk) {
    return (map[blk / 32 * 4 + (blk % 32) / 8] >> (7 - (blk % 8))) & 1;
}

/* A bitmap PAGE is not an assembled map: its first long is the page's own
 * checksum, so map data begins at offset 4. (Every page carries one -- that is
 * the convention afs_bitmap_read and afs_alloc_block both use.) These helpers
 * take the assembled-map view and the tests index the page through +4. */
#define PAGE_DATA(p) ((p) + 4)

/* A minimal but structurally valid 16-block FFS volume, rooted at block 8
 * ((2+15)/2), one bitmap page at block 9. The root is empty, so the walk marks
 * only the rootblock plus the bitmap page -- which is exactly the situation in
 * which the free-pass bounds are the only thing standing between a rebuild and
 * a destroyed bootblock. */
static void build_vol(uint8_t *img) {
    memset(img, 0, NBLK * BS);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x01);                 /* DOS\1 FFS */

    uint8_t *r = img + 8 * BS;
    put_be32(r, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);   /* bitmap valid */
    put_be32(r + BS - 196, 9);            /* bm_pages[0] = block 9 */
    put_be32(r + BS - 96, 0);             /* no bitmap extension */
    put_be32(r + BS - 8, 0);              /* no dircache */
    r[BS - 80] = 3; memcpy(r + BS - 79, "vol", 3);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    /* Old map: every block free except the two the walk must re-mark, and the
     * three the free pass must never reach. Nothing here is trusted -- that is
     * the premise of a rebuild. */
    uint8_t *bm = img + 9 * BS;
    memset(bm, 0, BS);
    for (uint32_t b = 0u; b < NBLK; b++) bm_set(PAGE_DATA(bm), b);
    /* clear = used */
    PAGE_DATA(bm)[0] &= (uint8_t)0x7F;                                  /* block 0 used */
    PAGE_DATA(bm)[(1 % 32) / 8] &= (uint8_t)~(0x80 >> (1 % 8));           /* block 1 used */
    PAGE_DATA(bm)[((NBLK - 1) / 32) * 4 + (((NBLK - 1) % 32) / 8)] &=
        (uint8_t)~(0x80 >> ((NBLK - 1) % 8));                            /* tail used */
    put_be32(bm, afs_block_checksum(bm, BS, 0));
}

static void test_rebuild_never_frees_the_structural_blocks(void) {
    uint8_t img[NBLK * BS];
    build_vol(img);

    afs_volume_t *v = NULL;
    char err[256];
    CHECK(afs_volume_open_memory(img, sizeof img, BS, &v, err, sizeof err) == 0,
          "volume opens (%s)", err);

    CHECK(afs_bitmap_rebuild(v, NULL, err, sizeof err) == 0,
          "rebuild succeeds (%s)", err);

    uint8_t *bm = img + 9 * BS;

    /* The three blocks the free pass must never reach. A mutant lowering the
     * lower bound from 2 to 0 flips all three to free. */
    CHECK_EQ_U(bm_get(PAGE_DATA(bm), 0), 0, "block 0 (bootblock) not freed");
    CHECK_EQ_U(bm_get(PAGE_DATA(bm), 1), 0, "block 1 (rootblock area) not freed");
    CHECK_EQ_U(bm_get(PAGE_DATA(bm), NBLK - 1), 0, "block 15 (volume tail) not freed");

    /* What the walk must mark. */
    CHECK_EQ_U(bm_get(PAGE_DATA(bm), 8), 0, "rootblock marked used");
    CHECK_EQ_U(bm_get(PAGE_DATA(bm), 9), 0, "bitmap page marked used");
}

static void test_rebuild_sets_the_valid_flag(void) {
    uint8_t img[NBLK * BS];
    build_vol(img);
    /* Start from the state the rebuild exists to fix: map flagged suspect.
     * Restamp the checksum afterwards -- editing the block without it is what
     * made this test fail for the wrong reason on its first run. */
    uint8_t *r = img + 8 * BS;
    put_be32(r + BS - 200, 1);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    afs_volume_t *v = NULL;
    char err[256];
    CHECK(afs_volume_open_memory(img, sizeof img, BS, &v, err, sizeof err) == 0,
          "volume opens (%s)", err);
    CHECK(afs_bitmap_rebuild(v, NULL, err, sizeof err) == 0,
          "rebuild succeeds (%s)", err);

    uint8_t *rb = img + 8 * BS;
    CHECK_EQ_U(be32(rb + BS - 200), 0xFFFFFFFFu, "bm_flag set to valid (-1)");
    CHECK(afs_block_valid(rb, BS, 20), "rootblock checksum restamped");
    CHECK(afs_block_valid(img + 9 * BS, BS, 0), "bitmap page checksum restamped");
}

static void test_rebuild_frees_what_the_tree_does_not_reach(void) {
    /* The whole point: a stale map over-reports usage, and reclaiming those
     * blocks is what makes a previously-unwritable volume writable. */
    uint8_t img[NBLK * BS];
    build_vol(img);
    uint8_t *bm = img + 9 * BS;
    /* Mark EVERY block used, as a badly stale map would. Bit 1 = FREE, so a
     * fully-used map is all zeroes -- 0xFF would be a fully-FREE map, which is
     * the opposite of what this test needs and would silently make the reclaim
     * count zero rather than fail loudly. */
    memset(bm + 4, 0x00, BS - 4);
    put_be32(bm, afs_block_checksum(bm, BS, 0));

    afs_volume_t *v = NULL;
    char err[256];
    CHECK(afs_volume_open_memory(img, sizeof img, BS, &v, err, sizeof err) == 0,
          "volume opens (%s)", err);

    afs_bitmap_rebuild_stats_t st;
    CHECK(afs_bitmap_rebuild(v, &st, err, sizeof err) == 0,
          "rebuild succeeds (%s)", err);
    CHECK(st.reclaimed > 0, "stale used blocks were reclaimed (%u)", st.reclaimed);
    CHECK_EQ_U(bm_get(PAGE_DATA(bm), 4), 1, "block 4 is free after rebuild");
    CHECK_EQ_U(bm_get(PAGE_DATA(bm), 8), 0, "rootblock still used after rebuild");
}

static void test_rebuild_is_idempotent(void) {
    uint8_t img[NBLK * BS];
    build_vol(img);
    afs_volume_t *v = NULL;
    char err[256];
    CHECK(afs_volume_open_memory(img, sizeof img, BS, &v, err, sizeof err) == 0,
          "volume opens (%s)", err);
    CHECK(afs_bitmap_rebuild(v, NULL, err, sizeof err) == 0, "first rebuild");

    uint8_t once[BS];
    memcpy(once, img + 9 * BS, BS);

    CHECK(afs_bitmap_rebuild(v, NULL, err, sizeof err) == 0, "second rebuild");
    CHECK(memcmp(once, img + 9 * BS, BS) == 0,
          "rebuilding twice leaves the same page byte-for-byte");
}

static void test_rebuild_refuses_a_short_volume(void) {
    uint8_t img[4 * BS];
    memset(img, 0, sizeof img);
    afs_volume_t *v = NULL;
    char err[256];
    /* A volume too small to hold a rootblock cannot be opened, so this asserts
     * the failure is reported rather than that a specific errno comes back. */
    int rc = afs_volume_open_memory(img, sizeof img, BS, &v, err, sizeof err);
    if (rc == 0) {
        char err2[256];
        err2[0] = '\0';
        CHECK(afs_bitmap_rebuild(v, NULL, err2, sizeof err2) == 0,
              "degenerate volume either refuses with a message (%s) or rebuilds",
              err2);
    } else {
        CHECK(err[0] != '\0', "open refuses with a named reason (%s)", err);
    }
}

int main(void) {
    test_rebuild_never_frees_the_structural_blocks();
    test_rebuild_sets_the_valid_flag();
    test_rebuild_frees_what_the_tree_does_not_reach();
    test_rebuild_is_idempotent();
    test_rebuild_refuses_a_short_volume();
    TEST_REPORT();
}