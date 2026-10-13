/* Unit tests for RDB parsing and partition discovery (rdb.c).
 *
 * A hard-disk image carries a Rigid Disk Block (RDB) chain at the front. The
 * RDB describes the physical geometry and points at PARTITION blocks, each of
 * which names a volume and carries a DosEnvVec holding that partition's DOS
 * type, geometry and reserved count. Inside a partition, all AmigaDOS block
 * pointers are PARTITION-RELATIVE, so mounting a partition means handing
 * blockio a window onto it.
 *
 * Field layout (verified against amitools' fs/block/rdb/PartitionBlock.py and
 * against the real a590_WD93028XA_6parts.hdd fixture):
 *   RDB block header: id[4] size hostID next[4] flags   (all big-endian)
 *   PART: next at long 4, flags long 5, drv_name as a BCPL string at long 9,
 *         DosEnvVec longs 32..51 (dos_type at long 48).
 */
#include "test.h"
#include "amigafs/be.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/rdb.h"

/* Build an RDB image with `nparts` partitions, mimicking the real fixture:
 * RDSK at block 0, PART blocks at 1..n, DosEnvVec present in each. */
static size_t build_rdb(uint8_t *img, size_t imgcap, int nparts,
                        const uint32_t *types, const char *const *names,
                        uint32_t *starts, uint32_t *sizes) {
    memset(img, 0, imgcap);
    /* RDSK block 0 */
    uint8_t *r = img;
    memcpy(r, "RDSK", 4);
    put_be32(r + 4, 64);            /* size */
    put_be32(r + 8, 0);             /* checksum (not validated by us) */
    put_be32(r + 12, 7);            /* hostID */
    put_be32(r + 16, 1);            /* next block = first PART */

    uint32_t cur = 2;   /* data area starts at block 2 */
    for (int i = 0; i < nparts; i++) {
        uint8_t *p = img + (1 + i) * 512;
        memcpy(p, "PART", 4);
        put_be32(p + 4, 64);
        put_be32(p + 8, 0);
        put_be32(p + 12, 7);
        put_be32(p + 16, (i + 1 < nparts) ? (uint32_t)(2 + i) : 0xFFFFFFFFu);
        put_be32(p + 20, 0);                        /* flags */
        put_be32(p + 32, 0);                        /* dev_flags */
        /* BCPL name at byte 36 (length) + 37 (data) */
        p[36] = (uint8_t)strlen(names[i]);
        memcpy(p + 37, names[i], strlen(names[i]));
        /* DosEnvVec */
        put_be32(p + 4 * 32, 16);                   /* size (default env) */
        put_be32(p + 4 * 33, 128);                  /* block_size */
        put_be32(p + 4 * 35, 2);                    /* surfaces */
        put_be32(p + 4 * 37, 5);                    /* sectors per track */
        put_be32(p + 4 * 38, 2);                    /* reserved */
        put_be32(p + 4 * 41, (uint32_t)(2 + i));    /* lowCyl */
        put_be32(p + 4 * 42, (uint32_t)(101 + i));  /* highCyl -> 100 cyls */
        put_be32(p + 4 * 48, types[i]);             /* dos_type */

        sizes[i] = 1000;   /* synthetic: 1000 blocks per partition */
        starts[i] = cur;
        cur += sizes[i];
    }
    return (size_t)(cur * 512);   /* total meaningful bytes */
}

