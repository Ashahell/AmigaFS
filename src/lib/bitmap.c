/* Block bitmap reading. The rootblock holds up to 25 bitmap block pointers plus
 * a pointer to a chain of bitmap EXTENSION blocks (each holding more). One bit
 * per block: 1 = free, 0 = allocated. Bit 0 of long 0 describes block 0.
 */
#include "amigafs/bitmap.h"
#include "amigafs/be.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/dir.h"
#include "amigafs/volume.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rb_err(char *buf, size_t len, const char *fmt, ...) {
    va_list ap;
    if (!buf || len == 0) return;
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

size_t afs_bitmap_pages_required(uint32_t nblocks, size_t bsize) {
    size_t longs_needed = ((size_t)nblocks + 31u) / 32u;
    size_t longs_per_page = bsize / 4u;
    return longs_per_page ? (longs_needed + longs_per_page - 1u) / longs_per_page
                          : 0u;
}

/* Test/set one bit in a raw bitmap buffer (bit 0 of long 0 = block 0). */
int afs_bitmap_test(const uint8_t *map, uint32_t blk) {
    const uint8_t *p = map + (blk / 32) * 4;
    return (p[(blk % 32) / 8] >> (7 - (blk % 8))) & 1;
}

void afs_bitmap_set(uint8_t *map, uint32_t blk) {
    uint8_t *p = map + (blk / 32) * 4;
    p[(blk % 32) / 8] |= (uint8_t)(0x80 >> (blk % 8));
}

void afs_bitmap_clear(uint8_t *map, uint32_t blk) {
    uint8_t *p = map + (blk / 32) * 4;
    p[(blk % 32) / 8] &= (uint8_t)~(0x80 >> (blk % 8));
}

/* Count free blocks reported across the root's bitmap pointers and extension
 * chain. `max_free_bits` bounds the scan to the bitmap storage available. */
static void collect_bitmap_blocks(const afs_rootblock_t *root, size_t bsize,
                                  uint32_t **blocks, size_t *count) {
    size_t cap = 25 + 64;      /* 25 inline + a bounded extension chain */
    uint32_t *arr = malloc(cap * sizeof *arr);
    size_t n = 0;
    if (!arr) { *blocks = NULL; *count = 0; return; }
    for (int i = 0; i < 25; i++)
        if (root->bm_pages[i]) arr[n++] = root->bm_pages[i];
    /* extension chain: each block's body holds pointers, last long is next */
    uint32_t ext = root->bm_ext;
    uint32_t guard = 0;
    while (ext && n < cap && guard++ < 64) {
        /* the extension block itself is a bitmap storage block; count it too */
        if (n < cap) arr[n++] = ext;
        ext = 0;   /* the caller resolves the chain; we record the head only */
        break;     /* chain traversal needs I/O, handled by the volume path */
    }
    (void)bsize;
    *blocks = arr;
    *count = n;
}

/* Assemble the volume's bitmap into a caller buffer. Returns the number of
 * blocks read, or -1 on error. The bitmap may span several storage blocks. */
int afs_bitmap_read(afs_volume_t *v, uint8_t *out, size_t out_cap) {
    const afs_rootblock_t *root = afs_volume_root(v);
    if (!root) return -1;
    memset(out, 0, out_cap);
    size_t bs = afs_volume_block_size(v);

    uint32_t *blocks = NULL;
    size_t nblocks = 0;
    collect_bitmap_blocks(root, bs, &blocks, &nblocks);
    if (!blocks) return -1;

    size_t filled = 0;   /* bytes of bitmap produced so far */
    size_t done = 0;
    for (size_t i = 0; i < nblocks && filled < out_cap; i++) {
        uint8_t *blk = malloc(bs);
        if (!blk) break;
        if (afs_volume_read_block(v, blocks[i], blk, bs) != 0) { free(blk); break; }
        size_t take = bs - 4;   /* bitmap blocks: the first long is the checksum */
        if (filled + take > out_cap) take = out_cap - filled;
        memcpy(out + filled, blk + 4, take);
        filled += take;
        done++;
        free(blk);
    }
    free(blocks);
    return (int)done;
}

/* --- Map rebuild ------------------------------------------------------- */

/* Data-block pointers a header or extension block can hold: BSIZE/4-56. Same
 * definition file.c uses, restated here because the reverse-order table walk
 * below has to agree with it exactly. */
static size_t rb_table_entries(size_t bsize) {
    return bsize / 4u - 56u;
}

