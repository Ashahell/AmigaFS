/* Block allocation (D3 write phase). See alloc.h for the contract. */
#include "amigafs/alloc.h"
#include "amigafs/be.h"
#include "amigafs/bitmap.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *buf, size_t len, const char *msg) {
    if (buf && len) snprintf(buf, len, "%s", msg);
}

/* Restamp a storage block and write it back. `chk_off` is 20 for the header
 * family, 0 for bitmap pages. */
static int write_stamped(afs_volume_t *v, uint32_t blk, uint8_t *b,
                         size_t bs, size_t chk_off) {
    put_be32(b + chk_off, afs_block_checksum(b, bs, chk_off));
    return afs_volume_write_block(v, blk, b, bs);
}

int afs_alloc_block(afs_volume_t *v, uint32_t *out,
                    char *errbuf, size_t errlen) {
    size_t bs;
    uint32_t nblocks, rootblk;
    uint8_t *rblk, *page;
    afs_rootblock_t root;
    uint32_t i, found = 0xFFFFFFFFu;

    if (!v || !out) { set_err(errbuf, errlen, "bad arguments"); return -1; }
    bs = afs_volume_block_size(v);
    nblocks = afs_volume_block_count(v);
    rootblk = afs_volume_rootblock(v);

    rblk = malloc(bs);
    page = malloc(bs);
    if (!rblk || !page) {
        free(rblk); free(page);
        set_err(errbuf, errlen, "out of memory");
        return -1;
    }
    if (afs_volume_read_block(v, rootblk, rblk, bs) != 0) {
        free(rblk); free(page);
        set_err(errbuf, errlen, "cannot read rootblock");
        return -1;
    }
    if (afs_parse_rootblock(rblk, bs, &root) != 0) {
        free(rblk); free(page);
        set_err(errbuf, errlen, "rootblock does not parse");
        return -1;
    }
    if (root.bm_ext != 0) {
        /* Documented limit (alloc.h): extension chains are refused, not
         * half-supported. None of our fixtures need one. */
        free(rblk); free(page);
        set_err(errbuf, errlen,
                "bitmap extension chain not supported (volume too large)");
        return -1;
    }

    /* scan pages in rootblock order for the first free bit at block >= 2 */
    for (i = 0; i < 25 && found == 0xFFFFFFFFu; i++) {
        uint32_t pg = root.bm_pages[i];
        uint32_t base, b;
        if (pg == 0 || pg >= nblocks) continue;
        if (afs_volume_read_block(v, pg, page, bs) != 0) continue;
        base = i * (uint32_t)((bs - 4) * 8);
        for (b = (base < 2) ? (2 - base) : 0;
             base + b < nblocks && b < (bs - 4) * 8; b++) {
            /* bit index within the DATA area (byte 4 onward); byte 0..3 is
             * this page's own checksum long, not map data */
            size_t byte = 4 + (b / 8);
            unsigned bit = 7 - (b % 8);
            if (byte < bs && (page[byte] & (uint8_t)(1u << bit))) {
                page[byte] &= (uint8_t)~(1u << bit);
                found = base + b;
                break;
            }
        }
        if (found != 0xFFFFFFFFu) {
            /* restamp this page's own checksum long and write it back */
            put_be32(page + 0, afs_block_checksum(page, bs, 0));
            if (afs_volume_write_block(v, pg, page, bs) != 0) {
                free(rblk); free(page);
                set_err(errbuf, errlen, "cannot write bitmap page");
                return -1;
            }
        }
    }
    free(page);
    if (found == 0xFFFFFFFFu) {
        free(rblk);
        set_err(errbuf, errlen, "volume full: no free block");
        return -1;
    }

    /* flag dance: INVALID while the bitmap is mid-flush, VALID after. A crash
     * between the two leaves a volume the verifier reports, not silent rot.
     * (Root mtime is deliberately untouched: nothing asserts it, and churning
     * it would make whole-image comparisons unstable for no reader benefit.) */
    put_be32(rblk + bs - 200, 0);   /* bm_flag = INVALID */
    if (write_stamped(v, rootblk, rblk, bs, 20) != 0) {
        free(rblk);
        set_err(errbuf, errlen, "cannot mark bitmap invalid");
        return -1;
    }
    put_be32(rblk + bs - 200, 0xFFFFFFFFu);
    if (write_stamped(v, rootblk, rblk, bs, 20) != 0) {
        free(rblk);
        set_err(errbuf, errlen, "cannot revalidate bitmap");
        return -1;
    }
    free(rblk);
    *out = found;
    return 0;
}