static void test_rdb_detected(void) {
    uint8_t img[1024 * 1024];
    uint32_t types[6] = {0x444F5300, 0x444F5302, 0x444F5304,
                         0x444F5301, 0x444F5303, 0x444F5305};
    const char *names[6] = {"OFS", "OFS INTL", "OFS DirCache",
                            "FFS", "FFS INTL", "FFS DirCache"};
    uint32_t starts[6], sizes[6];
    size_t used = build_rdb(img, sizeof img, 6, types, names, starts, sizes);

    afs_rdb_t rdb;
    CHECK_EQ_U(afs_rdb_parse(img, used, 512, &rdb), 0, "an RDB image parses");
    CHECK_EQ_U(rdb.partition_count, 6u, "six partitions discovered");

    /* the DOS types must come back EXACTLY as stored */
    if (rdb.partition_count == 6) {
        int all = 1;
        for (int i = 0; i < 6; i++)
            if (rdb.part[i].dos_type != types[i]) all = 0;
        CHECK(all, "every partition reports its stored DOS type");
        CHECK_EQ_U(rdb.part[0].dos_type, 0x444F5300u, "partition 0 is DOS\\0");
        CHECK_EQ_U(rdb.part[3].dos_type, 0x444F5301u, "partition 3 is DOS\\1");
        CHECK_EQ_S(rdb.part[2].name, "OFS DirCache", "partition 2 name");
        CHECK_EQ_U(rdb.part[1].reserved, 2u, "reserved blocks from DosEnvVec");
    }
}

/* A raw floppy ADF has no RDB; the caller must be told so, not crash. */
static void test_rdb_absent(void) {
    uint8_t img[512];
    memset(img, 0, sizeof img);
    memcpy(img, "DOS\1", 4);         /* a DOS bootblock, not an RDB */
    afs_rdb_t rdb;
    int rc = afs_rdb_parse(img, sizeof img, 512, &rdb);
    CHECK(rc != 0, "a non-RDB image is reported as having no RDB");
}

/* Every partition's block pointers are partition-relative: the window we
 * derive for partition i must begin at its own start block. */
static void test_partition_relative_window(void) {
    uint8_t img[1024 * 1024];
    uint32_t types[2] = {0x444F5300, 0x444F5301};
    const char *names[2] = {"One", "Two"};
    uint32_t starts[2], sizes[2];
    size_t used = build_rdb(img, sizeof img, 2, types, names, starts, sizes);

    afs_rdb_t rdb;
    CHECK_EQ_U(afs_rdb_parse(img, used, 512, &rdb), 0, "two-partition RDB parses");
    if (rdb.partition_count == 2) {
        /* partition 0 begins at the data start; partition 1 begins after it */
        CHECK(rdb.part[0].start_block < rdb.part[1].start_block,
              "partition starts increase");
        CHECK_EQ_U(rdb.part[0].size_blocks, 1000u, "partition 0 size");
        CHECK_EQ_U(rdb.part[1].size_blocks, 1000u, "partition 1 size");
        /* each partition's window must stay within its own range */
        CHECK(rdb.part[1].start_block + rdb.part[1].size_blocks <= used / 512,
              "partition 1 window lies within the image");
    }
}

/* A PART block with an unknown DOS type must be reported, not silently used. */
static void test_unsupported_dos_type_reported(void) {
    uint8_t img[1024 * 1024];
    uint32_t types[2] = {0x444F5300, 0x444F5306};   /* second is DOS\6 */
    const char *names[2] = {"Good", "LongName"};
    uint32_t starts[2], sizes[2];
    size_t used = build_rdb(img, sizeof img, 2, types, names, starts, sizes);

    afs_rdb_t rdb;
    afs_rdb_parse(img, used, 512, &rdb);
    /* the DOS\6 partition is discovered (so we can report it), but callers
     * must refuse to OPEN it. Expose support via a helper. */
    if (rdb.partition_count == 2) {
        CHECK(afs_rdb_dostype_supported(rdb.part[1].dos_type) == 0,
              "a DOS\\6 partition is discovered but flagged unsupported");
        CHECK(afs_rdb_dostype_supported(rdb.part[0].dos_type) == 1,
              "a DOS\\0 partition is flagged supported");
    }
}

int main(void) {
    printf("rdb\n");
    test_rdb_detected();
    test_rdb_absent();
    test_partition_relative_window();
    test_unsupported_dos_type_reported();
    TEST_REPORT();
}