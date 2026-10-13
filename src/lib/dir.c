#include "amigafs/dir.h"
#include "amigafs/be.h"
#include "amigafs/hash.h"

#include <stdlib.h>
#include <string.h>

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

    /* the rootblock and directory blocks share the T_HEADER layout with the
     * hash table at offset 24 and ht_size at offset 12 */
    uint32_t ht_size = be32(dir + 12);
    if (ht_size == 0 || (size_t)ht_size * 4 + 24 > bsize) {
        /* bogus ht_size: refuse rather than walking a wild table */
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

    uint32_t ht_size = be32(dir + 12);
    if (ht_size == 0 || (size_t)ht_size * 4 + 24 > bsize) { free(dir); return -1; }

    uint32_t bucket = afs_hash(name, namelen, ht_size, afs_volume_flags(v));
    uint32_t bno = be32(dir + 24 + 4 * bucket);

    lookup_ctx_t lc = { name, namelen, out, 0, afs_volume_flags(v) };
    if (bno != 0) walk_chain(v, bno, lookup_cb, &lc);
    free(dir);
    return lc.found ? 0 : -1;
}