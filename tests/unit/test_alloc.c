/* Unit tests for the write path (blockio write backend, allocator, file
 * create/write, mkdir).
 *
 * Writes go through the SAME seams as reads: blockdev_write() enforces the
 * identical window bounds as blockdev_read() (refuse, never clamp), and the
 * allocator persists through afs_volume_read_block/afs_bitmap_read so a
 * wrongly-placed allocation shows up as a wrong bit, not a wrong answer.
 * All images here are synthetic and in-memory; oracle diffing of real writes
 * lives in tests/oracle/m13_write.sh.
 */
#include "test.h"
#include "amigafs/alloc.h"
#include "amigafs/be.h"
#include "amigafs/bitmap.h"
#include "amigafs/blockio.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/dir.h"
#include "amigafs/file.h"
#include "amigafs/verify.h"
#include "amigafs/volume.h"

/* A writable synthetic volume: DOS\1 FFS floppy, rootblock 880, one bitmap
 * block at 881 with exactly blocks {0,1,880,881} marked used. */
#define IMG_BLOCKS 1760u
#define IMG_ROOT 880u
#define IMG_BM 881u

static uint8_t *build_writable_img(void) {
    size_t bytes = (size_t)IMG_BLOCKS * 512;
    uint8_t *img = calloc(1, bytes);
    uint8_t *r, *bm;
    size_t i;
    if (!img) return NULL;
    put_be32(img + 0, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                      ((uint32_t)'S' << 8) | 1u);
    put_be32(img + 4, afs_bootblock_checksum(img, 1024));
    r = img + (size_t)IMG_ROOT * 512;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(512));
    put_be32(r + 312, 0xFFFFFFFFu);
    put_be32(r + 316, IMG_BM);
    r[432] = 3;
    memcpy(r + 433, "vol", 3);
    put_be32(r + 508, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, 512, 20));
    bm = img + (size_t)IMG_BM * 512;
    /* calloc left every bit 0 ("used"); free everything except the reserved
     * blocks, then stamp the checksum at byte 0.
     *
     * Build the map SEPARATELY and copy it in at +4. A raw bitmap page carries
     * its own checksum long in bytes 0..3 and map data begins after it, which
     * is where afs_bitmap_read assembles from. afs_bitmap_set() addresses an
     * ASSEMBLED map at offset 0, so calling it on `bm` directly parked blocks
     * 0..31 inside the checksum long and shifted every later block four bytes
     * early -- leaving the rootblock and the bitmap page themselves reading
     * FREE. The sibling helper build_writable_img_free() below already
     * documents this in full; this is the same correction applied here. */
    {
        static uint8_t map[512];
        memset(map, 0, sizeof map);
        for (i = 0; i < IMG_BLOCKS; i++)
            if (i != 0 && i != 1 && i != IMG_ROOT && i != IMG_BM)
                afs_bitmap_set(map, (uint32_t)i);
        memset(bm, 0, 512);
        memcpy(bm + 4, map, 508);          /* the data area starts at 4 */
    }
    put_be32(bm + 0, afs_block_checksum(bm, 512, 0));
    return img;
}

static void test_write_backend_memory(void) {
    uint8_t data[4 * 512];
    uint8_t blk[512], w[512];
    size_t i;
    blockdev_t *d;
    memset(data, 0, sizeof data);
    for (i = 0; i < sizeof w; i++) w[i] = (uint8_t)(i & 0xFF);
    d = blockdev_open_memory(data, sizeof data, 512);
    CHECK(d != NULL, "memory backend opens");
    CHECK_EQ_U(blockdev_write(d, 2, w, 512), 0, "write block 2 succeeds");
    CHECK_EQ_U(blockdev_read(d, 2, blk, 512), 0, "read block 2 succeeds");
    CHECK(memcmp(blk, w, 512) == 0, "round trip is byte-identical");
    CHECK(blockdev_write(d, 4, w, 512) != 0, "one-past-end write is refused");
    CHECK(blockdev_write(d, 99, w, 512) != 0, "far-OOB write is refused");
    CHECK(blockdev_write(d, 0, w, 513) != 0, "oversize write is refused");
    blockdev_close(d);
}

static void test_allocator_first_free(void) {
    uint8_t *img = build_writable_img();
    afs_volume_t *v = NULL;
    uint32_t b1 = 0xFFFFFFFFu, b2 = 0xFFFFFFFFu;
    uint8_t map[512];
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "writable volume opens");
    CHECK_EQ_U(afs_alloc_block(v, &b1, NULL, 0), 0, "first alloc succeeds");
    CHECK_EQ_U(b1, 2u, "first free block is 2 (0,1 boot; root+bitmap used)");
    CHECK_EQ_U(afs_alloc_block(v, &b2, NULL, 0), 0, "second alloc succeeds");
    CHECK_EQ_U(b2, 3u, "allocations advance");
    CHECK_EQ_U(afs_bitmap_read(v, map, sizeof map), 1, "bitmap reread");
    CHECK(!afs_bitmap_test(map, 2), "block 2 now reads used from storage");
    CHECK(!afs_bitmap_test(map, 3), "block 3 now reads used from storage");
    CHECK(afs_bitmap_test(map, 4), "block 4 still free");
    afs_volume_close(v);
    free(img);
}

