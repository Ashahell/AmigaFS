#include "amigafs/dir.h"
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

/* Read the name from a directory-entry block. The BCPL string is the length
 * byte at BSIZE-80 followed by up to 30 bytes at BSIZE-79. */
static void entry_name(const uint8_t *blk, size_t bsize, char *out, size_t cap) {
    bstr(blk + bsize - 80, 31, out, cap);
}

/* Follow a hash chain, invoking a callback for each entry block until the
 * chain ends. The walk is bounded by the volume's block count so a corrupt or
 * self-referential chain terminates instead of looping forever. */
typedef int (*entry_cb)(afs_volume_t *v, uint32_t block, void *ctx);

static int walk_chain(afs_volume_t *v, uint32_t first, entry_cb cb, void *ctx) {
    uint32_t bno = first;
    size_t bsize = afs_volume_block_size(v);
    uint32_t limit = afs_volume_block_count(v);
    size_t steps = 0;

    while (bno != 0) {
        /* bounds: refuse any pointer outside the volume rather than reading it */
        if (bno >= limit) return 0;      /* end the chain defensively */
        /* a chain longer than the volume cannot be legitimate */
        if (steps++ > limit) return 0;

        uint8_t *blk = malloc(bsize);
        if (!blk) return -1;
        if (afs_volume_read_block(v, bno, blk, bsize) != 0) {
            free(blk);
            return 0;                     /* unreadable: end the chain */
        }
        uint32_t next = be32(blk + bsize - 16);   /* hash_chain */
        int rc = cb(v, bno, ctx);
        free(blk);
        if (rc != 0) return rc;
        if (next == bno) return 0;        /* self-loop guard */
        bno = next;
    }
    return 0;
}

typedef struct { afs_dirent_t *out; size_t max; size_t *count; } list_ctx_t;

static int list_cb(afs_volume_t *v, uint32_t bno, void *ctx) {
    list_ctx_t *lc = (list_ctx_t *)ctx;
    if (*lc->count >= lc->max) return -1;   /* caller array is full */
    size_t bsize = afs_volume_block_size(v);

    uint8_t *blk = malloc(bsize);
    if (!blk) return -1;
    if (afs_volume_read_block(v, bno, blk, bsize) != 0) { free(blk); return 0; }

    afs_block_kind_t kind = afs_block_kind(blk, bsize);
    if (kind == AFS_BLOCK_UNKNOWN) { free(blk); return 0; }

    afs_dirent_t *e = &lc->out[*lc->count];
    e->block = bno;
    e->kind = (uint8_t)kind;
    entry_name(blk, bsize, e->name, sizeof e->name);
    (*lc->count)++;
    free(blk);
    return 0;
}

int afs_dir_list(afs_volume_t *v, uint32_t dirblock,
                 afs_dirent_t *out, size_t max, size_t *count) {
    if (!v || !out || !count) return -1;
    *count = 0;
    size_t bsize = afs_volume_block_size(v);

    uint8_t *dir = malloc(bsize);
    if (!dir) return -1;
    if (afs_volume_read_block(v, dirblock, dir, bsize) != 0) { free(dir); return -1; }

    /* Hash-table size: the ROOTBLOCK stores ht_size at offset 12, but a
     * DIRECTORY block does not -- offsets 8/12/16 are unused there and read as
     * 0. So we derive the table size from the block size whenever the stored
     * field is zero. Reading the field blindly makes every subdirectory on
     * every Amiga volume appear empty. */
    uint32_t ht_size = be32(dir + 12);
    if (ht_size == 0) ht_size = (uint32_t)afs_hash_table_size(bsize);
    if (ht_size == 0 || (size_t)ht_size * 4 + 24 > bsize) {
        /* bogus table: refuse rather than walking a wild hash table */
        free(dir);
        return -1;
    }

    list_ctx_t lc = { out, max, count };
    for (uint32_t i = 0; i < ht_size; i++) {
        uint32_t bno = be32(dir + 24 + 4 * i);
        if (bno == 0) continue;
        if (walk_chain(v, bno, list_cb, &lc) != 0) { free(dir); return -1; }
        if (*count >= max) break;
    }
    free(dir);
    return 0;
}

typedef struct { const char *name; size_t namelen; afs_dirent_t *out; int found; uint8_t flags; } lookup_ctx_t;

