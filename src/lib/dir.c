#include "amigafs/dir.h"
#include "amigafs/alloc.h"
#include "amigafs/be.h"
#include "amigafs/checksum.h"
#include "amigafs/hash.h"
#include "amigafs/links.h"
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

static void dircache_write_rec(uint8_t *at, const afs_dircache_rec_t *r,
                               const char *comment, size_t commentlen) {
    /* bounded length without POSIX strnlen (strict C99): the name field is
     * NUL-terminated by both the parser and our own constructors. */
    size_t nl = 0;
    while (nl < 30 && r->name[nl]) nl++;
    if (commentlen > 79) commentlen = 79;   /* format maximum (m11) */
    if (!comment) commentlen = 0;
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
    at[24 + nl] = (uint8_t)commentlen;
    if (commentlen) memcpy(at + 25 + nl, comment, commentlen);
}

int afs_dircache_upsert(afs_volume_t *v, uint32_t dirblk,
                        const afs_dircache_rec_t *rec,
                        const char *comment, size_t commentlen,
                        char *errbuf, size_t errlen) {
    size_t bs;
    uint8_t *dir = NULL, *cb = NULL;
    uint32_t cur;
    uint32_t limit;
    size_t cl;
    int rc = -1;

    if (!v || !rec) { set_err(errbuf, errlen, "bad arguments"); return -1; }
    if (!afs_volume_has_dircache(v)) return 0;   /* no cache: nothing to do */
    bs = afs_volume_block_size(v);
    limit = afs_volume_block_count(v);
    cl = (comment && commentlen) ? commentlen : 0;
    if (cl > 79) cl = 79;   /* format maximum (m11) */
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
        dircache_write_rec(cb + 24, rec, comment, cl);
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
    /* drop any record for this header first (upsert, not append-blindly),
     * then append to the last block -- or chain a new one when full */
    {
        uint32_t w = cur, ws = 0;
        for (;;) {
            size_t at;
            if (w == 0 || w >= limit || ws++ > 64) {
                set_err(errbuf, errlen, "dir-cache chain corrupt");
                goto out;
            }
            if (afs_volume_read_block(v, w, cb, bs) != 0 ||
                afs_block_kind(cb, bs) != AFS_BLOCK_DIRCACHE) {
                set_err(errbuf, errlen, "dir-cache chain corrupt");
                goto out;
            }
            at = dircache_find(cb, bs, rec->header);
            if (at) {
                /* compact the tail down over it. Bounds first, bytes second:
                 * `at` names a matching header, not a well-formed record, so
                 * every step below is clamped to the block before it reads. */
                size_t onl = 0, ocl = 0, ocl_off, osz, rsz, tail;
                while (onl < 30 && at + 24 + onl < bs && cb[at + 24 + onl])
                    onl++;
                ocl_off = at + 24 + onl;
                if (ocl_off < bs) {
                    ocl = cb[ocl_off];
                    if (ocl > 80) ocl = 0;
                }
                osz = dircache_rec_size(onl, ocl, bs, at);
                rsz = (at + osz <= bs) ? osz : bs - at;
                tail = at + rsz;
                if (tail < bs)
                    memmove(cb + at, cb + tail, bs - tail);
                /* stale bytes always occupy the last rsz bytes now -- whether
                 * the tail moved down or the dropped record already ran to
                 * the end of the block */
                memset(cb + bs - rsz, 0, rsz);
                {
                    uint32_t nb = be32(cb + 12);
                    put_be32(cb + 12, nb > 0 ? nb - 1 : 0);
                }
                put_be32(cb + 20, afs_block_checksum(cb, bs, 20));
                if (afs_volume_write_block(v, w, cb, bs) != 0) {
                    set_err(errbuf, errlen, "cannot drop cache record");
                    goto out;
                }
            }
            if (be32(cb + 16) == 0) { cur = w; break; }
            w = be32(cb + 16);
        }
    }
    /* re-read the last block (the loop above leaves `cb` holding some block,
     * not necessarily the last) and append */
    if (afs_volume_read_block(v, cur, cb, bs) != 0) {
        set_err(errbuf, errlen, "dir-cache chain corrupt");
        goto out;
    }
    {
        size_t nl = 0, need, end = dircache_end(cb, bs);
        uint32_t nb = be32(cb + 12);
        while (nl < 30 && rec->name[nl]) nl++;
        need = dircache_rec_size(nl, cl, bs, end);
        /* records_nb is untrusted on read; on write we are the author, and a
         * block that would exceed the parser's clamp ((bsize-24)/26, 64 max)
         * must chain instead of growing silently-unreadable records */
        if (end && end + need <= bs && nb + 1 <= 64 &&
            nb + 1 <= (bs - 24) / 26) {
            dircache_write_rec(cb + end, rec, comment, cl);
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
        dircache_write_rec(cb + 24, rec, comment, cl);
        put_be32(cb + 20, afs_block_checksum(cb, bs, 20));
        if (afs_volume_write_block(v, nb2, cb, bs) != 0) {
            set_err(errbuf, errlen, "cannot write cache block");
            goto out;
        }
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
    if (afs_dircache_upsert(v, dirblock, &rec, NULL, 0, errbuf, errlen) != 0) return -1;

    *dir_out = nb;
    return 0;
}

int afs_dir_unlink(afs_volume_t *v, uint32_t dirblock,
                   const char *name, size_t namelen, uint32_t *block_out,
                   char *errbuf, size_t errlen) {
    size_t bs;
    uint8_t *dir = NULL, *prevb = NULL;
    uint32_t ht, bucket, slot, cur, prev = 0;
    uint32_t limit, steps = 0;
    int rc = -1;

    if (!v || !name || !block_out) {
        set_err(errbuf, errlen, "bad arguments");
        return -1;
    }
    bs = afs_volume_block_size(v);
    limit = afs_volume_block_count(v);
    dir = malloc(bs);
    prevb = malloc(bs);
    if (!dir || !prevb) { set_err(errbuf, errlen, "out of memory"); goto out; }
    if (afs_volume_read_block(v, dirblock, dir, bs) != 0) {
        set_err(errbuf, errlen, "cannot read directory %u", dirblock);
        goto out;
    }
    if (dir_ht_size(dir, bs, &ht) != 0) {
        set_err(errbuf, errlen, "directory %u has no usable hash table", dirblock);
        goto out;
    }
    bucket = afs_hash(name, namelen, ht, afs_volume_flags(v));
    slot = be32(dir + 24 + 4 * bucket);
    /* strict walk with predecessor: every pointer must be in range and
     * readable, like the link path -- unlinking around corruption would hide
     * it behind a successful return */
    cur = slot;
    for (;;) {
        uint8_t *eb;
        char nm[AFS_NAME_MAX];
        uint32_t next;
        if (cur == 0) {
            set_err(errbuf, errlen, "'%.*s' not found", (int)namelen, name);
            goto out;
        }
        if (cur >= limit || steps++ > limit) {
            set_err(errbuf, errlen, "hash chain corrupt in directory %u", dirblock);
            goto out;
        }
        eb = malloc(bs);
        if (!eb) { set_err(errbuf, errlen, "out of memory"); goto out; }
        if (afs_volume_read_block(v, cur, eb, bs) != 0) {
            free(eb);
            set_err(errbuf, errlen, "hash chain corrupt in directory %u", dirblock);
            goto out;
        }
        entry_name(eb, bs, nm, sizeof nm);
        next = be32(eb + bs - 16);
        free(eb);
        if (afs_name_equal(nm, strlen(nm), name, namelen, afs_volume_flags(v)))
            break;
        if (next == cur) {
            set_err(errbuf, errlen, "hash chain corrupt in directory %u", dirblock);
            goto out;
        }
        prev = cur;
        cur = next;
    }
    /* splice out: bucket head or predecessor link, then restamp */
    if (prev == 0) {
        uint8_t *eb = malloc(bs);
        uint32_t next;
        if (!eb) { set_err(errbuf, errlen, "out of memory"); goto out; }
        if (afs_volume_read_block(v, cur, eb, bs) != 0) {
            free(eb);
            set_err(errbuf, errlen, "cannot read entry %u", cur);
            goto out;
        }
        next = be32(eb + bs - 16);
        free(eb);
        put_be32(dir + 24 + 4 * bucket, next);
        if (write_header_block(v, dirblock, dir, bs) != 0) {
            set_err(errbuf, errlen, "cannot write directory %u", dirblock);
            goto out;
        }
    } else {
        if (afs_volume_read_block(v, prev, prevb, bs) != 0) {
            set_err(errbuf, errlen, "cannot read chain block %u", prev);
            goto out;
        }
        {
            uint8_t *eb = malloc(bs);
            uint32_t next;
            if (!eb) { set_err(errbuf, errlen, "out of memory"); goto out; }
            if (afs_volume_read_block(v, cur, eb, bs) != 0) {
                free(eb);
                set_err(errbuf, errlen, "cannot read entry %u", cur);
                goto out;
            }
            next = be32(eb + bs - 16);
            free(eb);
            put_be32(prevb + bs - 16, next);
        }
        if (write_header_block(v, prev, prevb, bs) != 0) {
            set_err(errbuf, errlen, "cannot relink chain in %u", dirblock);
            goto out;
        }
    }
    /* Reaching here means the entry WAS spliced out (every failure `goto out`s),
     * so a hard link must also leave its real entry's next_link chain or it
     * keeps counting towards nlink and `ls -l` over-reports (M24). */
    afs_link_after_unlink(v, cur);
    *block_out = cur;
    rc = 0;
out:
    free(dir);
    free(prevb);
    return rc;
}

int afs_dircache_del(afs_volume_t *v, uint32_t dirblock, uint32_t header,
                     char *errbuf, size_t errlen) {
    size_t bs;
    uint8_t *dir = NULL, *cb = NULL;
    uint32_t cur, steps = 0, limit;

    if (!v) { set_err(errbuf, errlen, "bad arguments"); return -1; }
    if (!afs_volume_has_dircache(v)) return 0;
    bs = afs_volume_block_size(v);
    limit = afs_volume_block_count(v);
    dir = malloc(bs);
    cb = malloc(bs);
    if (!dir || !cb) { set_err(errbuf, errlen, "out of memory"); goto out; }
    if (afs_volume_read_block(v, dirblock, dir, bs) != 0) {
        set_err(errbuf, errlen, "cannot read directory %u", dirblock);
        goto out;
    }
    cur = be32(dir + bs - 8);
    while (cur != 0) {
        size_t at, onl = 0, ocl = 0, ocl_off, osz, rsz, tail;
        uint32_t nb;
        if (cur >= limit || steps++ > 64) {
            set_err(errbuf, errlen, "dir-cache chain corrupt");
            goto out;
        }
        if (afs_volume_read_block(v, cur, cb, bs) != 0 ||
            afs_block_kind(cb, bs) != AFS_BLOCK_DIRCACHE) {
            set_err(errbuf, errlen, "dir-cache chain corrupt");
            goto out;
        }
        at = dircache_find(cb, bs, header);
        if (at) {
            /* same bounded compaction as upsert's drop path */
            while (onl < 30 && at + 24 + onl < bs && cb[at + 24 + onl]) onl++;
            ocl_off = at + 24 + onl;
            if (ocl_off < bs) {
                ocl = cb[ocl_off];
                if (ocl > 80) ocl = 0;
            }
            osz = dircache_rec_size(onl, ocl, bs, at);
            rsz = (at + osz <= bs) ? osz : bs - at;
            tail = at + rsz;
            if (tail < bs) {
                memmove(cb + at, cb + tail, bs - tail);
                memset(cb + bs - rsz, 0, rsz);
            }
            nb = be32(cb + 12);
            put_be32(cb + 12, nb > 0 ? nb - 1 : 0);
            put_be32(cb + 20, afs_block_checksum(cb, bs, 20));
            if (afs_volume_write_block(v, cur, cb, bs) != 0) {
                set_err(errbuf, errlen, "cannot drop cache record");
                goto out;
            }
            free(dir);
            free(cb);
            return 0;
        }
        cur = be32(cb + 16);
    }
    free(dir);
    free(cb);
    return 0;   /* no record named that header: nothing to do */
out:
    free(dir);
    free(cb);
    return -1;
}

/* Read one cache record's comment bytes (for rename preservation). Empty when
 * absent. Bounded like every other cache walk. */
static size_t dircache_read_comment(afs_volume_t *v, uint32_t dirblock,
                                    uint32_t header, char *out, size_t cap) {
    size_t bs = afs_volume_block_size(v);
    uint32_t limit = afs_volume_block_count(v);
    uint8_t *dir = NULL, *cb = NULL;
    uint32_t cur, steps = 0;
    size_t got = 0;

    if (!v || !out || cap == 0) return 0;
    out[0] = '\0';
    if (!afs_volume_has_dircache(v)) return 0;
    dir = malloc(bs);
    cb = malloc(bs);
    if (!dir || !cb) { free(dir); free(cb); return 0; }
    if (afs_volume_read_block(v, dirblock, dir, bs) != 0) goto done;
    cur = be32(dir + bs - 8);
    while (cur != 0) {
        size_t at, onl = 0, ocl_off, cl;
        if (cur >= limit || steps++ > 64) break;
        if (afs_volume_read_block(v, cur, cb, bs) != 0) break;
        if (afs_block_kind(cb, bs) != AFS_BLOCK_DIRCACHE) break;
        at = dircache_find(cb, bs, header);
        if (at) {
            while (onl < 30 && at + 24 + onl < bs && cb[at + 24 + onl]) onl++;
            ocl_off = at + 24 + onl;
            if (ocl_off >= bs) break;
            cl = cb[ocl_off];
            if (cl > 79) cl = 79;
            if (ocl_off + 1 + cl > bs) cl = (ocl_off + 1 < bs) ? bs - ocl_off - 1 : 0;
            if (cl > cap - 1) cl = cap - 1;
            if (cl) memcpy(out, cb + ocl_off + 1, cl);
            out[cl] = '\0';
            got = cl;
            break;
        }
        cur = be32(cb + 16);
    }
done:
    free(dir);
    free(cb);
    return got;
}

int afs_dir_rmdir(afs_volume_t *v, uint32_t dirblock,
                  const char *name, size_t namelen,
                  char *errbuf, size_t errlen) {
    size_t bs;
    uint8_t *b = NULL;
    afs_dirent_t e;
    uint32_t target, cur, steps = 0, limit;
    afs_dirent_t ents[8];
    size_t n = 0;

    if (!v || !name) { set_err(errbuf, errlen, "bad arguments"); return -1; }
    if (afs_dir_lookup(v, dirblock, name, namelen, &e) != 0) {
        set_err(errbuf, errlen, "'%.*s' not found", (int)namelen, name);
        return -1;
    }
    if (e.kind != AFS_BLOCK_DIR) {
        set_err(errbuf, errlen, "'%.*s' is not a directory",
                (int)namelen, name);
        return -1;
    }
    target = e.block;
    /* empty means empty: even one entry refuses (ADFlib parity) */
    if (afs_dir_list(v, target, ents, 8, &n) != 0) {
        set_err(errbuf, errlen, "cannot read directory '%.*s'",
                (int)namelen, name);
        return -1;
    }
    if (n != 0) {
        set_err(errbuf, errlen, "directory '%.*s' not empty",
                (int)namelen, name);
        return -1;
    }
    {
        uint32_t gone = 0;
        if (afs_dir_unlink(v, dirblock, name, namelen, &gone,
                           errbuf, errlen) != 0)
            return -1;
        if (gone != target) {
            set_err(errbuf, errlen, "unlink raced lookup (internal)");
            return -1;
        }
    }
    /* free the whole cache chain (empty dirs can own blocks from deleted
     * children), then the directory block itself */
    bs = afs_volume_block_size(v);
    limit = afs_volume_block_count(v);
    b = malloc(bs);
    if (!b) { set_err(errbuf, errlen, "out of memory"); return -1; }
    if (afs_volume_read_block(v, target, b, bs) != 0) {
        free(b);
        set_err(errbuf, errlen, "cannot read directory block %u", target);
        return -1;
    }
    cur = be32(b + bs - 8);
    while (cur != 0) {
        uint8_t *cb = malloc(bs);
        uint32_t next;
        if (!cb) { free(b); set_err(errbuf, errlen, "out of memory"); return -1; }
        if (cur >= limit || steps++ > 64 ||
            afs_volume_read_block(v, cur, cb, bs) != 0 ||
            afs_block_kind(cb, bs) != AFS_BLOCK_DIRCACHE) {
            free(cb); free(b);
            set_err(errbuf, errlen, "dir-cache chain corrupt");
            return -1;
        }
        next = be32(cb + 16);
        free(cb);
        if (afs_free_block(v, cur, errbuf, errlen) != 0) {
            free(b);
            return -1;
        }
        cur = next;
    }
    free(b);
    if (afs_dircache_del(v, dirblock, target, errbuf, errlen) != 0) return -1;
    if (afs_free_block(v, target, errbuf, errlen) != 0) return -1;
    return 0;
}

/* True when `blk` lies on the ancestor chain of `dirblk` (or equals it):
 * moving `blk` under `dirblk` would cycle. Bounded walk via parent fields. */
static int is_within_subtree(afs_volume_t *v, uint32_t blk, uint32_t dirblk) {
    size_t bs = afs_volume_block_size(v);
    uint32_t limit = afs_volume_block_count(v);
    uint32_t cur = dirblk, steps = 0;
    uint8_t *b = malloc(bs);
    if (!b) return 1;   /* fail closed: refuse the move on OOM */
    while (cur != 0) {
        if (cur == blk) { free(b); return 1; }
        if (cur >= limit || steps++ > limit) break;
        if (afs_volume_read_block(v, cur, b, bs) != 0) break;
        /* rootblock's parent field is 0: terminates at the top */
        cur = be32(b + bs - 12);
    }
    free(b);
    return 0;
}

int afs_dir_rename(afs_volume_t *v,
                   uint32_t src_dir, const char *oldname, size_t oldlen,
                   uint32_t dst_dir, const char *newname, size_t newlen,
                   char *errbuf, size_t errlen) {
    size_t bs;
    uint8_t *eb = NULL;
    afs_dirent_t src, dst_dup;
    afs_block_kind_t kind;
    uint32_t target;
    char comment[80];

    if (!v || !oldname || !newname) {
        set_err(errbuf, errlen, "bad arguments");
        return -1;
    }
    if (!afs_name_valid(newname, newlen)) {
        set_err(errbuf, errlen, "invalid name (1..30 bytes, no '/' or ':')");
        return -1;
    }
    bs = afs_volume_block_size(v);
    if (afs_dir_lookup(v, src_dir, oldname, oldlen, &src) != 0) {
        set_err(errbuf, errlen, "'%.*s' not found", (int)oldlen, oldname);
        return -1;
    }
    target = src.block;
    kind = (afs_block_kind_t)src.kind;
    if (kind != AFS_BLOCK_FILE && kind != AFS_BLOCK_DIR) {
        set_err(errbuf, errlen, "cannot move links yet (only files/dirs)");
        return -1;
    }
    /* same name in the same directory: no-op success (ADFlib parity) */
    if (src_dir == dst_dir &&
        afs_name_equal(oldname, oldlen, newname, newlen,
                       afs_volume_flags(v)))
        return 0;
    /* occupied destination: refuse with the victim untouched */
    if (afs_dir_lookup(v, dst_dir, newname, newlen, &dst_dup) == 0) {
        set_err(errbuf, errlen, "'%.*s' already exists", (int)newlen, newname);
        return -1;
    }
    /* a directory moved under itself would cycle */
    if (kind == AFS_BLOCK_DIR && is_within_subtree(v, target, dst_dir)) {
        set_err(errbuf, errlen, "cannot move a directory into itself");
        return -1;
    }
    /* preserve any comment across the move (xdftool-written files carry them) */
    comment[0] = '\0';
    dircache_read_comment(v, src_dir, target, comment, sizeof comment);

    eb = malloc(bs);
    if (!eb) { set_err(errbuf, errlen, "out of memory"); return -1; }
    if (afs_volume_read_block(v, target, eb, bs) != 0) {
        free(eb);
        set_err(errbuf, errlen, "cannot read entry %u", target);
        return -1;
    }
    /* rewrite name + parent in place, then rechain */
    eb[bs - 80] = (uint8_t)newlen;
    memcpy(eb + bs - 79, newname, newlen);
    put_be32(eb + bs - 12, dst_dir);
    put_be32(eb + bs - 16, 0);
    put_be32(eb + 20, afs_block_checksum(eb, bs, 20));
    {
        uint32_t gone = 0;
        if (afs_dir_unlink(v, src_dir, oldname, oldlen, &gone,
                           errbuf, errlen) != 0) {
            free(eb);
            return -1;
        }
        if (gone != target) {
            free(eb);
            set_err(errbuf, errlen, "unlink raced lookup (internal)");
            return -1;
        }
    }
    if (afs_volume_write_block(v, target, eb, bs) != 0) {
        /* entry is unlinked but unwritten: the name is gone while the block
         * still carries the old one. No transaction spans this; report it. */
        free(eb);
        set_err(errbuf, errlen, "cannot rewrite entry %u", target);
        return -1;
    }
    free(eb);
    if (afs_dir_link_entry(v, dst_dir, target, errbuf, errlen) != 0) return -1;
    {
        afs_dircache_rec_t rec;
        uint8_t *hb = malloc(bs);
        memset(&rec, 0, sizeof rec);
        if (!hb) { set_err(errbuf, errlen, "out of memory"); return -1; }
        /* size + timestamp travel with the entry */
        if (afs_volume_read_block(v, target, hb, bs) != 0) {
            free(hb);
            set_err(errbuf, errlen, "cannot reread entry %u", target);
            return -1;
        }
        rec.header = target;
        rec.size = (kind == AFS_BLOCK_FILE) ? be32(hb + bs - 188) : 0;
        rec.protect = be32(hb + bs - 192);
        rec.uid = be16(hb + bs - 196);
        rec.gid = be16(hb + bs - 194);
        rec.days = (int16_t)be16(hb + bs - 92);
        rec.mins = (int16_t)be16(hb + bs - 88);
        rec.ticks = (int16_t)be16(hb + bs - 84);
        rec.sec_type = (kind == AFS_BLOCK_FILE) ? (uint8_t)AFS_ST_FILE
                                                : (uint8_t)AFS_ST_USERDIR;
        memcpy(rec.name, newname, newlen < 30 ? newlen : 30);
        free(hb);
        if (afs_dircache_del(v, src_dir, target, errbuf, errlen) != 0) return -1;
        if (afs_dircache_upsert(v, dst_dir, &rec, comment[0] ? comment : NULL,
                                comment[0] ? strlen(comment) : 0,
                                errbuf, errlen) != 0)
            return -1;
    }
    return 0;
}