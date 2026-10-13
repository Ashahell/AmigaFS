/* Unit tests for volume open and DOS-type detection (volume.c).
 *
 * The detection rules that matter, and which this file pins:
 *
 *  - The DOS id is the ASCII 'D','O','S' followed by a flag byte. It may live
 *    on block 0 OR block 1 (AROS checks both); we accept either.
 *  - Supported flag values are DOS\0..DOS\5. DOS\6 and DOS\7 are AmigaOS 3.2
 *    long-filename types and are REFUSED with a named reason -- but note that
 *    DOS\6 is also simply invalid under the FAQ's flag list, so refusal is
 *    correct either way. muFS ('muF') is also refused.
 *  - OFS is the ABSENCE of the FFS bit, so DOS\2 and DOS\4 are OFS. A test of
 *    `flags == 0` would misclassify them (this is the AROS bug, C5).
 *  - DIRC implies INTL for case folding even though bit 1 stays clear.
 *  - The rootblock is located by ONE formula, INT((Reserved + highKey)/2),
 *    and must then validate as T_HEADER/ST_ROOT.
 */
#include "test.h"
#include "amigafs/be.h"
#include "amigafs/checksum.h"
#include "amigafs/hash.h"
#include "amigafs/volume.h"

/* Build an in-memory floppy image with a valid bootblock and rootblock. */
static void build_floppy(uint8_t *img, size_t bytes, uint8_t flags, uint32_t rootblk) {
    memset(img, 0, bytes);
    size_t bs = 512;
    /* bootblock on blocks 0..1 */
    uint8_t *boot = img;
    put_be32(boot + 0, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                       ((uint32_t)'S' << 8) | flags);
    put_be32(boot + 4, afs_bootblock_checksum(boot, 1024));
    put_be32(boot + 8, rootblk);

    /* rootblock */
    uint8_t *r = img + (size_t)rootblk * (size_t)bs;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(bs));
    put_be32(r + 312, 0xFFFFFFFFu);       /* bm_flag valid */
    put_be32(r + 316, rootblk + 1);       /* first bitmap block */
    r[432] = 5;
    memcpy(r + 433, "vol01", 5);
    put_be32(r + 508, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, bs, 20));
}

static void test_open_detects_type(void) {
    const size_t bytes = 1760 * 512;
    uint8_t *img = malloc(bytes);
    CHECK(img != NULL, "image allocated");
    if (!img) return;

    struct { uint8_t flags; int expect_ok; const char *why; } cases[] = {
        { 0, 1, "DOS\\0 OFS" },
        { 1, 1, "DOS\\1 FFS" },
        { 2, 1, "DOS\\2 OFS/INTL" },
        { 3, 1, "DOS\\3 FFS/INTL" },
        { 4, 1, "DOS\\4 OFS/DIRC" },
        { 5, 1, "DOS\\5 FFS/DIRC" },
        { 6, 0, "DOS\\6 refused" },
        { 7, 0, "DOS\\7 refused" },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        build_floppy(img, bytes, cases[i].flags, 880);
        afs_volume_t *v = NULL;
        int rc = afs_volume_open_memory(img, bytes, 512, &v, NULL, sizeof NULL);
        int ok = (rc == 0 && v != NULL);
        if (ok != cases[i].expect_ok) {
            CHECK(0, "%s: open %s but expected %s", cases[i].why,
                  ok ? "succeeded" : "to be refused", cases[i].expect_ok ? "success" : "refusal");
        } else {
            CHECK(1, "%s: open %s as expected", cases[i].why,
                  ok ? "succeeded" : "refused with a reason");
        }
        if (ok) {
            CHECK_EQ_U(afs_volume_flags(v), cases[i].flags, "reported flags");
            /* OFS is the absence of the FFS bit -- the C5 trap */
            int is_ofs = afs_volume_is_ofs(v);
            int want_ofs = !(cases[i].flags & AFS_DOSFS_FFS);
            CHECK_EQ_U((unsigned)is_ofs, (unsigned)want_ofs, "OFS classification");
            afs_volume_close(v);
        }
    }
    free(img);
}