static int lookup_cb(afs_volume_t *v, uint32_t bno, void *ctx) {
    lookup_ctx_t *lc = (lookup_ctx_t *)ctx;
    size_t bsize = afs_volume_block_size(v);

    uint8_t *blk = malloc(bsize);
    if (!blk) return -1;
    if (afs_volume_read_block(v, bno, blk, bsize) != 0) { free(blk); return 0; }

    char nm[AFS_NAME_MAX];
    entry_name(blk, bsize, nm, sizeof nm);
    /* the Amiga compares names case-INsensitively via capitalch */
    if (afs_name_equal(nm, strlen(nm), lc->name, lc->namelen, lc->flags)) {
        lc->out->block = bno;
        lc->out->kind = (uint8_t)afs_block_kind(blk, bsize);
        entry_name(blk, bsize, lc->out->name, sizeof lc->out->name);
        lc->found = 1;
        free(blk);
        return -1;                        /* stop the walk */
    }
    free(blk);
    return 0;
}

int afs_dir_lookup(afs_volume_t *v, uint32_t dirblock,
                   const char *name, size_t namelen, afs_dirent_t *out) {
    if (!v || !name || !out) return -1;
    size_t bsize = afs_volume_block_size(v);

    uint8_t *dir = malloc(bsize);
    if (!dir) return -1;
    if (afs_volume_read_block(v, dirblock, dir, bsize) != 0) { free(dir); return -1; }

    /* see afs_dir_list: a directory block does not store ht_size, so derive it */
    uint32_t ht_size = be32(dir + 12);
    if (ht_size == 0) ht_size = (uint32_t)afs_hash_table_size(bsize);
    if (ht_size == 0 || (size_t)ht_size * 4 + 24 > bsize) { free(dir); return -1; }

    uint32_t bucket = afs_hash(name, namelen, ht_size, afs_volume_flags(v));
    uint32_t bno = be32(dir + 24 + 4 * bucket);

    lookup_ctx_t lc = { name, namelen, out, 0, afs_volume_flags(v) };
    if (bno != 0) walk_chain(v, bno, lookup_cb, &lc);
    free(dir);
    return lc.found ? 0 : -1;
}

/* --- D3 write phase --- */

/* Hash-table size of a directory block, by the same rule the readers use: the
 * rootblock stores it at +12, a subdirectory block does not (M3). */
static int dir_ht_size(const uint8_t *dir, size_t bs, uint32_t *out) {
    uint32_t ht = be32(dir + 12);
    if (ht == 0) ht = (uint32_t)afs_hash_table_size(bs);
    if (ht == 0 || (size_t)ht * 4 + 24 > bs) return -1;
    *out = ht;
    return 0;
}

/* Restamp a header-family block (checksum at +20) and write it back. */
static int write_header_block(afs_volume_t *v, uint32_t blk, uint8_t *b,
                              size_t bs) {
    put_be32(b + 20, afs_block_checksum(b, bs, 20));
    return afs_volume_write_block(v, blk, b, bs);
}