static void test_file_create_write_read(void) {
    uint8_t *img = build_writable_img();
    afs_volume_t *v = NULL;
    uint32_t hdr = 0;
    uint8_t data[2000];
    void *got = NULL;
    size_t gotlen = 0, i;
    afs_dirent_t e;
    for (i = 0; i < sizeof data; i++) data[i] = (uint8_t)((i * 7 + 1) & 0xFF);
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    CHECK_EQ_U(afs_file_create(v, IMG_ROOT, "big", 3, &hdr, NULL, 0), 0, "create succeeds");
    CHECK(hdr != 0 && hdr != IMG_ROOT, "header is a fresh block");
    CHECK_EQ_U(afs_file_write_all(v, hdr, data, sizeof data, NULL, 0), 0, "write 2000 bytes");
    CHECK_EQ_U(afs_dir_lookup(v, IMG_ROOT, "big", 3, &e), 0, "created file is listed");
    CHECK_EQ_U(afs_file_read_all(v, IMG_ROOT, "big", 3, &got, &gotlen), 0, "reread succeeds");
    CHECK_EQ_U(gotlen, sizeof data, "reread length matches");
    CHECK(got && memcmp(got, data, sizeof data) == 0, "reread bytes identical");
    free(got);
    /* duplicate create must fail by name, like adfFileOpen "w" */
    CHECK(afs_file_create(v, IMG_ROOT, "big", 3, &hdr, NULL, 0) != 0, "duplicate name refused");
    afs_volume_close(v);
    free(img);
}

/* 80 data blocks overflows the header's 72-slot table, forcing exactly one
 * extension block holding 8. Exercises the reversed table in the second
 * table block, the extension chain link, and per-block high_seq. */
static void test_extension_chain_write(void) {
    uint8_t *img = build_writable_img();
    afs_volume_t *v = NULL;
    uint32_t hdr = 0;
    static uint8_t data[80 * 512];
    void *got = NULL;
    size_t gotlen = 0, i;
    uint8_t hb[512];
    for (i = 0; i < sizeof data; i++) data[i] = (uint8_t)((i * 13 + 5) & 0xFF);
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    CHECK_EQ_U(afs_file_create(v, IMG_ROOT, "wide", 4, &hdr, NULL, 0), 0, "create succeeds");
    CHECK_EQ_U(afs_file_write_all(v, hdr, data, sizeof data, NULL, 0), 0, "write 80 blocks");
    CHECK_EQ_U(afs_volume_read_block(v, hdr, hb, 512), 0, "header readable");
    CHECK_EQ_U(be32(hb + 8), 72u, "header high_seq is full at 72");
    {
        uint32_t ext = be32(hb + 512 - 8);
        uint8_t eb[512];
        CHECK(ext != 0, "extension block linked");
        CHECK_EQ_U(afs_volume_read_block(v, ext, eb, 512), 0, "extension readable");
        CHECK_EQ_U(be32(eb + 0), (uint32_t)AFS_T_LIST, "extension primary type");
        CHECK_EQ_U(be32(eb + 8), 8u, "extension high_seq counts its own 8");
        CHECK_EQ_U(be32(eb + 512 - 8), 0u, "extension chain terminates");
    }
    CHECK_EQ_U(afs_file_read_all(v, IMG_ROOT, "wide", 4, &got, &gotlen), 0, "reread succeeds");
    CHECK_EQ_U(gotlen, sizeof data, "reread length matches");
    CHECK(got && memcmp(got, data, sizeof data) == 0, "80 blocks byte-identical");
    free(got);
    afs_volume_close(v);
    free(img);
}

static void test_empty_file_is_header_only(void) {
    uint8_t *img = build_writable_img();
    afs_volume_t *v = NULL;
    uint32_t hdr = 0;
    uint8_t hb[512];
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    /* ADFlib issue #30's trap: an empty file must NOT point at block 0. */
    CHECK_EQ_U(afs_file_create(v, IMG_ROOT, "empty", 5, &hdr, NULL, 0), 0, "create succeeds");
    CHECK_EQ_U(afs_file_write_all(v, hdr, NULL, 0, NULL, 0), 0, "zero-length write succeeds");
    CHECK_EQ_U(afs_volume_read_block(v, hdr, hb, 512), 0, "header readable");
    CHECK_EQ_U(be32(hb + 512 - 188), 0u, "byte_size is 0");
    CHECK_EQ_U(be32(hb + 8), 0u, "high_seq is 0");
    CHECK_EQ_U(be32(hb + 24 + 4 * 71), 0u, "first data slot is 0, not block 0");
    afs_volume_close(v);
    free(img);
}

