/* Unit tests for on-disk block parsing (blocktypes.c) and volume open
 * (volume.c).
 *
 * Every offset asserted here was verified against REAL fixture bytes before
 * being written down -- see tests/oracle/m2_integration.c, which re-checks the
 * same facts through the oracles. The blocks are embedded so the unit test
 * stays hermetic.
 *
 * A block type is a primary type (T_*) plus a secondary type (ST_*). The
 * primary type alone does not identify a block; T_HEADER covers rootblock,
 * file header, directory block and dir-cache block, which are told apart by
 * their secondary type. Dir-cache blocks instead use the distinct primary
 * type 33.
 */
#include "test.h"
#include "amigafs/be.h"
#include "amigafs/blocktypes.h"

/* --- constants must match the spec exactly --- */
static void test_type_constants(void) {
    CHECK_EQ_U(AFS_T_HEADER, 2u, "T_HEADER");
    CHECK_EQ_U(AFS_T_DATA, 8u, "T_DATA");
    CHECK_EQ_U(AFS_T_LIST, 16u, "T_LIST (file extension)");
    CHECK_EQ_U(AFS_T_DIRCACHE, 33u, "T_DIRCACHE");
    CHECK_EQ_U(AFS_ST_ROOT, 1u, "ST_ROOT");
    CHECK_EQ_U(AFS_ST_USERDIR, 2u, "ST_USERDIR");
    CHECK_EQ_U(AFS_ST_SOFTLINK, 3u, "ST_SOFTLINK");
    CHECK_EQ_U(AFS_ST_LINKDIR, 4u, "ST_LINKDIR");
    CHECK_EQ_U(AFS_ST_FILE, 0xFFFFFFFDu, "ST_FILE is -3");
    CHECK_EQ_U(AFS_ST_LINKFILE, 0xFFFFFFFCu, "ST_LINKFILE is -4");
}

/* --- hash-table size derives from the block size, nothing hardcoded --- */
static void test_hash_table_size(void) {
    CHECK_EQ_U(afs_hash_table_size(512), 72u, "512-byte block -> 72 entries");
    CHECK_EQ_U(afs_hash_table_size(1024), 200u, "1024-byte block -> 200 entries");
    CHECK_EQ_U(afs_hash_table_size(2048), 456u, "2048-byte block -> 456 entries");
    /* must never underflow into a huge table for a nonsense block size */
    CHECK(afs_hash_table_size(64) == 0, "a too-small block size yields 0, not underflow");
}

/* --- rootblock, modelled on the real testffs.adf rootblock --- */
static void test_rootblock_parse(void) {
    uint8_t r[512];
    memset(r, 0, sizeof r);
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, 72);             /* ht_size */
    put_be32(r + 312, 0xFFFFFFFFu);   /* bm_flag = -1 (valid) */
    put_be32(r + 316, 882);           /* bm_pages[0] */
    r[432] = 9;                       /* name_len */
    memcpy(r + 433, "ffs_cache", 9);
    put_be32(r + 504, 881);           /* extension = first dircache block */
    put_be32(r + 508, AFS_ST_ROOT);

    afs_rootblock_t rb;
    int rc = afs_parse_rootblock(r, 512, &rb);
    CHECK_EQ_U(rc, 0, "rootblock parses");
    CHECK_EQ_U(rb.type, AFS_T_HEADER, "rootblock primary type");
    CHECK_EQ_U(rb.ht_size, 72u, "rootblock hash table size");
    CHECK_EQ_U(rb.bm_flag, 0xFFFFFFFFu, "bitmap-valid flag is -1");
    CHECK_EQ_U(rb.bm_pages[0], 882u, "first bitmap block pointer");
    CHECK_EQ_U(rb.bm_ext, 0u, "no bitmap extension");
    CHECK_EQ_U(rb.dircache_block, 881u, "first dir-cache block (extension field)");
    CHECK_EQ_S(rb.name, "ffs_cache", "volume name");
}

/* --- file header block --- */
static void test_file_header_parse(void) {
    uint8_t h[512];
    memset(h, 0, sizeof h);
    put_be32(h + 0, AFS_T_HEADER);
    put_be32(h + 8, 3);               /* high_seq: 3 data blocks */
    put_be32(h + 16, 200);            /* first_data */
    put_be16(h + 316, 1000);          /* UID  (16-bit, per C2) */
    put_be16(h + 318, 2000);          /* GID  (16-bit) */
    put_be32(h + 320, 0x1E);          /* protect bits */
    put_be32(h + 324, 1234);          /* byte_size */
    h[432] = 8;
    memcpy(h + 433, "MOON.GIF", 8);
    put_be32(h + 496, 900);           /* hash_chain */
    put_be32(h + 500, 880);           /* parent */
    put_be32(h + 504, 0);             /* extension */
    put_be32(h + 508, AFS_ST_FILE);

    afs_fileheader_t fh;
    int rc = afs_parse_fileheader(h, 512, &fh);
    CHECK_EQ_U(rc, 0, "file header parses");
    CHECK_EQ_U(fh.high_seq, 3u, "high_seq (data block count)");
    CHECK_EQ_U(fh.first_data, 200u, "first_data");
    CHECK_EQ_U(fh.uid, 1000u, "UID is 16-bit");
    CHECK_EQ_U(fh.gid, 2000u, "GID is 16-bit");
    CHECK_EQ_U(fh.protect, 0x1Eu, "protection bits");
    CHECK_EQ_U(fh.byte_size, 1234u, "byte size");
    CHECK_EQ_S(fh.name, "MOON.GIF", "file name");
    CHECK_EQ_U(fh.hash_chain, 900u, "hash chain pointer");
    CHECK_EQ_U(fh.parent, 880u, "parent directory");
}