static void test_dos_id_on_block1(void) {
    /* Some media carry the DOS id on block 1 rather than block 0. */
    const size_t bytes = 1760 * 512;
    uint8_t *img = malloc(bytes);
    if (!img) return;
    build_floppy(img, bytes, 0x01, 880);
    /* blank block 0's DOS id, put it on block 1 */
    memset(img, 0, 4);
    put_be32(img + 512, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                         ((uint32_t)'S' << 8) | 0x01);

    afs_volume_t *v = NULL;
    int rc = afs_volume_open_memory(img, bytes, 512, &v, NULL, sizeof NULL);
    CHECK_EQ_U(rc, 0, "volume with DOS id on block 1 opens");
    CHECK(v != NULL, "volume handle returned");
    if (v) {
        CHECK_EQ_U(afs_volume_flags(v), 0x01, "flags read from block 1");
        afs_volume_close(v);
    }
    free(img);
}

static void test_refuse_non_dos(void) {
    const size_t bytes = 1760 * 512;
    uint8_t *img = malloc(bytes);
    if (!img) return;

    /* PFS filesystem */
    build_floppy(img, bytes, 0x01, 880);
    put_be32(img, ((uint32_t)'P' << 24) | ((uint32_t)'F' << 16) | ((uint32_t)'S' << 8));
    afs_volume_t *v = NULL;
    char err[128];
    int rc = afs_volume_open_memory(img, bytes, 512, &v, err, sizeof err);
    CHECK(rc != 0, "PFS volume is refused");
    CHECK(err[0] != '\0', "refusal carries a named reason");
    if (err[0]) printf("         reason: %s\n", err);

    /* muFS */
    build_floppy(img, bytes, 0x01, 880);
    put_be32(img, ((uint32_t)'m' << 24) | ((uint32_t)'u' << 16) | ((uint32_t)'F' << 8));
    rc = afs_volume_open_memory(img, bytes, 512, &v, err, sizeof err);
    CHECK(rc != 0, "muFS volume is refused");
    if (err[0]) printf("         reason: %s\n", err);

    free(img);
}

static void test_rootblock_formula_and_validation(void) {
    const size_t bytes = 1760 * 512;
    uint8_t *img = malloc(bytes);
    if (!img) return;

    /* the formula must place the rootblock at 880 for a DD floppy and the
     * block there must be a valid T_HEADER/ST_ROOT */
    build_floppy(img, bytes, 0x01, 880);
    afs_volume_t *v = NULL;
    char err[128];
    int rc = afs_volume_open_memory(img, bytes, 512, &v, err, sizeof err);
    CHECK_EQ_U(rc, 0, "valid floppy opens");
    if (v) {
        CHECK_EQ_U(afs_volume_rootblock(v), 880u, "rootblock located at 880");
        afs_volume_close(v);
    }

    /* a rootblock that is NOT a T_HEADER/ST_ROOT must be refused, not trusted */
    build_floppy(img, bytes, 0x01, 880);
    put_be32(img + 880 * 512 + 508, AFS_ST_USERDIR);   /* wrong secondary type */
    rc = afs_volume_open_memory(img, bytes, 512, &v, err, sizeof err);
    CHECK(rc != 0, "a mislabelled rootblock is refused");
    if (err[0]) printf("         reason: %s\n", err);

    free(img);
}

static void test_hd_geometry_rootblock(void) {
    /* an HD floppy (3520 blocks) must place the rootblock at 1760 */
    const size_t bytes = 3520 * 512;
    uint8_t *img = malloc(bytes);
    if (!img) return;
    build_floppy(img, bytes, 0x01, 1760);
    afs_volume_t *v = NULL;
    int rc = afs_volume_open_memory(img, bytes, 512, &v, NULL, sizeof NULL);
    CHECK_EQ_U(rc, 0, "HD floppy opens");
    if (v) {
        CHECK_EQ_U(afs_volume_rootblock(v), 1760u, "HD rootblock located at 1760");
        afs_volume_close(v);
    }
    free(img);
}

int main(void) {
    printf("volume\n");
    test_open_detects_type();
    test_dos_id_on_block1();
    test_refuse_non_dos();
    test_rootblock_formula_and_validation();
    test_hd_geometry_rootblock();
    TEST_REPORT();
}