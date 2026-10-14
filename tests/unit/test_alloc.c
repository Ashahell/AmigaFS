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
     * blocks, then stamp the checksum at byte 0. */
    for (i = 0; i < IMG_BLOCKS; i++)
        if (i != 0 && i != 1 && i != IMG_ROOT && i != IMG_BM)
            afs_bitmap_set(bm, (uint32_t)i);
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

int main(void) {
    printf("alloc/write\n");
    test_write_backend_memory();
    test_allocator_first_free();
    test_file_create_write_read();
    test_extension_chain_write();
    test_empty_file_is_header_only();
    test_mkdir_and_nested_create();
    TEST_REPORT();
}
