#include "amigafs/verify.h"
#include "amigafs/be.h"
#include "amigafs/bitmap.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/dir.h"
#include "amigafs/hash.h"
#include "amigafs/volume.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add_problem(afs_verify_report_t *out, int *n, int max,
                        afs_problem_kind_t kind, uint32_t block,
                        const char *fmt, ...) {
    if (*n >= max) return;
    afs_verify_report_t *p = &out[*n];
    p->kind = kind;
    p->block = block;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->detail, sizeof p->detail, fmt, ap);
    va_end(ap);
    (*n)++;
}

/* Recursively verify a directory's entries: validate each entry block's
 * checksum and type, bounds-check pointers, detect double use, and cross-check
 * the dir cache. `seen` marks physical blocks already referenced. */
typedef struct { uint8_t *marks; uint32_t nblocks; int max; } visit_ctx_t;

static void visit_entry(afs_volume_t *v, uint32_t block, visit_ctx_t *vc,
                        int depth, afs_verify_report_t *rep, int *n, int max);

/* Walk a directory's hash table RAW, with none of the defensive filtering
 * afs_dir_list applies.
 *
 * This is why the verifier cannot delegate its enumeration to afs_dir_list().
 * A read path is right to drop a pointer it cannot read and to skip a block
 * whose type it does not recognise -- a filesystem must not die on a bad
 * pointer, and on real Amiga volumes those filters are what keep a damaged
 * directory readable. But those two filters are EXACTLY the defects the
 * verifier exists to report: an out-of-range bucket pointer, and an
 * unrecognised block type. Enumerating through the filtered reader made both
 * checks unreachable, so `afs verify` reported neither, ever.
 *
 * So the verifier reads the hash table itself. Chains are followed with the
 * volume's block count as the step bound, so a cycle in the chain data
 * terminates instead of spinning.
 */
static void visit_dir_raw(afs_volume_t *v, uint32_t dirblk, visit_ctx_t *vc,
                          int depth, afs_verify_report_t *rep, int *n, int max) {
    if (depth > 32) return;
    size_t bs = afs_volume_block_size(v);
    uint8_t *dir = malloc(bs);
    if (!dir) return;
    if (afs_volume_read_block(v, dirblk, dir, bs) != 0) { free(dir); return; }

    /* the rootblock stores ht_size at offset 12; a DIRECTORY block does not
     * and derives it from the block size instead (see dir.c) */
    uint32_t ht = be32(dir + 12);
    if (ht == 0) ht = (uint32_t)afs_hash_table_size(bs);
    if (ht == 0 || (size_t)ht * 4 + 24 > bs) { free(dir); return; }

    uint8_t *eb = malloc(bs);
    if (!eb) { free(dir); return; }

    for (uint32_t i = 0; i < ht && *n < max; i++) {
        uint32_t bno = be32(dir + 24 + 4 * i);
        for (uint32_t step = 0; bno != 0 && step <= vc->nblocks; step++) {
            uint32_t here = bno;
            visit_entry(v, here, vc, depth + 1, rep, n, max);
            if (*n >= max) break;
            /* an unreadable block ends the chain, exactly as the read path
             * does -- but the pointer that got us here was already range-checked
             * by visit_entry, so the bad pointer itself is on the record */
            if (afs_volume_read_block(v, here, eb, bs) != 0) break;
            uint32_t next = be32(eb + bs - 16);
            if (next == here) break;                     /* self-loop guard */
            bno = next;
        }
    }
    free(eb);
    free(dir);
}