int afs_dir_link_entry(afs_volume_t *v, uint32_t dirblock, uint32_t entryblock,
                       char *errbuf, size_t errlen) {
    size_t bs;
    uint8_t *dir = NULL, *tail = NULL;
    uint32_t ht, bucket, slot, cur, prev = 0;
    uint32_t limit, steps = 0;
    char nm[AFS_NAME_MAX];
    int rc = -1;

    if (!v) { set_err(errbuf, errlen, "bad arguments"); return -1; }
    bs = afs_volume_block_size(v);
    limit = afs_volume_block_count(v);
    if (entryblock == 0 || entryblock >= limit) {
        set_err(errbuf, errlen, "entry block %u outside volume", entryblock);
        return -1;
    }
    dir = malloc(bs);
    tail = malloc(bs);
    if (!dir || !tail) { set_err(errbuf, errlen, "out of memory"); goto out; }
    if (afs_volume_read_block(v, dirblock, dir, bs) != 0) {
        set_err(errbuf, errlen, "cannot read directory %u", dirblock);
        goto out;
    }
    if (dir_ht_size(dir, bs, &ht) != 0) {
        set_err(errbuf, errlen, "directory %u has no usable hash table", dirblock);
        goto out;
    }
    /* name the entry for error messages from its own block */
    nm[0] = '\0';
    {
        uint8_t *eb = malloc(bs);
        if (eb) {
            if (afs_volume_read_block(v, entryblock, eb, bs) == 0)
                entry_name(eb, bs, nm, sizeof nm);
            free(eb);
        }
    }
    bucket = afs_hash(nm, strlen(nm), ht, afs_volume_flags(v));
    slot = be32(dir + 24 + 4 * bucket);
    if (slot == 0) {
        put_be32(dir + 24 + 4 * bucket, entryblock);
        if (write_header_block(v, dirblock, dir, bs) != 0) {
            set_err(errbuf, errlen, "cannot write directory %u", dirblock);
            goto out;
        }
        rc = 0;
        goto out;
    }
    /* walk to the tail STRICTLY: every pointer must be in range and readable.
     * The read path ends a corrupt chain defensively; here that would bake
     * corruption into a volume we are improving. */
    cur = slot;
    for (;;) {
        uint32_t next;
        if (cur >= limit || steps++ > limit) {
            set_err(errbuf, errlen, "hash chain corrupt in directory %u", dirblock);
            goto out;
        }
        if (afs_volume_read_block(v, cur, tail, bs) != 0) {
            set_err(errbuf, errlen, "hash chain corrupt in directory %u", dirblock);
            goto out;
        }
        next = be32(tail + bs - 16);
        if (next == 0) { prev = cur; break; }
        if (next == cur) {
            set_err(errbuf, errlen, "hash chain corrupt in directory %u", dirblock);
            goto out;
        }
        cur = next;
    }
    put_be32(tail + bs - 16, entryblock);
    if (write_header_block(v, prev, tail, bs) != 0) {
        set_err(errbuf, errlen, "cannot link into directory %u", dirblock);
        goto out;
    }
    rc = 0;
out:
    free(dir);
    free(tail);
    return rc;
}

/* Byte size of one cache record with the given name/comment lengths, by the
 * same formula the parser walks (blocktypes.c): 24 header bytes + name +
 * comment-length byte + comment, +1 pad when inside the block, word-aligned. */
static size_t dircache_rec_size(size_t namelen, size_t commentlen, size_t bs,
                                size_t off) {
    size_t next = off + 24 + namelen + commentlen;
    if (next < bs) next++;
    if (next & 1) next++;
    return next - off;
}

/* Offset one past the last record in a cache block, walking with the parser's
 * formula. Returns 0 when the block does not parse as a cache block. */
static size_t dircache_end(const uint8_t *blk, size_t bs) {
    uint32_t nb, i;
    size_t off = 24;
    if (afs_block_kind(blk, bs) != AFS_BLOCK_DIRCACHE) return 0;
    nb = be32(blk + 12);
    for (i = 0; i < nb; i++) {
        size_t nl, cl, cl_off, next;
        if (off + 26 > bs) return 0;
        nl = blk[off + 23];
        if (nl > 30) return 0;
        cl_off = off + 24 + nl;
        if (cl_off >= bs) return 0;
        cl = blk[cl_off];
        if (cl > 80) return 0;
        next = cl_off + cl;
        if (next < bs) next++;
        if (next & 1) next++;
        if (next <= off || next > bs) return 0;
        off = next;
    }
    return off;
}

/* Find a record for `header` in a cache block; returns its offset, or 0 when
 * absent (offsets are never 0: records start at byte 24). */
static size_t dircache_find(const uint8_t *blk, size_t bs, uint32_t header) {
    uint32_t nb, i;
    size_t off = 24;
    if (afs_block_kind(blk, bs) != AFS_BLOCK_DIRCACHE) return 0;
    nb = be32(blk + 12);
    for (i = 0; i < nb; i++) {
        size_t nl, cl, cl_off, next;
        if (off + 26 > bs) return 0;
        if (be32(blk + off) == header) return off;
        nl = blk[off + 23];
        if (nl > 30) return 0;
        cl_off = off + 24 + nl;
        if (cl_off >= bs) return 0;
        cl = blk[cl_off];
        if (cl > 80) return 0;
        next = cl_off + cl;
        if (next < bs) next++;
        if (next & 1) next++;
        if (next <= off || next > bs) return 0;
        off = next;
    }
    return 0;
}