/* Overwrite = truncate + rewrite (fopen "w" semantics): grow, shrink, and
 * shrink-to-empty must each leave exact content, exact tables, and exact
 * bitmap accounting. A second write to live data used to be refused; these
 * assert the refusal is gone AND the result is right. */
static void test_overwrite_grow_shrink_empty(void) {
    uint8_t *img = build_writable_img();
    afs_volume_t *v = NULL;
    uint32_t hdr = 0;
    static uint8_t big[3000], small[10], mid[1000];
    void *got = NULL;
    size_t gotlen = 0, i;
    uint8_t hb[512];
    for (i = 0; i < sizeof big; i++) big[i] = (uint8_t)((i * 3 + 7) & 0xFF);
    for (i = 0; i < sizeof small; i++) small[i] = (uint8_t)(0x40 + i);
    for (i = 0; i < sizeof mid; i++) mid[i] = (uint8_t)((i * 11 + 3) & 0xFF);
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    CHECK_EQ_U(afs_file_create(v, IMG_ROOT, "rw", 2, &hdr, NULL, 0), 0, "create succeeds");
    /* grow 10 -> 3000 across the same header, then shrink, then empty */
    CHECK_EQ_U(afs_file_write_all(v, hdr, small, sizeof small, NULL, 0), 0, "write 10");
    CHECK_EQ_U(afs_file_write_all(v, hdr, big, sizeof big, NULL, 0), 0, "overwrite grows 10 -> 3000");
    CHECK_EQ_U(afs_file_read_all(v, IMG_ROOT, "rw", 2, &got, &gotlen), 0, "reread succeeds");
    CHECK_EQ_U(gotlen, sizeof big, "grown length matches");
    CHECK(got && memcmp(got, big, sizeof big) == 0, "grown bytes identical");
    free(got); got = NULL;
    /* shrink 3000 -> 1000: content exact, surplus blocks freed */
    CHECK_EQ_U(afs_file_write_all(v, hdr, mid, sizeof mid, NULL, 0), 0, "overwrite shrinks 3000 -> 1000");
    CHECK_EQ_U(afs_file_read_all(v, IMG_ROOT, "rw", 2, &got, &gotlen), 0, "reread succeeds");
    CHECK_EQ_U(gotlen, sizeof mid, "shrunk length matches");
    CHECK(got && memcmp(got, mid, sizeof mid) == 0, "shrunk bytes identical");
    free(got); got = NULL;
    /* shrink to empty: the M13a header-only shape must come back exactly */
    CHECK_EQ_U(afs_file_write_all(v, hdr, NULL, 0, NULL, 0), 0, "overwrite truncates to empty");
    CHECK_EQ_U(afs_volume_read_block(v, hdr, hb, 512), 0, "header readable");
    CHECK_EQ_U(be32(hb + 512 - 188), 0u, "byte_size back to 0");
    CHECK_EQ_U(be32(hb + 8), 0u, "high_seq back to 0");
    CHECK_EQ_U(be32(hb + 512 - 8), 0u, "extension unlinked");
    CHECK_EQ_U(be32(hb + 16), 0u, "first_data back to 0");
    /* freed blocks are reusable: the next allocation must recycle storage */
    {
        uint32_t re = 0xFFFFFFFFu;
        CHECK_EQ_U(afs_alloc_block(v, &re, NULL, 0), 0, "alloc after free succeeds");
        CHECK(re != 0 && re != IMG_ROOT && re != IMG_BM, "recycled block is ordinary storage");
    }
    afs_volume_close(v);
    free(img);
}

static void test_mkdir_and_nested_create(void) {
    uint8_t *img = build_writable_img();
    afs_volume_t *v = NULL;
    uint32_t sub = 0, hdr = 0;
    afs_dirent_t e;
    uint8_t one = 0xAB;
    void *got = NULL;
    size_t gotlen = 0;
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    CHECK_EQ_U(afs_dir_mkdir(v, IMG_ROOT, "sub", 3, &sub, NULL, 0), 0, "mkdir succeeds");
    CHECK_EQ_U(afs_dir_lookup(v, IMG_ROOT, "sub", 3, &e), 0, "new dir is listed");
    CHECK_EQ_U((unsigned)e.kind, (unsigned)AFS_BLOCK_DIR, "listed as a directory");
    CHECK_EQ_U(afs_file_create(v, sub, "f", 1, &hdr, NULL, 0), 0, "create inside works");
    CHECK_EQ_U(afs_file_write_all(v, hdr, &one, 1, NULL, 0), 0, "write 1 byte");
    CHECK_EQ_U(afs_file_read_all(v, sub, "f", 1, &got, &gotlen), 0, "reread succeeds");
    CHECK_EQ_U(gotlen, 1u, "length 1");
    CHECK(got && ((uint8_t *)got)[0] == 0xAB, "byte identical");
    free(got);
    CHECK(afs_dir_mkdir(v, IMG_ROOT, "sub", 3, &sub, NULL, 0) != 0, "duplicate dir refused");
    afs_volume_close(v);
    free(img);
}

