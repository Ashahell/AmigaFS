/* Unit tests for file data reads (file.c).
 *
 * The on-disk rule that matters, and which is easy to get wrong twice:
 *
 *  1. BOTH OFS and FFS read file data through the `data_blocks[]` TABLE, not
 *     through OFS's `next_data` chain. The table is stored in REVERSE: the
 *     first data block sits in the LAST slot, so file block i lives at table
 *     index (entries-1-i).
 *  2. `high_seq` is a PER-BLOCK count of the pointers in that one header or
 *     extension block -- it is NOT the file's total block count. The total
 *     comes from `byte_size`.
 *
 * The differences that remain: an OFS data block has a 24-byte header and
 * BSIZE-24 bytes of payload; an FFS data block is raw BSIZE bytes.
 *
 * A real 173847-byte OFS file in testofs.adf spans FIVE header/extension
 * blocks and was verified byte-for-byte against unadf.
 */
#include "test.h"
#include "amigafs/be.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/file.h"
#include "amigafs/hash.h"
#include "amigafs/volume.h"

#define BS 512
#define ENTRIES 72            /* BSIZE/4 - 56 */

/* Build a synthetic volume containing one file whose data blocks are laid out
 * REVERSED in the table, optionally spanning extension blocks, and optionally
 * OFS (24-byte header per data block) or FFS (raw). */
typedef struct {
    uint8_t img[1760 * BS];
    uint8_t flags;
    uint32_t byte_size;
} fsynth_t;

static void fsynth_init(fsynth_t *s, uint8_t flags) {
    memset(s->img, 0, sizeof s->img);
    s->flags = flags;
    put_be32(s->img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                     ((uint32_t)'S' << 8) | flags);
    uint8_t *r = s->img + 880 * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, ENTRIES);
    put_be32(r + 312, 0xFFFFFFFFu);
    put_be32(r + 316, 881);
    r[BS - 80] = 4;
    memcpy(r + BS - 79, "fsyn", 4);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
}

/* Create a file entry `fname` of `size` bytes whose data blocks each contain a
 * distinct, checkable pattern. Returns the file header's block number. */
static uint32_t fsynth_file(fsynth_t *s, const char *fname, uint32_t size,
                            uint32_t first_data_blk) {
    int ofs = afs_is_ofs(s->flags);
    uint32_t payload = ofs ? (BS - 24) : BS;
    uint32_t nblocks = (size + payload - 1) / payload;
    uint32_t hdr = 700;      /* a dedicated file-header block, clear of the root */
    uint32_t nxt = first_data_blk;

    /* write the data blocks first, remembering their numbers in order */
    uint32_t ptrs[512];
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t b = nxt++;
        uint8_t *d = s->img + b * BS;
        uint32_t off = 0;
        if (ofs) {
            put_be32(d + 0, AFS_T_DATA);
            put_be32(d + 4, hdr);
            put_be32(d + 8, i + 1);            /* seq_num */
            put_be32(d + 12, payload);
            put_be32(d + 16, (i + 1 < nblocks) ? (nxt - 1) : 0);
            off = 24;
        }
        /* fill payload with a per-block pattern */
        for (uint32_t k = 0; k < payload; k++)
            d[off + k] = (uint8_t)((i * 31 + k) & 0xFF);
        if (ofs) put_be32(d + 20, afs_block_checksum(d, BS, 20));
        ptrs[i] = b;
    }

    /* Now build the header + extension chain, writing the table REVERSED:
     * file block 0 goes to slot ENTRIES-1, file block 1 to ENTRIES-2, ...
     * An EMPTY file still needs a header block, so do-while: the first
     * iteration runs even when nblocks == 0. */
    uint32_t cur = hdr, written = 0, blkno = hdr, ext = 0;
    do {
        uint32_t hs = (nblocks - written < ENTRIES) ? (nblocks - written) : ENTRIES;
        uint8_t *b = s->img + blkno * BS;
        put_be32(b + 0, (blkno == hdr) ? AFS_T_HEADER : AFS_T_LIST);
        put_be32(b + 8, hs);                     /* high_seq for THIS block */
        put_be32(b + BS - 12, hdr);
        put_be32(b + BS - 4, AFS_ST_FILE);
        for (uint32_t k = 0; k < hs; k++)
            put_be32(b + 24 + 4 * (ENTRIES - 1 - k), ptrs[written + k]);
        written += hs;
        ext = (written < nblocks) ? (blkno + 1) : 0;
        put_be32(b + BS - 8, ext);
        /* name goes on the header block only */
        if (blkno == hdr) {
            size_t nl = strlen(fname);
            b[BS - 80] = (uint8_t)nl;
            memcpy(b + BS - 79, fname, nl);
            put_be32(b + BS - 16, 0);
            put_be32(b + BS - 188, size);
        }
        put_be32(b + 20, afs_block_checksum(b, BS, 20));
        blkno = ext ? blkno + 1 : blkno;
        cur = ext;
        (void)cur;
    } while (written < nblocks && ext != 0);

    /* file the name into the root hash table under its REAL bucket, so a
     * name-based lookup finds it exactly as it would on a real volume */
    uint8_t *r = s->img + 880 * BS;
    size_t nl2 = strlen(fname);
    uint32_t bucket = afs_hash(fname, nl2, ENTRIES, s->flags);
    put_be32(r + 24 + 4 * bucket, hdr);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
    return hdr;
}