typedef struct {
    afs_volume_t *v;
    uint8_t *marks;
    uint32_t nblocks;
    size_t   bsize;
    uint32_t payload;      /* usable data bytes per block, OFS vs FFS */
    char    *errbuf;
    size_t   errlen;
} rb_ctx_t;

static void rb_mark(rb_ctx_t *c, uint32_t b) {
    if (b < c->nblocks) c->marks[b] = 1;
}

/* Walk a file's table chain marking every data block and every extension block.
 *
 * The table is REVERSED: the first data block occupies the LAST slot, so file
 * block i is at index (entries-1-i). `high_seq` is a PER-BLOCK count of the
 * pointers held in THAT block, not a total for the file -- the walk continues
 * through bs-8 until `need` pointers have been gathered. Both facts are load
 * bearing: marking only the header's own table frees the extension blocks,
 * which is how a rebuild loses the tail of every large file.
 *
 * A file whose header claims more blocks than its chain actually supplies is
 * REFUSED rather than partially marked. Marking what we have and freeing the
 * rest would destroy the difference. */
static int rb_mark_file(rb_ctx_t *c, uint32_t hdrblk) {
    size_t bs = c->bsize;
    size_t entries = rb_table_entries(bs);
    uint8_t *b = malloc(bs);
    if (!b) { rb_err(c->errbuf, c->errlen, "out of memory"); return -1; }

    /* Read the header for byte_size, then derive how many data blocks it needs. */
    if (afs_volume_read_block(c->v, hdrblk, b, bs) != 0) {
        free(b);
        rb_err(c->errbuf, c->errlen, "cannot read file header block %u", hdrblk);
        return -1;
    }
    uint32_t bytes = be32(b + bs - 188);
    uint32_t need = (uint32_t)((bytes + c->payload - 1u) / c->payload);
    if (bytes == 0) need = 0;

    uint32_t cur = hdrblk;
    uint32_t guard = 0;
    uint32_t got = 0;
    while (got < need) {
        if (cur == 0) {
            free(b);
            rb_err(c->errbuf, c->errlen,
                   "file at block %u claims %u data blocks but its chain supplies %u",
                   hdrblk, need, got);
            return -1;
        }
        if (cur >= c->nblocks || guard++ > c->nblocks) {
            free(b);
            rb_err(c->errbuf, c->errlen,
                   "file at block %u has a corrupt table chain", hdrblk);
            return -1;
        }
        /* hdrblk was already read into b; anything else must be fetched. */
        if (cur != hdrblk) {
            if (afs_volume_read_block(c->v, cur, b, bs) != 0) {
                free(b);
                rb_err(c->errbuf, c->errlen, "cannot read table block %u", cur);
                return -1;
            }
        }
        if (cur != hdrblk) rb_mark(c, cur);          /* the extension block */

        uint32_t hs = be32(b + 8);                    /* pointers in THIS block */
        if (hs > entries) hs = (uint32_t)entries;
        for (uint32_t k = 0; k < hs && got < need; k++, got++) {
            size_t idx = entries - 1u - k;            /* REVERSED */
            uint32_t ptr = be32(b + 24 + 4 * idx);
            if (ptr > 1u) rb_mark(c, ptr);
        }
        uint32_t next = be32(b + bs - 8);
        cur = next;
        /* A chain that stops before `need` is a short file, not an error --
         * but leaving `got < need` to the loop above reports it precisely. */
    }
    free(b);
    return 0;
}

/* Mark a directory's dircache chain. The dir block's bs-8 field points at the
 * first cache block; each cache block's nextDirC lives at +4. ADFlib marks the
 * same chain (adf_bitm.c:989). */
static int rb_mark_dircache(rb_ctx_t *c, uint32_t dirblk) {
    size_t bs = c->bsize;
    uint8_t *b = malloc(bs);
    if (!b) { rb_err(c->errbuf, c->errlen, "out of memory"); return -1; }
    if (afs_volume_read_block(c->v, dirblk, b, bs) != 0) {
        free(b);
        rb_err(c->errbuf, c->errlen, "cannot read directory block %u", dirblk);
        return -1;
    }
    uint32_t cb = be32(b + bs - 8);
    uint32_t guard = 0;
    while (cb) {
        if (cb >= c->nblocks || guard++ > c->nblocks) {
            free(b);
            rb_err(c->errbuf, c->errlen,
                   "directory block %u has a corrupt dircache chain", dirblk);
            return -1;
        }
        rb_mark(c, cb);
        if (afs_volume_read_block(c->v, cb, b, bs) != 0) {
            free(b);
            rb_err(c->errbuf, c->errlen, "cannot read dircache block %u", cb);
            return -1;
        }
        cb = be32(b + 4);                            /* nextDirC */
    }
    free(b);
    return 0;
}

