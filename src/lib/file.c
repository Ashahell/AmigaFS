#include "amigafs/file.h"
#include "amigafs/be.h"
#include "amigafs/hash.h"

#include <stdlib.h>
#include <string.h>

/* Number of data-block pointers a header/extension block can hold: BSIZE/4-56,
 * and the file payload size of one data block for the volume's variant. */
static size_t table_entries(size_t bsize) {
    return afs_hash_table_size(bsize);   /* BSIZE/4 - 56 */
}

static size_t data_payload(afs_volume_t *v, size_t bsize) {
    /* OFS carries a 24-byte header on every data block */
    return afs_volume_is_ofs(v) ? (bsize - 24) : bsize;
}

/* Resolve a directory entry to the file header block that holds its data.
 * A hard link (ST_LINKFILE) carries `real_entry` pointing at the real file;
 * a plain file IS its own header block. */
static int resolve_file_block(afs_volume_t *v, uint32_t entryblock,
                              uint32_t *fileblock, uint8_t *kind) {
    size_t bsize = afs_volume_block_size(v);
    uint8_t *b = malloc(bsize);
    if (!b) return -1;
    if (afs_volume_read_block(v, entryblock, b, bsize) != 0) { free(b); return -1; }

    afs_block_kind_t k = afs_block_kind(b, bsize);
    if (kind) *kind = (uint8_t)k;

    if (k == AFS_BLOCK_FILE) {
        *fileblock = entryblock;
        free(b);
        return 0;
    }
    if (k == AFS_BLOCK_LINKFILE) {
        /* real_entry at BSIZE-44 */
        uint32_t real = be32(b + bsize - 44);
        free(b);
        if (real == 0) return -1;
        *fileblock = real;
        return 0;
    }
    free(b);
    return -1;   /* a directory or softlink is not a file here */
}

/* Collect the file's data block numbers, walking the header -> extension
 * chain until `need` pointers have been gathered. The table is REVERSED, so
 * the pointer for file block i is at index (entries-1-i). */
static int collect_blocks(afs_volume_t *v, uint32_t fileblock,
                          size_t need, uint32_t **out) {
    size_t bsize = afs_volume_block_size(v);
    size_t entries = table_entries(bsize);
    if (need == 0) { *out = NULL; return 0; }

    uint32_t *ptrs = malloc(need * sizeof *ptrs);
    if (!ptrs) return -1;

    size_t got = 0;
    uint32_t cur = fileblock;
    uint32_t guard = 0;
    uint32_t limit = afs_volume_block_count(v);

    while (got < need && cur != 0) {
        if (cur >= limit || guard++ > limit) { free(ptrs); return -1; }  /* corrupt chain */
        uint8_t *b = malloc(bsize);
        if (!b) { free(ptrs); return -1; }
        if (afs_volume_read_block(v, cur, b, bsize) != 0) { free(b); free(ptrs); return -1; }

        uint32_t hs = be32(b + 8);            /* high_seq: pointers in THIS block */
        if (hs > entries) hs = (uint32_t)entries;
        size_t take = hs;
        if (take > need - got) take = need - got;

        for (size_t k = 0; k < take; k++) {
            size_t idx = entries - 1 - k;     /* REVERSED */
            ptrs[got + k] = be32(b + 24 + 4 * idx);
        }
        got += take;
        cur = be32(b + bsize - 8);            /* extension -> next table block */
        free(b);
    }

    if (got < need) { free(ptrs); return -1; }   /* file claims more than exists */
    *out = ptrs;
    return 0;
}

static int file_stat_resolved(afs_volume_t *v, uint32_t fileblock,
                              afs_filestat_t *out) {
    size_t bsize = afs_volume_block_size(v);
    uint8_t *b = malloc(bsize);
    if (!b) return -1;
    if (afs_volume_read_block(v, fileblock, b, bsize) != 0) { free(b); return -1; }

    out->block = fileblock;
    out->kind = (uint8_t)afs_block_kind(b, bsize);
    out->byte_size = be32(b + bsize - 188);
    out->protect = be32(b + bsize - 192);
    out->uid = be16(b + bsize - 196);
    out->gid = be16(b + bsize - 194);
    bstr(b + bsize - 80, 31, out->name, sizeof out->name);
    free(b);
    return 0;
}

