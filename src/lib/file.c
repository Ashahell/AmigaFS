#include "amigafs/file.h"
#include "amigafs/alloc.h"
#include "amigafs/be.h"
#include "amigafs/checksum.h"
#include "amigafs/hash.h"
#include "amigafs/names.h"
#include "amigafs/translate.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *buf, size_t len, const char *fmt, ...) {
    va_list ap;
    if (!buf || len == 0) return;
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

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

/* The comment field's distance from the end of the block, and the format's
 * maximum. Verified against amitools' EntryBlock._read_nac_modts, which reads
 * it as _get_bstr(-46, 79) -- a plain absolute offset bsize - 4*46. */
#define AFS_COMMENT_OFF 184u
#define AFS_COMMENT_MAX 79u

size_t afs_entry_comment(const uint8_t *blk, size_t bsize,
                         char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    /* the field must lie wholly inside the block, whatever the block size */
    if (!blk || bsize <= AFS_COMMENT_OFF) return 0;

    size_t len = blk[bsize - AFS_COMMENT_OFF];
    if (len > AFS_COMMENT_MAX) len = AFS_COMMENT_MAX;      /* corrupt length */
    size_t room = bsize - (AFS_COMMENT_OFF - 1u);          /* data to end of blk */
    if (len > room) len = room;                             /* never past the end */

    if (len > cap - 1) len = cap - 1;                       /* never past the cap */
    if (len) memcpy(out, blk + bsize - (AFS_COMMENT_OFF - 1u), len);
    out[len] = '\0';
    return len;
}

int afs_file_comment(afs_volume_t *v, uint32_t dirblock,
                     const char *name, size_t namelen,
                     char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!v || !name) return -1;
    afs_dirent_t e;
    if (afs_dir_lookup(v, dirblock, name, namelen, &e) != 0) return -1;
    /* a hard link resolves to its REAL entry, so both names for one inode
     * report the same comment rather than an empty one */
    uint32_t fblk;
    uint8_t kind;
    if (resolve_file_block(v, e.block, &fblk, &kind) != 0) return -1;

    size_t bsize = afs_volume_block_size(v);
    uint8_t *blk = malloc(bsize);
    if (!blk) return -1;
    if (afs_volume_read_block(v, fblk, blk, bsize) != 0) {
        free(blk);
        return -1;
    }
    afs_entry_comment(blk, bsize, out, cap);
    free(blk);
    return 0;
}

/* --- D3 write phase --- */

int afs_file_create(afs_volume_t *v, uint32_t dirblock,
                    const char *name, size_t namelen, uint32_t *hdr_out,
                    char *errbuf, size_t errlen) {
    size_t bs;
    uint8_t *b = NULL;
    uint32_t nb;
    afs_dirent_t dup;
    afs_dircache_rec_t rec;
    uint32_t dd, mm, tt;

    if (!v || !name || !hdr_out) {
        set_err(errbuf, errlen, "bad arguments");
        return -1;
    }
    if (!afs_name_valid(name, namelen)) {
        set_err(errbuf, errlen, "invalid name (1..30 bytes, no '/' or ':')");
        return -1;
    }
    /* duplicate by the same lookup a later read will use */
    if (afs_dir_lookup(v, dirblock, name, namelen, &dup) == 0) {
        set_err(errbuf, errlen, "'%.*s' already exists", (int)namelen, name);
        return -1;
    }
    bs = afs_volume_block_size(v);
    if (afs_alloc_block(v, &nb, errbuf, errlen) != 0) return -1;
    b = calloc(1, bs);
    if (!b) { set_err(errbuf, errlen, "out of memory"); return -1; }

    afs_now_amiga(&dd, &mm, &tt);
    put_be32(b + 0, AFS_T_HEADER);
    put_be32(b + 4, nb);
    /* +8 high_seq 0, +16 first_data 0, data table stays zeroed */
    put_be32(b + bs - 196, 0);
    put_be32(b + bs - 194, 0);
    put_be32(b + bs - 192, 0);
    put_be32(b + bs - 188, 0);
    put_be32(b + bs - 92, dd);
    put_be32(b + bs - 88, mm);
    put_be32(b + bs - 84, tt);
    b[bs - 80] = (uint8_t)namelen;
    memcpy(b + bs - 79, name, namelen);
    put_be32(b + bs - 44, 0);
    put_be32(b + bs - 40, 0);
    b[bs - 184] = 0;                    /* no comment */
    put_be32(b + bs - 16, 0);           /* no chain successor yet */
    put_be32(b + bs - 12, dirblock);
    put_be32(b + bs - 8, 0);            /* no extension yet */
    put_be32(b + bs - 4, AFS_ST_FILE);
    put_be32(b + 20, afs_block_checksum(b, bs, 20));
    if (afs_volume_write_block(v, nb, b, bs) != 0) {
        free(b);
        set_err(errbuf, errlen, "cannot write file header");
        return -1;
    }
    free(b);
    if (afs_dir_link_entry(v, dirblock, nb, errbuf, errlen) != 0) return -1;

    memset(&rec, 0, sizeof rec);
    rec.header = nb;
    rec.size = 0;
    rec.days = (int16_t)dd; rec.mins = (int16_t)mm; rec.ticks = (int16_t)tt;
    rec.sec_type = (uint8_t)AFS_ST_FILE;
    memcpy(rec.name, name, namelen < 30 ? namelen : 30);
    if (afs_dircache_upsert(v, dirblock, &rec, errbuf, errlen) != 0) return -1;

    *hdr_out = nb;
    return 0;
}

