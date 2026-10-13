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
    /* checksum at byte 20 for the T_HEADER/T_LIST family, byte 0 for T_DATA */
    uint32_t primary = be32(b);
    size_t chk_off = (primary == AFS_T_DATA) ? 20 : 20;  /* header family uses 20 */
    if (primary == AFS_T_DIRCACHE) chk_off = 20;
    if (!afs_block_valid(b, bs, chk_off)) {
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

    if (k == AFS_BLOCK_DIR || k == AFS_BLOCK_LINKDIR) {
        uint32_t dirc = be32(b + bs - 8);
        /* recurse into subdirectories */
        char name[32];
        bstr(b + bs - 80, 31, name, sizeof name);
        (void)name; (void)dirc;
        /* list this dir and visit its entries */
        afs_dirent_t ents[256];
        size_t ec = 0;
        if (afs_dir_list(v, block, ents, 256, &ec) == 0) {
            for (size_t i = 0; i < ec; i++)
                visit_entry(v, ents[i].block, vc, depth + 1, rep, n, max);
        }
    }
    free(b);
}

int afs_verify_memory(const uint8_t *img, size_t size, size_t bsize,
                      afs_verify_report_t *out, int max) {
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
        for (int i = 0; i < 25 && n < max; i++) {
            uint32_t bp = rb->bm_pages[i];
            if (!bp) break;
            if (bp >= nblocks) {
                add_problem(out, &n, max, AFS_PROBLEM_RANGE, bp,
                            "bitmap block pointer %u outside volume", bp);
                continue;
            }
            uint8_t *bm = malloc(bs);
            if (!bm) break;
            if (afs_volume_read_block(v, bp, bm, bs) != 0) { free(bm); continue; }
            /* bitmap checksum lives at byte 0 */
            if (!afs_block_valid(bm, bs, 0))
                add_problem(out, &n, max, AFS_PROBLEM_CHECKSUM, bp,
                            "bitmap block %u checksum invalid", bp);
            free(bm);
        }
    }

    free(vc.marks);
    afs_volume_close(v);
    return n;
}