/* Delete + rename (M14 RED): entry unlinked from its chain, storage freed,
 * cache record dropped, bitmap exact. Non-empty dirs, links, and moves into
 * the moved subtree refuse with named reasons (ADFlib refuses the same set:
 * adfRemoveEntry rejects non-empty dirs and non-file/dir types). */
static void test_delete_file_frees_blocks(void) {
    uint8_t *img = build_writable_img();
    afs_volume_t *v = NULL;
    uint32_t hdr = 0;
    static uint8_t data[3000];
    size_t i;
    afs_dirent_t e;
    uint8_t map[512];
    uint32_t used_before = 0, used_after = 0, b;
    for (i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i & 0xFF);
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    CHECK_EQ_U(afs_bitmap_read(v, map, sizeof map), 1, "bitmap reads");
    for (b = 0; b < IMG_BLOCKS; b++)
        if (!afs_bitmap_test(map, b)) used_before++;
    CHECK_EQ_U(afs_file_create(v, IMG_ROOT, "gone", 4, &hdr, NULL, 0), 0, "create succeeds");
    CHECK_EQ_U(afs_file_write_all(v, hdr, data, sizeof data, NULL, 0), 0, "write 3000 bytes");
    CHECK_EQ_U(afs_file_delete(v, IMG_ROOT, "gone", 4, NULL, 0), 0, "delete succeeds");
    CHECK(afs_dir_lookup(v, IMG_ROOT, "gone", 4, &e) != 0, "deleted name no longer resolves");
    CHECK_EQ_U(afs_bitmap_read(v, map, sizeof map), 1, "bitmap rereads");
    for (b = 0; b < IMG_BLOCKS; b++)
        if (!afs_bitmap_test(map, b)) used_after++;
    CHECK_EQ_U(used_after, used_before, "bitmap back to baseline: header+6 data freed");
    /* the freed header block itself is reusable storage again */
    {
        uint32_t re = 0;
        CHECK_EQ_U(afs_file_create(v, IMG_ROOT, "back", 4, &hdr, NULL, 0), 0, "recreate succeeds");
        CHECK_EQ_U(afs_alloc_block(v, &re, NULL, 0), 0, "alloc succeeds");
        (void)re;
    }
    afs_volume_close(v);
    free(img);
}

static void test_rmdir_and_refusals(void) {
    uint8_t *img = build_writable_img();
    afs_volume_t *v = NULL;
    uint32_t sub = 0, hdr = 0;
    uint8_t one = 0x55;
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    CHECK_EQ_U(afs_dir_mkdir(v, IMG_ROOT, "dd", 2, &sub, NULL, 0), 0, "mkdir succeeds");
    CHECK_EQ_U(afs_file_create(v, sub, "f", 1, &hdr, NULL, 0), 0, "create inside succeeds");
    CHECK_EQ_U(afs_file_write_all(v, hdr, &one, 1, NULL, 0), 0, "write 1 byte");
    CHECK(afs_dir_rmdir(v, IMG_ROOT, "dd", 2, NULL, 0) != 0, "non-empty dir refused");
    CHECK_EQ_U(afs_file_delete(v, sub, "f", 1, NULL, 0), 0, "empty it first");
    CHECK_EQ_U(afs_dir_rmdir(v, IMG_ROOT, "dd", 2, NULL, 0), 0, "empty dir removed");
    {
        afs_dirent_t e;
        CHECK(afs_dir_lookup(v, IMG_ROOT, "dd", 2, &e) != 0, "removed dir no longer resolves");
    }
    CHECK(afs_dir_rmdir(v, IMG_ROOT, "dd", 2, NULL, 0) != 0, "double rmdir refused");
    afs_volume_close(v);
    free(img);
}

