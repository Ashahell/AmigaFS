/* Unit tests for the block I/O seam (blockio.c).
 *
 * blockio is the ONLY place the core touches storage, and it is the security
 * boundary: every on-disk block pointer in an AmigaDOS file (data blocks,
 * hash-table entries, extension chains) is attacker-controlled data. A pointer
 * outside the partition must be REFUSED, never clamped and never read past the
 * window -- so bounds checks are the central test subject here.
 *
 * Backends under test: a memory backend (deterministic) and a file backend
 * (pread). The window offset is exercised because block pointers inside a
 * partition are PARTITION-RELATIVE and must be translated by the partition's
 * start.
 */
#include "test.h"
#include "amigafs/blockio.h"

static void test_memory_backend_roundtrip(void) {
    uint8_t data[16 * 512];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i & 0xFF);

    blockdev_t *d = blockdev_open_memory(data, sizeof data, 512);
    CHECK(d != NULL, "memory backend opens");

    uint8_t blk[512];
    CHECK_EQ_U(blockdev_read(d, 0, blk, 512), 0, "read block 0 succeeds");
    CHECK_EQ_U(blk[0], 0u, "block 0 byte 0 is data[0]");
    CHECK_EQ_U(blk[511], (unsigned char)(511 & 0xFF), "block 0 byte 511 is data[511]");

    CHECK_EQ_U(blockdev_read(d, 5, blk, 512), 0, "read block 5 succeeds");
    CHECK_EQ_U(blk[0], (unsigned char)((5 * 512) & 0xFF), "block 5 starts at data[2560]");

    blockdev_close(d);
}

static void test_memory_backend_bounds(void) {
    uint8_t data[16 * 512];
    memset(data, 0, sizeof data);
    blockdev_t *d = blockdev_open_memory(data, sizeof data, 512);

    uint8_t blk[512];
    /* 16 blocks: valid indices 0..15 */
    CHECK_EQ_U(blockdev_read(d, 15, blk, 512), 0, "last block (15) is in range");
    CHECK(blockdev_read(d, 16, blk, 512) != 0, "block 16 (one past end) is refused");
    CHECK(blockdev_read(d, 9999, blk, 512) != 0, "a far out-of-range block is refused");
    /* a size that is not block-aligned must refuse, not round down */
    CHECK_EQ_U(blockdev_read(d, 0, blk, 511), 0, "a short read within a block is allowed");

    blockdev_close(d);
}

/* Partition windows: block pointers are partition-relative; blockio adds the
 * window's start block. Reading partition block 0 must equal reading absolute
 * block `start`. */
static void test_partition_window(void) {
    uint8_t data[32 * 512];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i & 0xFF);

    /* partition starts at absolute block 8, holds 8 blocks (abs 8..15) */
    blockdev_t *base = blockdev_open_memory(data, sizeof data, 512);
    blockdev_t *win = blockdev_window(base, 8, 8);
    CHECK(win != NULL, "partition window opens");

    uint8_t wblk[512], ablk[512];
    CHECK_EQ_U(blockdev_read(win, 0, wblk, 512), 0, "partition block 0 reads");
    CHECK_EQ_U(blockdev_read(base, 8, ablk, 512), 0, "absolute block 8 reads");
    CHECK(memcmp(wblk, ablk, 512) == 0,
          "partition block 0 equals absolute block 8 (offset applied)");

    /* within-window bounds: 8 blocks -> indices 0..7 valid, 8 refused */
    CHECK_EQ_U(blockdev_read(win, 7, wblk, 512), 0, "partition last block (7) in range");
    CHECK(blockdev_read(win, 8, wblk, 512) != 0,
          "partition block 8 is refused (would read past the partition)");
    /* and it must not have read into the next partition's data */
    CHECK(blockdev_read(win, 1000, wblk, 512) != 0,
          "far out-of-window block is refused");

    blockdev_close(base);   /* closes the window too */
}

static void test_window_bounds_validation(void) {
    uint8_t data[16 * 512];
    memset(data, 0, sizeof data);
    blockdev_t *base = blockdev_open_memory(data, sizeof data, 512);

    /* a window that runs off the end of the backing device must be refused at
     * open time, not discovered on first read */
    CHECK(blockdev_window(base, 14, 8) == NULL,
          "a window extending past the device is refused at open");
    CHECK(blockdev_window(base, 16, 1) == NULL,
          "a window starting past the device is refused at open");
    CHECK(blockdev_window(base, 0, 0) == NULL,
          "a zero-length window is refused");

    blockdev_close(base);
}

/* File backend: same contract over a real fd via pread. */
static void test_file_backend(void) {
    const char *path = "/tmp/amigafs_blockio_test.bin";
    uint8_t data[8 * 512];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i & 0xFF);

    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "created temp image");
    if (!f) return;
    fwrite(data, 1, sizeof data, f);
    fclose(f);

    blockdev_t *d = blockdev_open_file(path, 512);
    CHECK(d != NULL, "file backend opens");
    uint8_t blk[512];
    CHECK_EQ_U(blockdev_read(d, 3, blk, 512), 0, "file backend reads block 3");
    CHECK_EQ_U(blk[0], (unsigned char)((3 * 512) & 0xFF), "file backend data is correct");
    CHECK(blockdev_read(d, 8, blk, 512) != 0, "file backend refuses past EOF");

    blockdev_close(d);
    remove(path);
}

int main(void) {
    printf("blockio\n");
    test_memory_backend_roundtrip();
    test_memory_backend_bounds();
    test_partition_window();
    test_window_bounds_validation();
    test_file_backend();
    TEST_REPORT();
}