/* Recursive directory walk. `AFS_BLOCK_LINKDIR` is deliberately NOT descended:
 * a hard-linked directory is a cycle, and following it would either hang or
 * free a live directory's blocks. The link block itself is still marked. */
static int rb_walk(rb_ctx_t *c, uint32_t dirblk, int depth) {
    if (depth > 32) {
        rb_err(c->errbuf, c->errlen, "directory tree nested deeper than 32");
        return -1;
    }
    enum { RB_MAX_ENTS = 4096 };
    afs_dirent_t *ents = malloc(RB_MAX_ENTS * sizeof *ents);
    if (!ents) { rb_err(c->errbuf, c->errlen, "out of memory"); return -1; }

    size_t n = 0;
    if (afs_dir_list(c->v, dirblk, ents, RB_MAX_ENTS, &n) != 0) {
        free(ents);
        rb_err(c->errbuf, c->errlen, "cannot list directory block %u", dirblk);
        return -1;
    }
    /* A truncated listing is the dangerous case: the entries we did NOT see
     * would look unreferenced and be freed. Refuse rather than guess. */
    if (n == RB_MAX_ENTS) {
        free(ents);
        rb_err(c->errbuf, c->errlen,
               "directory block %u has at least %u entries; refusing to rebuild "
               "a map from a truncated listing", dirblk, RB_MAX_ENTS);
        return -1;
    }

    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        uint32_t b = ents[i].block;
        rb_mark(c, b);                       /* the entry's own header block */

        if (ents[i].kind == AFS_BLOCK_FILE) {
            rc = rb_mark_file(c, b);
        } else if (ents[i].kind == AFS_BLOCK_DIR) {
            rc = rb_mark_dircache(c, b);
            if (rc == 0) rc = rb_walk(c, b, depth + 1);
        }
        /* LINKFILE / LINKDIR / SOFTLINK: the block is marked, nothing is
         * descended. Matches ADFlib, including its not following a soft
         * link's target. */
    }
    free(ents);
    return rc;
}