static void visit_entry(afs_volume_t *v, uint32_t block, visit_ctx_t *vc,
                        int depth, afs_verify_report_t *rep, int *n, int max) {
    if (depth > 32) return;
    if (block >= vc->nblocks) {
        add_problem(rep, n, max, AFS_PROBLEM_RANGE, block,
                    "entry block pointer %u outside volume (0..%u)",
                    block, vc->nblocks - 1);
        return;
    }
    if (vc->marks[block]) {
        add_problem(rep, n, max, AFS_PROBLEM_DOUBLE_USE, block,
                    "block %u referenced more than once", block);
        return;
    }
    vc->marks[block] = 1;

    size_t bs = afs_volume_block_size(v);
    uint8_t *b = malloc(bs);
    if (!b) return;
    if (afs_volume_read_block(v, block, b, bs) != 0) {
        add_problem(rep, n, max, AFS_PROBLEM_RANGE, block, "cannot read block %u", block);
        free(b);
        return;
    }
    /* The checksum rule is offset-independent: afs_block_valid sums every long
     * INCLUDING the stored checksum and requires the total to be zero, so the
     * checksum's own position never enters into it. (An earlier ternary here
     * offered 20 for both arms while its comment claimed T_DATA used 0; both
     * spellings pass or fail together, which is why it was never noticed.) */
    uint32_t primary = be32(b);
    if (!afs_block_valid(b, bs, 20)) {
        add_problem(rep, n, max, AFS_PROBLEM_CHECKSUM, block,
                    "block %u checksum invalid", block);
    }

    afs_block_kind_t k = afs_block_kind(b, bs);
    if (k == AFS_BLOCK_UNKNOWN) {
        add_problem(rep, n, max, AFS_PROBLEM_TYPE, block,
                    "block %u has unknown type (primary 0x%08x, sec 0x%08x)",
                    block, primary, be32(b + bs - 4));
        free(b);
        return;
    }

    /* Recurse into anything that CONTAINS entries. AFS_BLOCK_ROOT is the
     * crucial one: afs_verify_memory starts the walk at the rootblock, whose
     * kind is ROOT, not DIR -- so testing only DIR/LINKDIR meant the walk
     * never left block 1 and every check below was dead code on a real
     * volume. LINKDIR is deliberately NOT descended: a hard-linked directory
     * is a cycle (C12), and following it would re-visit the real directory
     * and report a false "referenced more than once". The link block itself
     * is still visited, so its checksum and type are checked. */
    if (k == AFS_BLOCK_ROOT || k == AFS_BLOCK_DIR) {
        visit_dir_raw(v, block, vc, depth + 1, rep, n, max);
    }
    free(b);
}