static void test_rename_within_and_across(void) {
    uint8_t *img = build_writable_img();
    afs_volume_t *v = NULL;
    uint32_t hdr = 0, d1 = 0, d2 = 0;
    uint8_t payload[100];
    void *got = NULL;
    size_t gotlen = 0, i;
    afs_dirent_t e;
    for (i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(0xE0 - i);
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    CHECK_EQ_U(afs_dir_mkdir(v, IMG_ROOT, "d1", 2, &d1, NULL, 0), 0, "mkdir d1");
    CHECK_EQ_U(afs_dir_mkdir(v, IMG_ROOT, "d2", 2, &d2, NULL, 0), 0, "mkdir d2");
    CHECK_EQ_U(afs_file_create(v, d1, "a", 1, &hdr, NULL, 0), 0, "create a in d1");
    CHECK_EQ_U(afs_file_write_all(v, hdr, payload, sizeof payload, NULL, 0), 0, "write payload");
    /* same-dir rename: content follows the name, old name dies */
    CHECK_EQ_U(afs_dir_rename(v, d1, "a", 1, d1, "b", 1, NULL, 0), 0, "rename a->b succeeds");
    CHECK(afs_dir_lookup(v, d1, "a", 1, &e) != 0, "old name no longer resolves");
    CHECK_EQ_U(afs_dir_lookup(v, d1, "b", 1, &e), 0, "new name resolves");
    CHECK_EQ_U(afs_file_read_all(v, d1, "b", 1, &got, &gotlen), 0, "reread succeeds");
    CHECK_EQ_U(gotlen, sizeof payload, "length intact across rename");
    CHECK(got && memcmp(got, payload, sizeof payload) == 0, "bytes intact across rename");
    free(got); got = NULL;
    /* move across directories */
    CHECK_EQ_U(afs_dir_rename(v, d1, "b", 1, d2, "c", 1, NULL, 0), 0, "move d1/b -> d2/c succeeds");
    CHECK(afs_dir_lookup(v, d1, "b", 1, &e) != 0, "source gone after move");
    CHECK_EQ_U(afs_file_read_all(v, d2, "c", 1, &got, &gotlen), 0, "reread at destination");
    CHECK(got && memcmp(got, payload, sizeof payload) == 0, "bytes intact across move");
    free(got); got = NULL;
    /* onto an existing name: refused, victim untouched */
    CHECK(afs_dir_rename(v, d2, "c", 1, IMG_ROOT, "d1", 2, NULL, 0) != 0, "rename onto existing refused");
    CHECK_EQ_U(afs_dir_lookup(v, d2, "c", 1, &e), 0, "victim still resolves");
    /* a directory moved into its own subtree would cycle: refused, both the
     * direct self-move and the deeper descendant case (which needs the
     * walk-up, not just an equality check) */
    {
        uint32_t sub = 0;
        CHECK_EQ_U(afs_dir_mkdir(v, d1, "sub", 3, &sub, NULL, 0), 0, "mkdir d1/sub");
        CHECK(afs_dir_rename(v, IMG_ROOT, "d1", 2, d1, "d1", 2, NULL, 0) != 0, "self-move refused");
        CHECK(afs_dir_rename(v, IMG_ROOT, "d1", 2, sub, "moved", 5, NULL, 0) != 0, "move into own subtree refused");
        CHECK_EQ_U(afs_dir_lookup(v, IMG_ROOT, "d1", 2, &e), 0, "d1 still at root after refused moves");
    }
    afs_volume_close(v);
    free(img);
}

/* Partial-range writes (M15 RED): overwrite a span without touching the rest,
 * append at exactly EOF, and extend past EOF with a zero-filled gap (POSIX).
 * Sizes, tables and bitmap must all come out exact -- a range write that
 * rewrites the whole file would still pass content checks, so the bitmap
 * delta is asserted to prove only the needed blocks moved. */
static void test_write_range_span(void) {
    uint8_t *img = build_writable_img();
    afs_volume_t *v = NULL;
    uint32_t hdr = 0;
    static uint8_t base[2000], patch[100], tail[48];
    void *got = NULL;
    size_t gotlen = 0, i;
    uint8_t map[512];
    uint32_t used_before = 0, used_after = 0, b;
    for (i = 0; i < sizeof base; i++) base[i] = (uint8_t)(i & 0xFF);
    for (i = 0; i < sizeof patch; i++) patch[i] = (uint8_t)(0xA0 + i);
    for (i = 0; i < sizeof tail; i++) tail[i] = (uint8_t)(0x50 + i);
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    CHECK_EQ_U(afs_file_create(v, IMG_ROOT, "rg", 2, &hdr, NULL, 0), 0, "create succeeds");
    CHECK_EQ_U(afs_file_write_all(v, hdr, base, sizeof base, NULL, 0), 0, "write 2000");
    CHECK_EQ_U(afs_bitmap_read(v, map, sizeof map), 1, "bitmap reads");
    for (b = 0; b < IMG_BLOCKS; b++)
        if (!afs_bitmap_test(map, b)) used_before++;
    /* mid-span overwrite crossing a block boundary (600..700 spans 512+88) */
    CHECK_EQ_U(afs_file_write_range(v, hdr, patch, sizeof patch, 600, NULL, 0), 0, "range write succeeds");
    CHECK_EQ_U(afs_file_read_all(v, IMG_ROOT, "rg", 2, &got, &gotlen), 0, "reread succeeds");
    CHECK_EQ_U(gotlen, sizeof base, "length unchanged by span write");
    CHECK(got && memcmp(got, base, 600) == 0, "prefix intact");
    CHECK(got && memcmp((uint8_t *)got + 600, patch, sizeof patch) == 0, "span replaced");
    CHECK(got && memcmp((uint8_t *)got + 700, base + 700, sizeof base - 700) == 0, "suffix intact");
    free(got); got = NULL;
    /* append at exactly EOF grows by the tail */
    CHECK_EQ_U(afs_file_write_range(v, hdr, tail, sizeof tail, sizeof base, NULL, 0), 0, "append at EOF");
    CHECK_EQ_U(afs_file_read_all(v, IMG_ROOT, "rg", 2, &got, &gotlen), 0, "reread succeeds");
    CHECK_EQ_U(gotlen, sizeof base + sizeof tail, "grown length matches");
    CHECK(got && memcmp((uint8_t *)got + sizeof base, tail, sizeof tail) == 0, "tail bytes identical");
    free(got); got = NULL;
    CHECK_EQ_U(afs_bitmap_read(v, map, sizeof map), 1, "bitmap rereads");
    for (b = 0; b < IMG_BLOCKS; b++)
        if (!afs_bitmap_test(map, b)) used_after++;
    CHECK_EQ_U(used_after, used_before, "span+append moved no extra blocks (4 data total)");
    afs_volume_close(v);
    free(img);
}

static void test_write_range_zero_gap(void) {
    uint8_t *img = build_writable_img();
    afs_volume_t *v = NULL;
    uint32_t hdr = 0;
    static uint8_t one[1] = { 0x7E };
    void *got = NULL;
    size_t gotlen = 0, i;
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    CHECK_EQ_U(afs_file_create(v, IMG_ROOT, "gz", 2, &hdr, NULL, 0), 0, "create succeeds");
    CHECK_EQ_U(afs_file_write_all(v, hdr, one, sizeof one, NULL, 0), 0, "write 1 byte");
    /* write past EOF: the gap must read back as zeros (POSIX hole semantics) */
    CHECK_EQ_U(afs_file_write_range(v, hdr, one, sizeof one, 1000, NULL, 0), 0, "gap write succeeds");
    CHECK_EQ_U(afs_file_read_all(v, IMG_ROOT, "gz", 2, &got, &gotlen), 0, "reread succeeds");
    CHECK_EQ_U(gotlen, 1001u, "size spans the gap");
    if (got && gotlen == 1001) {
        int zeros = 1;
        CHECK(((uint8_t *)got)[0] == 0x7E, "first byte intact");
        CHECK(((uint8_t *)got)[1000] == 0x7E, "written byte landed");
        for (i = 1; i < 1000; i++)
            if (((uint8_t *)got)[i] != 0) { zeros = 0; break; }
        CHECK(zeros, "gap reads back zero-filled");
    } else {
        CHECK(0, "reread shape wrong");
    }
    free(got);
    afs_volume_close(v);
    free(img);
}

/* A volume with only `free_blocks` free blocks -- enough to provoke the
 * out-of-space path deterministically, which no fixed scenario did until the
 * write fuzz (M16) found a leak there under LeakSanitizer.
 *
 * GOTCHA worth stating, because the first version of this test got it wrong
 * and "proved" a volume full of free blocks was full: afs_bitmap_set/clear
 * index an ASSEMBLED map (what afs_bitmap_read produces -- the page data
 * areas, concatenated, with each page's checksum long stripped), not a raw
 * storage block. A block's bit sits at byte (blk/32)*4 + (blk%32)/8 of THAT
 * buffer. Passing a raw page instead parks blocks 0..31 inside the page's own
 * checksum long, which the allocator never reads. Hence: build the map
 * separately, then copy it into the page at offset 4. */
static uint8_t *build_writable_img_free(unsigned free_blocks) {
    uint8_t *img = build_writable_img();
    static uint8_t map[512];
    uint8_t *bm;
    unsigned avail = 0;
    if (!img) return NULL;
    memset(map, 0, sizeof map);            /* 0 = allocated */
    for (unsigned i = 0; i < IMG_BLOCKS; i++) afs_bitmap_set(map, i);
    /* The structural blocks MUST be marked allocated. The first loop marks every
     * block free, and the second loop below `continue`s past exactly these two,
     * so without this they stayed FREE -- which is not merely untidy, it is the
     * defect M34's guard exists to catch: block 880 is the rootblock and 881 is
     * the bitmap page, both permanently referenced, and a map that calls them
     * free would hand them to the first file created.
     *
     * M29 again, and the third time in this file: a fixture sharing a mistake
     * with the code, producing a suite that agreed with itself. The sibling
     * build_writable_img() above has the correct inverted form -- it only marks
     * the NON-structural blocks free. This one built the map the other way round
     * and forgot to put the structural blocks back.
     *
     * It passed every one of its 171 checks until M34's guard asked the
     * question, which is the whole argument for the guard. */
    afs_bitmap_clear(map, 0);
    afs_bitmap_clear(map, 1);
    afs_bitmap_clear(map, IMG_ROOT);
    afs_bitmap_clear(map, IMG_BM);
    for (unsigned i = 2; i < IMG_BLOCKS; i++) {
        if (i == IMG_ROOT || i == IMG_BM) continue;
        avail++;
        if (avail > free_blocks) afs_bitmap_clear(map, i);
    }
    bm = img + (size_t)IMG_BM * 512;
    memset(bm, 0, 512);
    memcpy(bm + 4, map, 508);              /* the data area starts at 4 */
    put_be32(bm + 0, afs_block_checksum(bm, 512, 0));
    return img;
}

/* A write that cannot fit must be refused with the previous content intact --
 * deficit blocks are allocated BEFORE anything is mutated. This is the
 * user-visible half of what the fuzz found; the leak it found needs the
 * sanitizer, and lives in the M16 gate. */
static void test_enospc_leaves_content_intact(void) {
    static uint8_t small[100], huge[8000];
    uint8_t *img;
    afs_volume_t *v = NULL;
    uint32_t hdr = 0;
    void *got = NULL;
    size_t gotlen = 0;
    size_t i;
    for (i = 0; i < sizeof small; i++) small[i] = (uint8_t)(i ^ 0x3C);
    for (i = 0; i < sizeof huge; i++) huge[i] = (uint8_t)(0xE1 - (i & 0x7F));
    /* room for the header plus a couple of data blocks: enough for `small`,
     * nowhere near enough for `huge` */
    img = build_writable_img_free(3);
    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    CHECK_EQ_U(afs_file_create(v, IMG_ROOT, "tight", 5, &hdr, NULL, 0), 0,
               "create succeeds");
    char err[256] = {0};
    CHECK_EQ_U(afs_file_write_all(v, hdr, small, sizeof small, err, sizeof err), 0,
               "100 bytes fit");
    CHECK(afs_file_write_all(v, hdr, huge, sizeof huge, NULL, 0) != 0,
           "8000 bytes refused (out of space)");
    CHECK_EQ_U(afs_file_read_all(v, IMG_ROOT, "tight", 5, &got, &gotlen), 0,
               "reread succeeds");
    CHECK_EQ_U(gotlen, sizeof small, "length unchanged by the refused write");
    CHECK(got && memcmp(got, small, sizeof small) == 0,
           "every byte of the previous content intact");
    free(got);
    CHECK_EQ_U(afs_file_write_all(v, hdr, NULL, 0, NULL, 0), 0,
               "truncate to empty still fits");
    CHECK_EQ_U(afs_file_read_all(v, IMG_ROOT, "tight", 5, &got, &gotlen), 0,
               "reread after truncate");
    CHECK_EQ_U(gotlen, 0, "emptied");
    free(got);
    afs_volume_close(v);
    free(img);
}

/* The volume every allocation test runs against must have a map the LIBRARY
 * can actually read. This is the fourth instance of the same defect (after
 * verify.c's page reader, test_verify.c's bm_set(), and write_fuzz.c's
 * fixture): `afs_bitmap_set` addresses an ASSEMBLED map at offset 0, while a
 * raw bitmap PAGE carries its own checksum long in bytes 0..3, so page data
 * begins at +4. Using the setter on a page parks blocks 0..31 inside the
 * checksum and every other block 4 bytes early.
 *
 * It went unnoticed because build_writable_img() is the base image for every
 * test in this file, so all 161 checks agreed with each other about map state
 * that no reader would ever see. The sibling helper build_writable_img_free()
 * already documents the gotcha in full and does it correctly -- it builds the
 * map separately and copies it into the page at +4. This test is that
 * observation applied to the base image.
 *
 * Note the allocator reads the SAME wrong region, so allocations still
 * succeeded; the suite was testing self-consistent nonsense. */
static void test_the_base_volume_has_a_readable_map(void) {
    uint8_t *img = build_writable_img();
    CHECK(img != NULL, "the base volume builds");
    if (!img) return;

    afs_volume_t *v = NULL;
    char err[256] = {0};
    CHECK(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512, &v,
                                 err, sizeof err) == 0,
          "the base volume opens (%s)", err);
    if (!v) { free(img); return; }

    /* Strict is the only mode that checks the tree against the map -- which is
     * exactly the question ("is every live block marked used?") this test is
     * asking. */
    afs_verify_report_t rep[16];
    int n = afs_verify_memory_ex(img, (size_t)IMG_BLOCKS * 512, 512, rep, 16,
                                 AFS_VERIFY_STRICT);
    for (int i = 0; i < n && i < 3; i++)
        printf("         [k%d blk %u] %s\n", (int)rep[i].kind, rep[i].block,
               rep[i].detail);
    CHECK_EQ_U(n, 0,
               "every block the tree references is marked used in the map");

    /* And the map must say what the comment above it claims: all but
     * {0, 1, 880, 881} free. */
    uint32_t nb = afs_volume_block_count(v);
    size_t mb = ((size_t)nb + 31u) / 32u * 4u;
    uint8_t *map = calloc(1, mb);
    afs_bitmap_read(v, map, mb);
    CHECK(!afs_bitmap_test(map, 0),  "block 0 is used");
    CHECK(!afs_bitmap_test(map, 1),  "block 1 is used");
    CHECK(!afs_bitmap_test(map, IMG_ROOT), "the rootblock is used");
    CHECK(!afs_bitmap_test(map, IMG_BM),   "the bitmap page is used");
    CHECK(afs_bitmap_test(map, 2),  "block 2 is free");
    CHECK(afs_bitmap_test(map, 100), "block 100 is free");
    CHECK(afs_bitmap_test(map, nb - 1), "the last block is free");
    free(map);
    afs_volume_close(v);
    free(img);
}