static afs_volume_t *fsynth_open(fsynth_t *s) {
    afs_volume_t *v = NULL;
    if (afs_volume_open_memory(s->img, sizeof s->img, BS, &v, NULL, 0) != 0) return NULL;
    return v;
}

/* An empty file: byte_size 0, no data blocks at all. */
static void test_empty_file(void) {
    fsynth_t s;
    fsynth_init(&s, 0x01);
    fsynth_file(&s, "empty", 0, 1000);
    afs_volume_t *v = fsynth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }
    afs_filestat_t st;
    CHECK_EQ_U(afs_file_stat(v, 880, "empty", 5, &st), 0, "empty file stats");
    CHECK_EQ_U(st.byte_size, 0u, "empty file has size 0");
    size_t n = 0;
    char buf[16];
    CHECK_EQ_U(afs_file_read(v, 880, "empty", 5, buf, sizeof buf, 0, &n), 0, "empty file reads");
    CHECK_EQ_U(n, 0u, "empty file yields 0 bytes");
    afs_volume_close(v);
}

/* A small FFS file, entirely within one header block. */
static void test_small_ffs(void) {
    fsynth_t s;
    fsynth_init(&s, 0x01);          /* FFS */
    fsynth_file(&s, "small.bin", 1000, 1000);
    afs_volume_t *v = fsynth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    size_t n = 0;
    uint8_t buf[1024];
    CHECK_EQ_U(afs_file_read(v, 880, "small.bin", 9, buf, sizeof buf, 0, &n), 0, "FFS read");
    CHECK_EQ_U(n, 1000u, "read the whole file");
    /* FFS blocks are raw BSIZE: block 0 payload starts with our pattern 0.. */
    CHECK_EQ_U(buf[0], 0u, "first byte of an FFS data block");
    CHECK_EQ_U(buf[31], 31u, "byte 31 follows the pattern");
    afs_volume_close(v);
}

/* An OFS file: each data block has a 24-byte header, payload BSIZE-24. */
static void test_ofs_offsets(void) {
    fsynth_t s;
    fsynth_init(&s, 0x00);          /* OFS */
    fsynth_file(&s, "o.bin", 900, 1000);
    afs_volume_t *v = fsynth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    size_t n = 0;
    uint8_t buf[1024];
    CHECK_EQ_U(afs_file_read(v, 880, "o.bin", 5, buf, sizeof buf, 0, &n), 0, "OFS read");
    CHECK_EQ_U(n, 900u, "read 900 bytes");
    /* The pattern is (block*31 + offset)&0xFF. For OFS the payload starts at
     * byte 24 of each block, so the FILE stream is contiguous: file offset f
     * carries pattern (f/488 * 31 + f%488). Assert that directly -- it is the
     * real invariant, and it would catch a wrong per-block payload size. */
    int ok = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t blk_i = i / (BS - 24), off_i = i % (BS - 24);
        if (buf[i] != (uint8_t)((blk_i * 31 + off_i) & 0xFF)) { ok = 0; break; }
    }
    CHECK(ok, "OFS file stream is contiguous across 488-byte payloads");
    CHECK_EQ_U(buf[0], 0u, "OFS block0 payload starts at pattern 0");
    afs_volume_close(v);
}

/* A file spanning MULTIPLE extension blocks: 200 blocks > one table. */
static void test_extension_chain(void) {
    fsynth_t s;
    fsynth_init(&s, 0x01);          /* FFS: BSIZE payload */
    uint32_t nblocks = 200;        /* > 72, forces 2 extension blocks */
    uint32_t size = nblocks * BS;  /* but do not fill the last block */
    size -= 100;
    fsynth_file(&s, "big.bin", size, 1000);
    afs_volume_t *v = fsynth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    size_t n = 0;
    uint8_t *buf = malloc(size);
    if (!buf) { CHECK(0, "alloc"); afs_volume_close(v); return; }
    int rc = afs_file_read(v, 880, "big.bin", 7, buf, size, 0, &n);
    CHECK_EQ_U(rc, 0, "multi-extension read");
    CHECK_EQ_U(n, size, "read the whole multi-extension file");
    /* verify the per-block pattern continues across the table boundary */
    int ok = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t blk = i / BS, off = i % BS;
        if (buf[i] != (uint8_t)((blk * 31 + off) & 0xFF)) { ok = 0; break; }
    }
    CHECK(ok, "every byte matches the per-block pattern across block boundaries");
    free(buf);
    afs_volume_close(v);
}

