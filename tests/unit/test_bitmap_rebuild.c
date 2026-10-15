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
#include "amigafs/bitmap.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/hash.h"
#include "amigafs/volume.h"

#define BS 512
#define NBLK 16

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

/* --- multi-page coverage -------------------------------------------------
 *
 * NOT a test-first test. The paging code already existed when this was
 * written, so this cannot fail first and does not pretend to. Its value is
 * proven by a MUTANT instead: changing the per-page offset from
 * `i * (bs - 4)` to `i * bs` must turn it red. Assert the reason in the
 * comment, or a future reader will trust the coverage it appears to provide.
 *
 * Why it matters: every OPENABLE fixture needs exactly ONE bitmap page
 * (measured across all 11 — the 2-page one, g1a30c.adf, is the corrupt
 * fixture a rebuild refuses). So without this the second page's offset is
 * never computed by any test in the project, and a volume large enough to
 * need one -- roughly 2048 blocks at 512 bytes -- would be rebuilt with its
 * second page written at the wrong offset.
 *
 * 4200 blocks at bs=512 needs two pages: ceil(ceil(4200/32) / 128) = 2. The
 * rootblock lands at (2+4199)/2 = 2100, and block 4096 is the first block
 * the SECOND page describes. */
#define MP_NBLK 4200
#define MP_ROOT 2100
#define MP_P0   2101
#define MP_P1   2102

static void test_rebuild_writes_both_bitmap_pages(void) {
    size_t imgsz = (size_t)MP_NBLK * BS;
    uint8_t *img = calloc(1, imgsz);
    if (!img) { CHECK(0, "out of memory building the 2-page image"); return; }

    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x01);

    uint8_t *r = img + MP_ROOT * BS;
    put_be32(r, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(r + BS - 200, 0xFFFFFFFFu);
    put_be32(r + BS - 196, MP_P0);
    put_be32(r + BS - 192, MP_P1);
    put_be32(r + BS - 96, 0);
    put_be32(r + BS - 8, 0);
    r[BS - 80] = 3; memcpy(r + BS - 79, "big", 3);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    afs_volume_t *v = NULL;
    char err[256];
    CHECK(afs_volume_open_memory(img, imgsz, BS, &v, err, sizeof err) == 0,
          "2-page volume opens (%s)", err);

    afs_bitmap_rebuild_stats_t st;
    CHECK(afs_bitmap_rebuild(v, &st, err, sizeof err) == 0,
          "rebuild succeeds on a 2-page volume (%s)", err);

    CHECK_EQ_U(st.total, MP_NBLK, "volume reports every block");

    /* Both pages must carry a valid checksum. An unwritten or mis-offset
     * second page is zeros, whose checksum is zero -- which would read as
     * "valid" to a naive check, so the bit assertions below matter too. */
    CHECK(afs_block_valid(img + MP_P0 * BS, BS, 0), "page 0 checksum valid");
    CHECK(afs_block_valid(img + MP_P1 * BS, BS, 0), "page 1 checksum valid");

    /* Read the map back through the PUBLIC reader, so the assertion exercises
     * the same reassembly the allocator and `afs bitmap` use. */
    size_t mapbytes = ((size_t)MP_NBLK + 31u) / 32u * 4u;
    uint8_t *map = calloc(1, mapbytes);
    int pages = afs_bitmap_read(v, map, mapbytes);
    CHECK_EQ_U(pages, 2, "reader reassembles two pages");

    /* Block 4096 is the first block ONLY page 1 can describe. If the write
     * offset is wrong this bit comes back wrong, which is the whole point. */
    CHECK_EQ_U(bm_get(map, 4096), 1, "block 4096 (page 1's first block) is free");
    CHECK_EQ_U(bm_get(map, MP_NBLK - 1), 0, "volume tail still used");
    CHECK_EQ_U(bm_get(map, MP_ROOT), 0, "rootblock still used");
    CHECK_EQ_U(bm_get(map, MP_P0), 0, "bitmap page 0 still used");
    CHECK_EQ_U(bm_get(map, MP_P1), 0, "bitmap page 1 still used");

    /* An empty tree, so the only used blocks are the three the rebuild marks
     * explicitly; every other block in [2, nblocks-2) must read free. */
    uint32_t free_seen = 0;
    for (uint32_t b = 0; b < MP_NBLK; b++) if (bm_get(map, b)) free_seen++;
    CHECK_EQ_U(free_seen, MP_NBLK - 6,
               "exactly the unreferenced blocks read free (tail excluded)");

    free(map);
    afs_volume_close(v);
    free(img);
}

/* The ROOTBLOCK'S OWN dir-cache chain must survive a rebuild.
 *
 * Found by a real corruption, not by inspection: four consecutive 256 KB writes
 * to a DOS\5 volume, the fourth destroyed the root's dir-cache block -- the
 * allocator handed that block out as FILE DATA because the rebuild had marked
 * it free. The write then failed with "dir-cache chain corrupt", a message
 * about a subsystem the user never touched.
 *
 * rb_walk called rb_mark_dircache only for CHILD directories (kind == DIR).
 * The directory it was HANDED -- the rootblock -- never had its own chain
 * marked, so every root-level cache block was freed by the rebuild and later
 * handed to file data.
 *
 * This is a data-loss bug wearing a data-integrity bug's clothes: the volume
 * LOOKED fine and `verify` said clean right up until a write clobbered it. */