int afs_file_write_all(afs_volume_t *v, uint32_t hdrblock,
                       const void *data, size_t len,
                       char *errbuf, size_t errlen) {
    size_t bs, payload, entries;
    uint8_t *hdr = NULL;
    uint32_t nblocks, ntables;
    uint32_t *ext = NULL, *dblks = NULL;
    uint32_t dd, mm, tt;
    uint32_t parent;

    if (!v) { set_err(errbuf, errlen, "bad arguments"); return -1; }
    if (len > 0 && !data) { set_err(errbuf, errlen, "bad arguments"); return -1; }
    if (len > 0xFFFFFFFFu) {
        set_err(errbuf, errlen, "file too large (past 4 GiB)");
        return -1;
    }
    bs = afs_volume_block_size(v);
    payload = data_payload(v, bs);
    entries = table_entries(bs);
    hdr = malloc(bs);
    if (!hdr) { set_err(errbuf, errlen, "out of memory"); return -1; }
    if (afs_volume_read_block(v, hdrblock, hdr, bs) != 0) {
        free(hdr);
        set_err(errbuf, errlen, "cannot read file header %u", hdrblock);
        return -1;
    }
    if (afs_block_kind(hdr, bs) != AFS_BLOCK_FILE) {
        free(hdr);
        set_err(errbuf, errlen, "block %u is not a plain file", hdrblock);
        return -1;
    }
    /* no overwrite yet: a second write to live data is refused, not merged */
    if (be32(hdr + bs - 188) != 0 || be32(hdr + 8) != 0 ||
        be32(hdr + bs - 8) != 0) {
        free(hdr);
        set_err(errbuf, errlen, "file already has data (no overwrite yet)");
        return -1;
    }
    parent = be32(hdr + bs - 12);

    nblocks = (len == 0) ? 0 : (uint32_t)((len + payload - 1) / payload);
    ntables = (nblocks <= entries) ? 1 : 1 + (uint32_t)((nblocks - entries + entries - 1) / entries);
    if (nblocks > 0) {
        uint32_t n_ext = ntables - 1, k;
        size_t t;
        ext = calloc(n_ext ? n_ext : 1, sizeof *ext);
        dblks = malloc(nblocks * sizeof *dblks);
        if ((n_ext && !ext) || !dblks) {
            free(ext); free(dblks); free(hdr);
            set_err(errbuf, errlen, "out of memory");
            return -1;
        }
        /* extension blocks first, then data (uniform order both variants;
         * placement is first-free either way and no reader cares) */
        for (k = 0; k < n_ext; k++)
            if (afs_alloc_block(v, &ext[k], errbuf, errlen) != 0) goto fail;
        for (t = 0; t < nblocks; t++)
            if (afs_alloc_block(v, &dblks[t], errbuf, errlen) != 0) goto fail;

        /* fill the tables REVERSED (file block j at index entries-1-j) */
        for (t = 0; t < ntables; t++) {
            uint8_t *tb;
            size_t first = t * entries;
            size_t count = nblocks - first;
            uint32_t tblk;
            uint32_t k2;
            if (count > entries) count = entries;
            if (t == 0) {
                tb = hdr;
                tblk = hdrblock;
            } else {
                tb = calloc(1, bs);
                if (!tb) { set_err(errbuf, errlen, "out of memory"); goto fail; }
                tblk = ext[t - 1];
                put_be32(tb + 0, AFS_T_LIST);
                put_be32(tb + 4, tblk);
                put_be32(tb + bs - 4, AFS_ST_FILE);
                put_be32(tb + bs - 12, hdrblock);
                put_be32(tb + bs - 8, (t + 1 < ntables) ? ext[t] : 0);
            }
            for (k2 = 0; k2 < count; k2++)
                put_be32(tb + 24 + 4 * (entries - 1 - k2), dblks[first + k2]);
            put_be32(tb + 8, (uint32_t)count);
            if (t == 0)
                put_be32(tb + bs - 8, n_ext ? ext[0] : 0);
            put_be32(tb + 20, afs_block_checksum(tb, bs, 20));
            if (afs_volume_write_block(v, tblk, tb, bs) != 0) {
                if (t != 0) free(tb);
                set_err(errbuf, errlen, "cannot write file table");
                goto fail;
            }
            if (t != 0) free(tb);
        }

        /* data blocks */
        {
            const uint8_t *src = (const uint8_t *)data;
            int is_ofs = afs_volume_is_ofs(v);
            for (t = 0; t < nblocks; t++) {
                uint8_t *db = calloc(1, bs);
                size_t want = len - t * payload;
                size_t have = want > payload ? payload : want;
                if (!db) { set_err(errbuf, errlen, "out of memory"); goto fail; }
                if (is_ofs) {
                    put_be32(db + 0, AFS_T_DATA);
                    put_be32(db + 4, hdrblock);
                    put_be32(db + 8, (uint32_t)(t + 1));
                    put_be32(db + 12, (uint32_t)have);
                    put_be32(db + 16, (t + 1 < nblocks) ? dblks[t + 1] : 0);
                    memcpy(db + 24, src + t * payload, have);
                    put_be32(db + 20, afs_block_checksum(db, bs, 20));
                } else {
                    memcpy(db, src + t * payload, have);
                }
                if (afs_volume_write_block(v, dblks[t], db, bs) != 0) {
                    free(db);
                    set_err(errbuf, errlen, "cannot write data block");
                    goto fail;
                }
                free(db);
            }
        }
        /* first_data mirrors ADFlib on both variants (the FFS reader ignores
         * it, but salvage tools and byte-level oracles may not) */
        put_be32(hdr + 16, dblks[0]);
    }

    afs_now_amiga(&dd, &mm, &tt);
    put_be32(hdr + bs - 188, (uint32_t)len);
    put_be32(hdr + bs - 92, dd);
    put_be32(hdr + bs - 88, mm);
    put_be32(hdr + bs - 84, tt);
    put_be32(hdr + 20, afs_block_checksum(hdr, bs, 20));
    if (afs_volume_write_block(v, hdrblock, hdr, bs) != 0) {
        set_err(errbuf, errlen, "cannot flush file header");
        goto fail;
    }
    {
        afs_dircache_rec_t rec;
        memset(&rec, 0, sizeof rec);
        rec.header = hdrblock;
        rec.size = (uint32_t)len;
        rec.days = (int16_t)dd; rec.mins = (int16_t)mm; rec.ticks = (int16_t)tt;
        rec.sec_type = (uint8_t)AFS_ST_FILE;
        {
            char nm[AFS_NAME_MAX];
            bstr(hdr + bs - 80, 31, nm, sizeof nm);
            memcpy(rec.name, nm, strlen(nm) < 30 ? strlen(nm) : 30);
        }
        if (afs_dircache_upsert(v, parent, &rec, errbuf, errlen) != 0) {
            /* upsert already named the reason in errbuf; keep it */
            goto fail;
        }
    }
    free(ext);
    free(dblks);
    free(hdr);
    return 0;

fail:
    /* No transactions in M13a: blocks allocated before an I/O failure stay
     * allocated but unreferenced. `afs verify` reports the volume as it is;
     * a failed create/write never leaves a half-linked entry behind, because
     * linking is the LAST structural step (create) and this function never
     * links at all. */
    free(ext);
    free(dblks);
    free(hdr);
    return -1;
}