/* --- dir-cache block: type 33, records with 16-bit fields (C2 again) --- */
static void test_dircache_parse(void) {
    uint8_t d[512];
    memset(d, 0, sizeof d);
    put_be32(d + 0, AFS_T_DIRCACHE);
    put_be32(d + 4, 881);       /* header_key (self) */
    put_be32(d + 8, 880);       /* parent */
    put_be32(d + 12, 1);        /* records_nb */
    put_be32(d + 16, 1220);     /* next_dirc */
    /* one record at offset 24: 26 bytes when name_len=9 and comm_len=0 */
    size_t o = 24;
    put_be32(d + o + 0, 1144);        /* header block pointer */
    put_be32(d + o + 4, 0);           /* size (0 = directory) */
    put_be32(d + o + 8, 0x1);         /* protect */
    put_be16(d + o + 12, 7);          /* UID  */
    put_be16(d + o + 14, 8);          /* GID  */
    d[o + 16] = 0; d[o + 17] = 0x1C;  /* days (signed short) */
    d[o + 22] = AFS_ST_USERDIR;       /* secondary type */
    d[o + 23] = 9;                    /* name_len */
    memcpy(d + o + 24, "empty_dir", 9);
    d[o + 33] = 0;                    /* comm_len */
    d[o + 34] = 0;                    /* padding byte */

    afs_dircache_t dc;
    int rc = afs_parse_dircache(d, 512, &dc);
    CHECK_EQ_U(rc, 0, "dir-cache block parses");
    CHECK_EQ_U(dc.parent, 880u, "dir-cache parent");
    CHECK_EQ_U(dc.records_nb, 1u, "dir-cache record count");
    CHECK_EQ_U(dc.next_dirc, 1220u, "dir-cache chain continues");
    if (dc.records_nb > 0) {
        CHECK_EQ_U(dc.rec[0].header, 1144u, "record header block pointer");
        CHECK_EQ_U(dc.rec[0].uid, 7u, "record UID is 16-bit");
        CHECK_EQ_U(dc.rec[0].gid, 8u, "record GID is 16-bit");
        CHECK_EQ_U(dc.rec[0].sec_type, AFS_ST_USERDIR, "record secondary type");
        CHECK_EQ_S(dc.rec[0].name, "empty_dir", "record name");
    } else {
        CHECK(0, "dir-cache record was not decoded");
    }
}

/* --- OFS data block header (type 8) vs FFS raw payload --- */
static void test_ofs_datablock(void) {
    uint8_t b[512];
    memset(b, 0, sizeof b);
    put_be32(b + 0, AFS_T_DATA);
    put_be32(b + 4, 200);       /* header_key -> file header */
    put_be32(b + 8, 1);         /* seq_num */
    put_be32(b + 12, 488);      /* data_size = BSIZE-24 */
    put_be32(b + 16, 205);      /* next_data */
    memcpy(b + 24, "DATA", 4);

    afs_ofsdatablock_t ob;
    int rc = afs_parse_ofs_datablock(b, 512, &ob);
    CHECK_EQ_U(rc, 0, "OFS data block parses");
    CHECK_EQ_U(ob.seq_num, 1u, "sequence number");
    CHECK_EQ_U(ob.data_size, 488u, "data size is BSIZE-24");
    CHECK_EQ_U(ob.next_data, 205u, "next_data pointer");
    CHECK_EQ_S((const char *)ob.payload, "DATA", "payload begins at byte 24");
}

/* --- block type identification --- */
static void test_block_identification(void) {
    uint8_t b[512];
    memset(b, 0, sizeof b);

    put_be32(b + 0, AFS_T_HEADER);
    put_be32(b + 508, AFS_ST_ROOT);
    CHECK_EQ_U(afs_block_kind(b, 512), AFS_BLOCK_ROOT, "T_HEADER/ST_ROOT is a rootblock");

    put_be32(b + 508, AFS_ST_USERDIR);
    CHECK_EQ_U(afs_block_kind(b, 512), AFS_BLOCK_DIR, "T_HEADER/ST_USERDIR is a directory");

    put_be32(b + 508, AFS_ST_FILE);
    CHECK_EQ_U(afs_block_kind(b, 512), AFS_BLOCK_FILE, "T_HEADER/ST_FILE is a file");

    put_be32(b + 508, AFS_ST_SOFTLINK);
    CHECK_EQ_U(afs_block_kind(b, 512), AFS_BLOCK_SOFTLINK, "T_HEADER/ST_SOFTLINK is a symlink");

    put_be32(b + 0, AFS_T_LIST);
    put_be32(b + 508, AFS_ST_FILE);
    CHECK_EQ_U(afs_block_kind(b, 512), AFS_BLOCK_FILEEXT, "T_LIST/ST_FILE is a file extension");

    put_be32(b + 0, AFS_T_DIRCACHE);
    CHECK_EQ_U(afs_block_kind(b, 512), AFS_BLOCK_DIRCACHE, "type 33 is a dir-cache block");

    put_be32(b + 0, 0x1234);
    CHECK_EQ_U(afs_block_kind(b, 512), AFS_BLOCK_UNKNOWN, "an unknown primary type is unknown");
}

int main(void) {
    printf("blocktypes\n");
    test_type_constants();
    test_hash_table_size();
    test_rootblock_parse();
    test_file_header_parse();
    test_dircache_parse();
    test_ofs_datablock();
    test_block_identification();
    TEST_REPORT();
}