static void add_root_dircache(uint8_t *img) {
    uint8_t *r = img + 8 * BS;
    /* cache block 10, hanging off the root's bs-8, chain terminated */
    uint8_t *cb = img + 10 * BS;
    memset(cb, 0, BS);
    put_be32(cb + 0, AFS_T_DIRCACHE);
    put_be32(cb + 4, 10);            /* headerKey: self-pointer */
    put_be32(cb + 8, 8);             /* parent: the rootblock */
    put_be32(cb + 12, 1);            /* recordsNb */
    put_be32(cb + 16, 0);            /* nextDirC: single-block chain */
    put_be32(cb + 20, afs_block_checksum(cb, BS, 20));
    put_be32(r + BS - 8, 10);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
}

/* A volume that ACTUALLY has a dir-cache.
 *
 * M35: these two tests used to call build_vol(), which makes a DOS\1 volume --
 * and DOS\1 has no DIRC bit, so a volume of that shape cannot have a dir-cache
 * at all. They passed only because rb_mark_dircache never asked whether the
 * volume had one: it walked the "chain" unconditionally and marked the blocks
 * the fixture had planted. Give the reader the question and they failed, which
 * is the test having been green for a reason that was not the reason (M28).
 *
 * The fixture was wrong, not the assertion: what these tests are about is
 * PRESERVING a dir-cache chain, so the volume must be one that has one. */
static void build_dirc_vol(uint8_t *img) {
    build_vol(img);
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x05);                 /* DOS\5 DIRC */
    put_be32(img + 8 * BS + 20, afs_block_checksum(img + 8 * BS, BS, 20));
}

static void test_rebuild_keeps_the_rootblocks_dircache(void) {
    uint8_t img[NBLK * BS];
    build_dirc_vol(img);
    add_root_dircache(img);

    afs_volume_t *v = NULL;
    char err[256] = {0};
    CHECK(afs_volume_open_memory(img, sizeof img, BS, &v, err, sizeof err) == 0,
          "volume opens (%s)", err);
    if (!v) return;

    afs_bitmap_rebuild_stats_t st;
    CHECK(afs_bitmap_rebuild(v, &st, err, sizeof err) == 0,
          "rebuild succeeds on a volume whose root has a cache chain (%s)", err);

    uint32_t n = afs_volume_block_count(v);
    size_t mapbytes = ((size_t)n + 31u) / 32u * 4u;
    uint8_t *map = calloc(1, mapbytes);
    CHECK(afs_bitmap_read(v, map, mapbytes) >= 0, "map readable");
    /* bm_get semantics: a CLEAR bit means USED */
    CHECK(!bm_get(map, 10),
          "the root's dir-cache block 10 is marked USED, not freed");
    CHECK(!bm_get(map, 8),  "the rootblock itself is used");
    CHECK(!bm_get(map, 9),  "the bitmap page is used");
    free(map);
    afs_volume_close(v);
}

/* The same for a CHILD directory's chain -- the case that already worked, kept
 * so the fix for the rootblock cannot regress it. */
static void test_rebuild_keeps_a_subdirs_dircache(void) {
    uint8_t img[NBLK * BS];
    build_dirc_vol(img);

    /* a subdirectory entry at block 11 in the root's hash table, whose own
     * cache chain is block 12 */
    uint8_t *d = img + 11 * BS;
    memset(d, 0, BS);
    put_be32(d, AFS_T_HEADER);
    put_be32(d + 12, (uint32_t)afs_hash_table_size(BS));
    put_be32(d + BS - 12, 8);
    put_be32(d + BS - 8, 12);              /* its dircache chain */
    put_be32(d + BS - 4, AFS_ST_USERDIR);
    d[BS - 80] = 3; memcpy(d + BS - 79, "sub", 3);
    put_be32(d + 20, afs_block_checksum(d, BS, 20));

    uint8_t *cb = img + 12 * BS;
    memset(cb, 0, BS);
    put_be32(cb, AFS_T_DIRCACHE);
    put_be32(cb + 4, 12);
    put_be32(cb + 8, 11);
    put_be32(cb + 12, 1);
    put_be32(cb + 16, 0);
    put_be32(cb + 20, afs_block_checksum(cb, BS, 20));

    /* file it into the root's hash table */
    uint8_t *r = img + 8 * BS;
    uint32_t bucket = afs_hash("sub", 3, (uint32_t)afs_hash_table_size(BS), 0x01);
    put_be32(r + 24 + 4 * bucket, 11);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    afs_volume_t *v = NULL;
    char err[256] = {0};
    CHECK(afs_volume_open_memory(img, sizeof img, BS, &v, err, sizeof err) == 0,
          "volume opens (%s)", err);
    if (!v) return;

    afs_bitmap_rebuild_stats_t st;
    CHECK(afs_bitmap_rebuild(v, &st, err, sizeof err) == 0,
          "rebuild succeeds (%s)", err);

    uint32_t n = afs_volume_block_count(v);
    size_t mapbytes = ((size_t)n + 31u) / 32u * 4u;
    uint8_t *map = calloc(1, mapbytes);
    CHECK(afs_bitmap_read(v, map, mapbytes) >= 0, "map readable");
    CHECK(!bm_get(map, 11), "the subdirectory block is used");
    CHECK(!bm_get(map, 12), "the subdirectory's dir-cache block is used");
    free(map);
    afs_volume_close(v);
}