/* M34: the allocator must REFUSE when the map marks free a block the tree
 * references, because that is the state in which it hands out a block already in
 * use -- silent data loss, not a crash.
 *
 * The volume is built and a real file written, so the tree genuinely references
 * the file's header. Marking that header FREE in the map is the smallest
 * possible disagreement, and it is exactly the state every fixture in the repo
 * is in (their maps are all stale), which is why the write gates need a rebuild
 * line rather than a workaround.
 *
 * The second half is the part that matters most: the refusal names a remedy, and
 * the test RUNS the remedy and shows the refusal clearing. A refusal naming a
 * missing remedy is worse than no refusal, because it sends the user to do a
 * thing they cannot do -- which is exactly how bitmap-rebuild used to behave on
 * g1a30c (part 11). */
static void test_alloc_refuses_when_the_map_disagrees_with_the_tree(void) {
    uint8_t *img = build_writable_img_free(IMG_BLOCKS - 8);
    afs_volume_t *v = NULL;
    char err[256] = {0};
    uint32_t hdr = 0, got = 0xFFFFFFFFu;
    uint8_t map[512];

    CHECK(img != NULL, "image allocated");
    CHECK_EQ_U(afs_volume_open_memory(img, (size_t)IMG_BLOCKS * 512, 512,
                                      &v, NULL, 0), 0, "volume opens");
    if (!v) return;

    CHECK_EQ_U(afs_file_create(v, IMG_ROOT, "big", 3, &hdr, NULL, 0), 0,
               "create succeeds");
    uint8_t body[900];
    for (size_t i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i * 5u + 3u);
    CHECK_EQ_U(afs_file_write_all(v, hdr, body, sizeof body, NULL, 0), 0,
               "write succeeds while the map still agrees");

    /* now corrupt the map: the file's HEADER is referenced by the tree, so
     * marking it FREE is a disagreement */
    memset(map, 0, sizeof map);
    for (uint32_t i = 0; i < IMG_BLOCKS; i++)
        if (i != 0 && i != 1 && i != IMG_ROOT && i != IMG_BM && i != hdr)
            afs_bitmap_set(map, i);
    uint8_t *bm = img + (size_t)IMG_BM * 512;
    memset(bm, 0, 512);
    memcpy(bm + 4, map, 508);
    put_be32(bm + 0, afs_block_checksum(bm, 512, 0));

    CHECK(afs_alloc_block(v, &got, err, sizeof err) != 0,
          "allocation REFUSES when the map marks a referenced block free");
    CHECK(strstr(err, "bitmap-rebuild") != NULL,
          "  ...and the message names the remedy (%s)", err);
    CHECK_EQ_U(got, 0xFFFFFFFFu, "  ...and hands back no block");

    /* and the named remedy actually clears the refusal */
    afs_bitmap_rebuild_stats_t st;
    CHECK_EQ_U(afs_bitmap_rebuild(v, &st, err, sizeof err), 0,
               "the named remedy runs clean (%s)", err);
    got = 0xFFFFFFFFu;
    CHECK_EQ_U(afs_alloc_block(v, &got, err, sizeof err), 0,
               "after the remedy, allocation succeeds again (%s)", err);
    CHECK(got != hdr, "  ...and does not hand back the file's header");
}

int main(void) {
    printf("alloc/write\n");
    test_the_base_volume_has_a_readable_map();
    test_write_backend_memory();
    test_allocator_first_free();
    test_file_create_write_read();
    test_extension_chain_write();
    test_overwrite_grow_shrink_empty();
    test_empty_file_is_header_only();
    test_mkdir_and_nested_create();
    test_delete_file_frees_blocks();
    test_rmdir_and_refusals();
    test_rename_within_and_across();
    test_alloc_refuses_when_the_map_disagrees_with_the_tree();
    test_write_range_span();
    test_write_range_zero_gap();
    test_enospc_leaves_content_intact();
    TEST_REPORT();
}