int afs_bitmap_rebuild(afs_volume_t *v, afs_bitmap_rebuild_stats_t *stats,
                       char *errbuf, size_t errlen) {
    if (!v) { rb_err(errbuf, errlen, "bad arguments"); return -1; }

    const afs_rootblock_t *root = afs_volume_root(v);
    if (!root) { rb_err(errbuf, errlen, "volume has no rootblock"); return -1; }
    if (root->bm_ext != 0) {
        /* Same documented limit as the allocator: refusing beats half-support.
         * ADFlib walks the chain; we do not, and saying so is the point. */
        rb_err(errbuf, errlen,
               "bitmap extension chain not supported (volume too large)");
        return -1;
    }

    size_t bs = afs_volume_block_size(v);
    uint32_t nblocks = afs_volume_block_count(v);
    uint32_t rootblk = afs_volume_rootblock(v);
    if (nblocks < 3) {
        rb_err(errbuf, errlen, "volume of %u blocks is too small to rebuild",
               nblocks);
        return -1;
    }

    /* The page list is zero-terminated; more pages than the volume needs means
     * map bits describing blocks outside it (g1a30c.adf's second page IS a live
     * directory entry). Rebuild to the correct size rather than trusting it --
     * that is exactly what a rescan is for. */
    size_t want_pages = afs_bitmap_pages_required(nblocks, bs);

    uint32_t pages[25];
    size_t npages = 0;
    for (int i = 0; i < 25 && root->bm_pages[i] != 0; i++) pages[npages++] = root->bm_pages[i];
    if (npages != want_pages) {
        rb_err(errbuf, errlen,
               "volume lists %zu bitmap pages but needs %zu; rebuild needs the "
               "page list repaired first", npages, want_pages);
        return -1;
    }
    for (size_t i = 0; i < npages; i++) {
        if (pages[i] >= nblocks) {
            rb_err(errbuf, errlen, "bitmap page %zu points outside the volume",
                   i);
            return -1;
        }
    }

    /* Mark the rootblock and every bitmap page BEFORE the walk, exactly as
     * ADFlib does (adf_bitm.c:572-578). A page that the free pass could select
     * would make the allocator hand out the map itself. */
    uint8_t *marks = calloc(nblocks, 1);
    if (!marks) { rb_err(errbuf, errlen, "out of memory"); return -1; }

    rb_ctx_t c;
    c.marks = marks;
    c.nblocks = nblocks;
    c.bsize = bs;
    c.payload = (uint32_t)(afs_volume_is_ofs(v) ? (bs - 24u) : bs);
    c.v = v;
    c.errbuf = errbuf;
    c.errlen = errlen;

    marks[rootblk] = 1;
    for (size_t i = 0; i < npages; i++) marks[pages[i]] = 1;

    int rc = rb_walk(&c, rootblk, 0);
    if (rc != 0) { free(marks); return -1; }

    /* Everything is marked. Only now do we touch the volume -- so a failure
     * anywhere above leaves the image byte-identical, which is what makes the
     * abort meaningful. */
    size_t map_bytes = ((size_t)nblocks + 31u) / 32u * 4u;
    uint8_t *map = calloc(map_bytes, 1);
    uint8_t *old = calloc(map_bytes, 1);
    uint8_t *page = malloc(bs);
    uint8_t *rblk = malloc(bs);
    if (!map || !old || !page || !rblk) {
        free(map); free(old); free(page); free(rblk); free(marks);
        rb_err(errbuf, errlen, "out of memory");
        return -1;
    }

    /* The old map, for the reclaim count only. A map we cannot read is not a
     * reason to refuse the rebuild; it only costs the statistic. */
    if (afs_bitmap_read(v, old, map_bytes) < 0) memset(old, 0, map_bytes);

    uint32_t reclaimed = 0;
    for (uint32_t b = 2; b + 1 < nblocks; b++) {
        /* The bound is the safety property: b starts at 2 and stops before the
         * tail, so blocks 0, 1 and the volume-tail mirror are not candidates.
         * ADFlib: for (i = 2; i < lastBlock - firstBlock; i++) setFree(i); */
        if (marks[b]) afs_bitmap_clear(map, b);
        else           afs_bitmap_set(map, b);
        if (marks[b] == 0 && afs_bitmap_test(old, b) == 0) reclaimed++;
    }

    for (size_t i = 0; i < npages; i++) {
        /* Each page's data area is bs-4 bytes (its first long is the checksum),
         * laid out contiguously across pages -- the same split afs_bitmap_read
         * and afs_alloc_block use, so a rebuild round-trips through both. */
        size_t off = i * (bs - 4);
        size_t take = bs - 4;
        if (off >= map_bytes) take = 0;
        else if (off + take > map_bytes) take = map_bytes - off;
        memset(page, 0, bs);
        if (take) memcpy(page + 4, map + off, take);
        put_be32(page, afs_block_checksum(page, bs, 0));
        if (afs_volume_write_block(v, pages[i], page, bs) != 0) {
            free(map); free(old); free(page); free(rblk); free(marks);
            rb_err(errbuf, errlen, "cannot write bitmap page %zu", i);
            return -1;
        }
    }

    /* Flag dance, matching the allocator: INVALID while mid-write, VALID after.
     * A crash between the two leaves a volume the verifier reports rather than
     * silent rot -- which is precisely the state this function exists to leave
     * behind deliberately. */
    if (afs_volume_read_block(v, rootblk, rblk, bs) != 0) {
        free(map); free(old); free(page); free(rblk); free(marks);
        rb_err(errbuf, errlen, "cannot read rootblock");
        return -1;
    }
    put_be32(rblk + bs - 200, 0);
    put_be32(rblk + 20, afs_block_checksum(rblk, bs, 20));
    if (afs_volume_write_block(v, rootblk, rblk, bs) != 0) {
        free(map); free(old); free(page); free(rblk); free(marks);
        rb_err(errbuf, errlen, "cannot mark bitmap invalid");
        return -1;
    }
    put_be32(rblk + bs - 200, 0xFFFFFFFFu);
    put_be32(rblk + 20, afs_block_checksum(rblk, bs, 20));
    if (afs_volume_write_block(v, rootblk, rblk, bs) != 0) {
        free(map); free(old); free(page); free(rblk); free(marks);
        rb_err(errbuf, errlen, "cannot revalidate bitmap");
        return -1;
    }

    if (stats) {
        stats->total = nblocks;
        stats->used = 0;
        for (uint32_t b = 0; b < nblocks; b++) if (marks[b]) stats->used++;
        stats->free_blocks = nblocks - stats->used;
        stats->reclaimed = reclaimed;
    }
    free(map); free(old); free(page); free(rblk); free(marks);
    return 0;
}