int afs_file_stat(afs_volume_t *v, uint32_t dirblock,
                  const char *name, size_t namelen, afs_filestat_t *out) {
    if (!v || !name || !out) return -1;
    afs_dirent_t e;
    if (afs_dir_lookup(v, dirblock, name, namelen, &e) != 0) return -1;
    uint32_t fblk;
    uint8_t kind;
    if (resolve_file_block(v, e.block, &fblk, &kind) != 0) return -1;
    return file_stat_resolved(v, fblk, out);
}

static int read_into(afs_volume_t *v, uint32_t fileblock,
                     uint8_t *out, uint64_t offset, size_t want, size_t *got) {
    size_t bsize = afs_volume_block_size(v);
    size_t payload = data_payload(v, bsize);
    afs_filestat_t st;
    if (file_stat_resolved(v, fileblock, &st) != 0) return -1;

    /* clamp the request to the file size */
    if (offset >= st.byte_size) { *got = 0; return 0; }
    uint64_t avail = st.byte_size - offset;
    if (want > avail) want = (size_t)avail;

    size_t first = (size_t)(offset / payload);
    size_t skip0 = (size_t)(offset % payload);
    /* the span must account for the partial skip INSIDE the first block:
     * offset 500 with a 512-byte payload needs 2 blocks, not 1 */
    size_t nblocks = (skip0 + want + payload - 1) / payload;

    uint32_t *ptrs = NULL;
    if (collect_blocks(v, fileblock, first + nblocks, &ptrs) != 0) return -1;

    uint8_t *blkbuf = malloc(bsize);
    if (!blkbuf) { free(ptrs); return -1; }

    size_t written = 0;
    for (size_t i = 0; i < nblocks; i++) {
        uint32_t p = ptrs[first + i];
        if (p == 0 || afs_volume_read_block(v, p, blkbuf, bsize) != 0) break;
        const uint8_t *src = blkbuf;
        if (afs_volume_is_ofs(v)) {
            /* OFS: skip the 24-byte header, honour this block's data_size */
            uint32_t dsz = be32(blkbuf + 12);
            if (dsz > payload) dsz = (uint32_t)payload;
            src = blkbuf + 24;
            size_t copy = dsz;
            size_t skip = 0;
            if (i == 0) { skip = skip0; if (skip > copy) skip = copy; copy -= skip; }
            if (copy > want - written) copy = want - written;
            if (copy) memcpy(out + written, src + skip, copy);
            written += copy;
        } else {
            /* FFS: raw payload */
            size_t skip = 0;
            if (i == 0) skip = skip0;
            size_t copy = payload - skip;
            if (copy > want - written) copy = want - written;
            if (copy) memcpy(out + written, src + skip, copy);
            written += copy;
        }
        if (written >= want) break;
    }
    free(blkbuf);
    free(ptrs);
    *got = written;
    return 0;
}

int afs_file_read(afs_volume_t *v, uint32_t dirblock,
                  const char *name, size_t namelen,
                  void *buf, size_t len, uint64_t offset, size_t *got) {
    if (!v || !name || !buf || !got) return -1;
    afs_dirent_t e;
    if (afs_dir_lookup(v, dirblock, name, namelen, &e) != 0) return -1;
    uint32_t fblk;
    uint8_t kind;
    if (resolve_file_block(v, e.block, &fblk, &kind) != 0) return -1;
    if (len == 0) { *got = 0; return 0; }
    return read_into(v, fblk, (uint8_t *)buf, offset, len, got);
}

int afs_file_read_all(afs_volume_t *v, uint32_t dirblock,
                       const char *name, size_t namelen,
                       void **out, size_t *out_len) {
    if (!v || !name || !out) return -1;
    *out = NULL;
    afs_filestat_t st;
    if (afs_file_stat(v, dirblock, name, namelen, &st) != 0) return -1;
    uint8_t *buf = malloc(st.byte_size ? st.byte_size : 1);
    if (!buf) return -1;
    size_t got = 0;
    if (afs_file_read(v, dirblock, name, namelen, buf, st.byte_size, 0, &got) != 0) {
        free(buf);
        return -1;
    }
    *out = buf;
    if (out_len) *out_len = got;
    return 0;
}
/* Read by header block (the FUSE path): resolve a hard link first, then read. */
int afs_read_file_by_block(afs_volume_t *v, uint32_t fileblock,
                           void *out, size_t len, uint64_t offset, size_t *got) {
    if (!v || !out || !got) return -1;
    uint32_t fblk;
    uint8_t kind;
    if (resolve_file_block(v, fileblock, &fblk, &kind) != 0) return -1;
    if (len == 0) { *got = 0; return 0; }
    return read_into(v, fblk, (uint8_t *)out, offset, len, got);
}