static void dircache_write_rec(uint8_t *at, const afs_dircache_rec_t *r) {
    /* bounded length without POSIX strnlen (strict C99): the name field is
     * NUL-terminated by both the parser and our own constructors. */
    size_t nl = 0;
    while (nl < 30 && r->name[nl]) nl++;
    put_be32(at + 0, r->header);
    put_be32(at + 4, r->size);
    put_be32(at + 8, r->protect);
    at[12] = (uint8_t)(r->uid >> 8); at[13] = (uint8_t)r->uid;
    at[14] = (uint8_t)(r->gid >> 8); at[15] = (uint8_t)r->gid;
    at[16] = (uint8_t)(r->days >> 8); at[17] = (uint8_t)r->days;
    at[18] = (uint8_t)(r->mins >> 8); at[19] = (uint8_t)r->mins;
    at[20] = (uint8_t)(r->ticks >> 8); at[21] = (uint8_t)r->ticks;
    at[22] = r->sec_type;
    at[23] = (uint8_t)nl;
    memcpy(at + 24, r->name, nl);
    /* our writes carry no comment: length 0 at the comment slot */
    at[24 + nl] = 0;
}

int afs_dircache_upsert(afs_volume_t *v, uint32_t dirblk,
                        const afs_dircache_rec_t *rec,
                        char *errbuf, size_t errlen) {
    size_t bs;
    uint8_t *dir = NULL, *cb = NULL;
    uint32_t cur, steps = 0;
    uint32_t limit;
    int rc = -1;

    if (!v || !rec) { set_err(errbuf, errlen, "bad arguments"); return -1; }
    if (!afs_volume_has_dircache(v)) return 0;   /* no cache: nothing to do */
    bs = afs_volume_block_size(v);
    limit = afs_volume_block_count(v);
    dir = malloc(bs);
    cb = malloc(bs);
    if (!dir || !cb) { set_err(errbuf, errlen, "out of memory"); goto out; }
    if (afs_volume_read_block(v, dirblk, dir, bs) != 0) {
        set_err(errbuf, errlen, "cannot read directory %u", dirblk);
        goto out;
    }
    cur = be32(dir + bs - 8);   /* first cache block: the extension field */
    if (cur == 0) {
        /* no cache yet: allocate one and link it */
        uint32_t nb;
        if (afs_alloc_block(v, &nb, errbuf, errlen) != 0) goto out;
        memset(cb, 0, bs);
        put_be32(cb + 0, AFS_T_DIRCACHE);
        put_be32(cb + 4, nb);
        put_be32(cb + 8, dirblk);
        put_be32(cb + 12, 0);
        put_be32(cb + 16, 0);
        dircache_write_rec(cb + 24, rec);
        put_be32(cb + 12, 1);
        put_be32(cb + 20, afs_block_checksum(cb, bs, 20));
        if (afs_volume_write_block(v, nb, cb, bs) != 0) {
            set_err(errbuf, errlen, "cannot write cache block");
            goto out;
        }
        put_be32(dir + bs - 8, nb);
        if (write_header_block(v, dirblk, dir, bs) != 0) {
            set_err(errbuf, errlen, "cannot link cache block");
            goto out;
        }
        rc = 0;
        goto out;
    }
    /* walk to the last cache block */
    for (;;) {
        uint32_t next;
        if (cur == 0 || cur >= limit || steps++ > 64) {
            set_err(errbuf, errlen, "dir-cache chain corrupt");
            goto out;
        }
        if (afs_volume_read_block(v, cur, cb, bs) != 0) {
            set_err(errbuf, errlen, "dir-cache chain corrupt");
            goto out;
        }
        if (afs_block_kind(cb, bs) != AFS_BLOCK_DIRCACHE) {
            set_err(errbuf, errlen, "dir-cache chain corrupt");
            goto out;
        }
        next = be32(cb + 16);
        if (next == 0) break;
        cur = next;
    }
    {
        size_t at = dircache_find(cb, bs, rec->header);
        if (at) {
            /* patch size + timestamp in place; comments and unknown bytes
             * survive byte-for-byte (m11's commented entries live here) */
            put_be32(cb + at + 4, rec->size);
            cb[at + 16] = (uint8_t)(rec->days >> 8);
            cb[at + 17] = (uint8_t)rec->days;
            cb[at + 18] = (uint8_t)(rec->mins >> 8);
            cb[at + 19] = (uint8_t)rec->mins;
            cb[at + 20] = (uint8_t)(rec->ticks >> 8);
            cb[at + 21] = (uint8_t)rec->ticks;
            put_be32(cb + 20, afs_block_checksum(cb, bs, 20));
            if (afs_volume_write_block(v, cur, cb, bs) != 0) {
                set_err(errbuf, errlen, "cannot refresh cache block");
                goto out;
            }
            rc = 0;
            goto out;
        }
    }
    {
        size_t nl = 0, need, end = dircache_end(cb, bs);
        uint32_t nb = be32(cb + 12);
        while (nl < 30 && rec->name[nl]) nl++;
        need = dircache_rec_size(nl, 0, bs, end);
        /* records_nb is untrusted on read; on write we are the author, and a
         * block that would exceed the parser's clamp ((bsize-24)/26, 64 max)
         * must chain instead of growing silently-unreadable records */
        if (end && end + need <= bs && nb + 1 <= 64 &&
            nb + 1 <= (bs - 24) / 26) {
            dircache_write_rec(cb + end, rec);
            put_be32(cb + 12, nb + 1);
            put_be32(cb + 20, afs_block_checksum(cb, bs, 20));
            if (afs_volume_write_block(v, cur, cb, bs) != 0) {
                set_err(errbuf, errlen, "cannot append cache record");
                goto out;
            }
            rc = 0;
            goto out;
        }
    }
    /* last block is full: allocate, link, write */
    {
        uint32_t nb2;
        if (afs_alloc_block(v, &nb2, errbuf, errlen) != 0) goto out;
        memset(cb, 0, bs);
        put_be32(cb + 0, AFS_T_DIRCACHE);
        put_be32(cb + 4, nb2);
        put_be32(cb + 8, dirblk);
        put_be32(cb + 12, 1);
        put_be32(cb + 16, 0);
        dircache_write_rec(cb + 24, rec);
        put_be32(cb + 20, afs_block_checksum(cb, bs, 20));
        if (afs_volume_write_block(v, nb2, cb, bs) != 0) {
            set_err(errbuf, errlen, "cannot write cache block");
            goto out;
        }
        /* link: re-read is unnecessary, `cb` still holds the prev block?
         * No -- cb was just overwritten. Re-read prev... `cur` IS the prev
         * (loop broke with next == 0), so re-read it. */
        if (afs_volume_read_block(v, cur, cb, bs) != 0) {
            set_err(errbuf, errlen, "cannot relink cache chain");
            goto out;
        }
        put_be32(cb + 16, nb2);
        put_be32(cb + 20, afs_block_checksum(cb, bs, 20));
        if (afs_volume_write_block(v, cur, cb, bs) != 0) {
            set_err(errbuf, errlen, "cannot relink cache chain");
            goto out;
        }
        rc = 0;
    }
out:
    free(dir);
    free(cb);
    return rc;
}