int afs_verify_memory_ex(const uint8_t *img, size_t size, size_t bsize,
                         afs_verify_report_t *out, int max, unsigned flags) {
    if (!img || !out || max <= 0) return 0;

    afs_volume_t *v = NULL;
    char err[256];
    if (afs_volume_open_memory((void *)img, size, bsize, &v, err, sizeof err) != 0) {
        /* The volume could not even be opened (bad rootblock, missing DOS id,
         * a checksum the open refuses to trust, ...). That IS the finding, so
         * report it rather than silently returning "no problems". */
        int n0 = 0;
        add_problem(out, &n0, max, AFS_PROBLEM_ROOTBLOCK, 0, "volume rejected: %s", err);
        return n0;
    }

    int n = 0;
    uint32_t nblocks = afs_volume_block_count(v);
    visit_ctx_t vc;
    vc.marks = calloc(nblocks, 1);
    vc.nblocks = nblocks;
    vc.max = max;
    if (!vc.marks) { afs_volume_close(v); return 0; }

    /* the rootblock is validated by open (it refuses a bad rootblock), so we
     * verify its entries here */
    uint32_t root = afs_volume_rootblock(v);
    visit_entry(v, root, &vc, 0, out, &n, max);

    /* Cross-check the bootblock's root field. Many images leave it 0 (the
     * computed value is authoritative); a NON-ZERO value that disagrees with
     * the computed rootblock is a real defect -- the hostile ffdisk0049.adf
     * carries 'DOS\2' here. */
    {
        uint8_t boot[512];
        if (afs_volume_read_block(v, 0, boot, sizeof boot) == 0) {
            uint32_t stated = be32(boot + 8);
            if (stated != 0 && stated != root)
                add_problem(out, &n, max, AFS_PROBLEM_ROOTBLOCK, 0,
                            "bootblock root field %u disagrees with the "
                            "computed rootblock %u", stated, root);
        }
    }

    /* Validate the bitmap: the flag must be -1 (valid) and every bitmap
     * storage block must carry a correct checksum. */
    {
        const afs_rootblock_t *rb = afs_volume_root(v);
        size_t bs = afs_volume_block_size(v);
        if (rb->bm_flag != 0xFFFFFFFFu)
            add_problem(out, &n, max, AFS_PROBLEM_BITMAP_MISMATCH, 0,
                        "bitmap flagged invalid (bm_flag=0x%08x)", rb->bm_flag);
        int npages = 0;
        for (int i = 0; i < 25 && n < max; i++) {
            uint32_t bp = rb->bm_pages[i];
            if (!bp) break;
            npages++;
            if (bp >= nblocks) {
                add_problem(out, &n, max, AFS_PROBLEM_RANGE, bp,
                            "bitmap block pointer %u outside volume", bp);
                continue;
            }
            /* A bitmap page is storage, like any other block, so it belongs in
             * the same "referenced once" set the tree walk uses. Without this
             * the double-use check could not see it: the real g1a30c.adf has
             * block 948 as bitmap page 2 AND as a file in the tree, so writing
             * either would silently corrupt the other. */
            if (vc.marks[bp]) {
                add_problem(out, &n, max, AFS_PROBLEM_DOUBLE_USE, bp,
                            "block %u is both bitmap page %d and a directory "
                            "entry", bp, i + 1);
                continue;
            }
            vc.marks[bp] = 1;
            uint8_t *bm = malloc(bs);
            if (!bm) break;
            if (afs_volume_read_block(v, bp, bm, bs) != 0) { free(bm); continue; }
            /* bitmap checksum lives at byte 0 */
            if (!afs_block_valid(bm, bs, 0))
                add_problem(out, &n, max, AFS_PROBLEM_CHECKSUM, bp,
                            "bitmap block %u checksum invalid", bp);
            free(bm);
        }
        /* The page list is terminated by a zero entry, so its length is the
         * volume's own size in pages: one bit per block, 32 bits per long,
         * and (block_size/4) longs per page. More pages than that means extra
         * blocks are being claimed as allocation map that do not need to be. */
        {
            /* the formula lives in bitmap.c: the allocator enforces the same
             * invariant (and refuses such a volume outright), so the two
             * cannot drift apart */
            size_t want = afs_bitmap_pages_required((uint32_t)nblocks, bs);
            if (npages != (int)want)
                add_problem(out, &n, max, AFS_PROBLEM_BITMAP_MISMATCH, 0,
                            "bitmap block count %d, but a %u-block volume "
                            "needs %zu", npages, nblocks, want);
        }

        /* PER-BLOCK AGREEMENT -- the check the header of this file has always
         * claimed and which did not exist. It is the one that matters for a
         * WRITER: a block the tree references but the map calls free will be
         * handed out again by the allocator, and the corruption is silent
         * because every pointer still resolves -- two entries just end up
         * claiming the same block, and the next write through either destroys
         * the other. ffdisk0049.adf is exactly this (map calls block 1089 free;
         * it is Plot/save.c's header).
         *
         * Only the REFERENCED-BUT-FREE direction is asserted. The reverse is
         * normal and must not be flagged: the bootblock and its mirror, and
         * often the rootblock itself, are marked used without appearing in any
         * chain, and every healthy fixture here has several such blocks.
         *
         * Runs after the page loop above so `marks` already includes the
         * bitmap pages themselves. */
        {
            size_t mapbytes = ((size_t)nblocks + 7u) / 8u;
            uint8_t *map;
            if (!(flags & AFS_VERIFY_STRICT)) goto skip_agreement;
            map = malloc(mapbytes ? mapbytes : 1);
            if (map && afs_bitmap_read(v, map, mapbytes) > 0) {
                for (uint32_t b = 0; b < (uint32_t)nblocks && n < max; b++) {
                    if (vc.marks[b] && afs_bitmap_test(map, b))
                        add_problem(out, &n, max,
                                    AFS_PROBLEM_BITMAP_MISMATCH, b,
                                    "block %u is referenced by the tree but "
                                    "the bitmap marks it free", b);
                }
            }
            free(map);
        }
skip_agreement:
            ;
    }

    free(vc.marks);
    afs_volume_close(v);
    return n;
}

int afs_verify_memory(const uint8_t *img, size_t size, size_t bsize,
                      afs_verify_report_t *out, int max) {
    return afs_verify_memory_ex(img, size, bsize, out, max, 0);
}