/* --- M35: the rebuild must not read a dir-cache chain that does not exist ----
 *
 * rb_mark_dircache reads the directory block's bs-8 as the first cache block
 * and follows +16 from there. On a volume WITHOUT a dir-cache that field is the
 * EXTENSION pointer and means something else entirely, and it is routinely
 * non-zero: measured on the fixtures, bs-8 is 881 on blank.adf (DOS\0 OFS),
 * testofs.adf (DOS\2 OFS/INTL) and testffs.adf alike. So on every OFS volume
 * the walk leaves the cache context, reads an ordinary block's +16 as a chain
 * link, and dies with "directory block 880 has a corrupt dircache chain" --
 * refusing to rebuild a volume that is perfectly healthy.
 *
 * That matters far beyond tidiness: `bitmap-rebuild` is the REMEDY the M34
 * guard names when it refuses to write, so a rebuild that cannot run on OFS
 * leaves the refusal with nowhere to point. Four of the project's eleven
 * fixtures are OFS.
 *
 * Every other reader already asks. dir.c gates in three places, verify.c gates
 * once; bitmap.c was the only one that did not.
 */
static void test_rebuild_ignores_bs8_when_the_volume_has_no_dircache(void) {
    uint8_t img[NBLK * BS];
    build_vol(img);
    /* DOS\0 OFS -- no DIRC bit, so no dir-cache and bs-8 is the extension */
    put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                  ((uint32_t)'S' << 8) | 0x00);
    /* point bs-8 at an ordinary file block, exactly as a real OFS volume does */
    put_be32(img + 8 * BS + BS - 8, 10);
    /* and block 10 is an ORDINARY file, not a cache block: its +16 is payload,
     * so reading it as nextDirC walks off the end of the volume. This is what
     * a real OFS volume looks like -- my first attempt left block 10 zeroed, so
     * nextDirC read as 0, the chain ended cleanly, and the test PASSED against
     * the bug. A reproduction has to reproduce. */
    put_be32(img + 10 * BS + 0, AFS_T_HEADER);
    put_be32(img + 10 * BS + 16, 9999);      /* garbage "nextDirC" */
    put_be32(img + 10 * BS + 20, afs_block_checksum(img + 10 * BS, BS, 20));
    /* re-stamp: changing the DOS type invalidates the rootblock checksum, and
     * my first version of this test failed with "rootblock checksum invalid" --
     * RED for the wrong reason, which is the check working */
    put_be32(img + 8 * BS + 20, afs_block_checksum(img + 8 * BS, BS, 20));

    afs_volume_t *v = NULL;
    char err[256] = {0};
    CHECK(afs_volume_open_memory(img, sizeof img, BS, &v, err, sizeof err) == 0,
          "an OFS volume opens (%s)", err);
    if (!v) return;
    CHECK(!afs_volume_has_dircache(v), "and is correctly reported as no-dircache");

    afs_bitmap_rebuild_stats_t st;
    int rc = afs_bitmap_rebuild(v, &st, err, sizeof err);
    CHECK(rc == 0,
          "rebuild succeeds on a volume with NO dir-cache (%s)", err);
    CHECK(st.total == NBLK, "  ...and reports a full pass (%u blocks)", st.total);

    /* and the blocks the tree does reach are still marked used, not freed */
    uint32_t n = afs_volume_block_count(v);
    size_t mapbytes = ((size_t)n + 31u) / 32u * 4u;
    uint8_t *map = calloc(1, mapbytes);
    if (afs_bitmap_read(v, map, mapbytes) >= 0) {
        CHECK(!bm_get(map, 8), "the rootblock is marked used");
        CHECK(!bm_get(map, 9), "the bitmap page is marked used");
    } else {
        CHECK(0, "map readable after the rebuild");
    }
    free(map);
    afs_volume_close(v);
}

int main(void) {
    test_rebuild_ignores_bs8_when_the_volume_has_no_dircache();
    test_rebuild_keeps_the_rootblocks_dircache();
    test_rebuild_keeps_a_subdirs_dircache();
    test_rebuild_never_frees_the_structural_blocks();
    test_rebuild_sets_the_valid_flag();
    test_rebuild_frees_what_the_tree_does_not_reach();
    test_rebuild_is_idempotent();
    test_rebuild_refuses_a_short_volume();
    test_rebuild_writes_both_bitmap_pages();
    TEST_REPORT();
}