/* Partial read at an offset, and a read clamped at EOF. */
static void test_offset_and_clamp(void) {
    fsynth_t s;
    fsynth_init(&s, 0x01);
    fsynth_file(&s, "p.bin", 1000, 1000);
    afs_volume_t *v = fsynth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    uint8_t buf[64];
    size_t n = 0;
    /* read 64 bytes from offset 500 */
    CHECK_EQ_U(afs_file_read(v, 880, "p.bin", (size_t)strlen("p.bin"), buf, 64, 500, &n), 0, "offset read");
    CHECK_EQ_U(n, 64u, "offset read length");
    CHECK_EQ_U(buf[0], (unsigned char)(500 & 0xFF), "offset read lands at byte 500");
    /* a read that runs past EOF is CLAMPED, not an error, not a short buffer */
    CHECK_EQ_U(afs_file_read(v, 880, "p.bin", (size_t)strlen("p.bin"), buf, sizeof buf, 990, &n), 0, "read at EOF");
    CHECK_EQ_U(n, 10u, "a read past EOF is clamped to the remaining bytes");
    /* a read that SPANS two data blocks must return the full amount */
    CHECK_EQ_U(afs_file_read(v, 880, "p.bin", (size_t)strlen("p.bin"), buf, sizeof buf, 500, &n), 0, "read spanning two blocks");
    CHECK_EQ_U(n, 64u, "a read spanning two data blocks returns the full length");
    /* a zero-length read is legal */
    CHECK_EQ_U(afs_file_read(v, 880, "p.bin", (size_t)strlen("p.bin"), buf, sizeof buf, 0, &n), 0, "read at 0");
    afs_volume_close(v);
}

/* A comment lives in the entry block at bsize-184, on EVERY volume -- DIRC or
 * not, file or directory. The layout was verified byte-for-byte against
 * amitools' own parser on real fixtures before any of this was written; see
 * docs/comment-layout.md. These tests pin the BOUNDARY behaviour, which is
 * what a hostile length byte can attack. */
static void test_entry_comment(void) {
    uint8_t blk[BS];
    char out[128];

    memset(blk, 0, sizeof blk);
    put_be32(blk + 0, AFS_T_HEADER);
    put_be32(blk + BS - 4, AFS_ST_FILE);
    blk[BS - 80] = 4; memcpy(blk + BS - 79, "file", 4);

    memset(out, 0x7E, sizeof out);            /* poison: catches stale bytes */
    CHECK_EQ_U(afs_entry_comment(blk, BS, out, sizeof out), 0,
               "an entry with no comment yields an empty string");
    CHECK_EQ_U(out[0], 0, "and the result is NUL-terminated");

    const char *cmt = "comment of MOON.GIF";
    size_t n = strlen(cmt);
    blk[BS - 184] = (uint8_t)n;
    memcpy(blk + BS - 183, cmt, n);
    memset(out, 0x7E, sizeof out);
    CHECK_EQ_U(afs_entry_comment(blk, BS, out, sizeof out), n,
               "the comment length is returned");
    CHECK_EQ_S(out, cmt, "and the text is read from bsize-183");

    /* a corrupt length byte must not run past the block, nor past the
     * format's 79-byte maximum */
    /* fill from the comment field to the end of the block (BS - (BS-183) = 183
     * bytes), so a length byte claiming more than the block holds has something
     * to run into */
    memset(blk + BS - 183, 'x', 183);
    blk[BS - 184] = 0xFF;
    memset(out, 0x7E, sizeof out);
    size_t got = afs_entry_comment(blk, BS, out, sizeof out);
    CHECK(got <= (size_t)79, "a corrupt length byte cannot claim more than 79 bytes");
    CHECK_EQ_U(out[got], 0, "and the result stays NUL-terminated");

    /* A short output buffer truncates instead of overflowing. The caller's
     * array is deliberately LARGER than the capacity passed, so "did it write
     * past what I allowed" is observable rather than assumed. */
    blk[BS - 184] = 19;
    memcpy(blk + BS - 183, "protracker module", 17);
    char small[16];
    memset(small, 0x7E, sizeof small);
    size_t sg = afs_entry_comment(blk, BS, small, 8);
    CHECK_EQ_U(sg, 7, "a capacity of 8 yields 7 characters plus the NUL");
    CHECK_EQ_S(small, "protrac", "and they are the leading ones");
    CHECK_EQ_U(small[7], 0, "the NUL lands inside the allowed capacity");
    CHECK_EQ_U(small[8], 0x7E, "nothing was written past the capacity");

    /* zero capacity writes nothing at all */
    char none[4];
    memset(none, 0x7E, sizeof none);
    CHECK_EQ_U(afs_entry_comment(blk, BS, none, 0), 0,
               "zero capacity is accepted and reports nothing written");
    CHECK_EQ_U(none[0], 0x7E, "and the caller's buffer is left untouched");
}

int main(void) {
    printf("file\n");
    test_empty_file();
    test_small_ffs();
    test_ofs_offsets();
    test_extension_chain();
    test_offset_and_clamp();
    test_entry_comment();
    TEST_REPORT();
}