int afs_dir_mkdir(afs_volume_t *v, uint32_t dirblock,
                  const char *name, size_t namelen, uint32_t *dir_out,
                  char *errbuf, size_t errlen) {
    size_t bs;
    uint8_t *b = NULL;
    uint32_t nb;
    afs_dirent_t dup;
    afs_dircache_rec_t rec;
    uint32_t dd, mm, tt;

    if (!v || !name || !dir_out) {
        set_err(errbuf, errlen, "bad arguments");
        return -1;
    }
    if (!afs_name_valid(name, namelen)) {
        set_err(errbuf, errlen, "invalid name (1..30 bytes, no '/' or ':')");
        return -1;
    }
    /* duplicate by the same lookup a later `ls` will use (INTL folds here) */
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
    /* +8 high_seq 0, +12 ht_size 0 (subdirs derive it: M3), +16 data_size 0 */
    /* hash table area stays zeroed */
    put_be16(b + bs - 196, 0);
    put_be16(b + bs - 194, 0);
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
    put_be32(b + bs - 8, 0);            /* no cache until first child */
    put_be32(b + bs - 4, AFS_ST_USERDIR);
    put_be32(b + 20, afs_block_checksum(b, bs, 20));
    if (afs_volume_write_block(v, nb, b, bs) != 0) {
        free(b);
        set_err(errbuf, errlen, "cannot write directory block");
        return -1;
    }
    free(b);
    if (afs_dir_link_entry(v, dirblock, nb, errbuf, errlen) != 0) return -1;

    memset(&rec, 0, sizeof rec);
    rec.header = nb;
    rec.size = 0;
    rec.days = (int16_t)dd; rec.mins = (int16_t)mm; rec.ticks = (int16_t)tt;
    rec.sec_type = (uint8_t)AFS_ST_USERDIR;
    memcpy(rec.name, name, namelen < 30 ? namelen : 30);
    if (afs_dircache_upsert(v, dirblock, &rec, errbuf, errlen) != 0) return -1;

    *dir_out = nb;
    return 0;
}