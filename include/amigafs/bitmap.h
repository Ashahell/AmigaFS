#ifndef AMIGAFS_BITMAP_H
#define AMIGAFS_BITMAP_H

#include <stddef.h>
#include <stdint.h>

#include "amigafs/volume.h"

/* Block bitmap access. One bit per block: 1 = free, 0 = allocated. Bit 0 of
 * long 0 describes block 0. The bitmap may span several storage blocks; its
 * checksum lives at byte 0 of the FIRST storage block only (every later
 * storage block's first long is ordinary map data). */

/* Raw bit helpers on a bitmap buffer. */
int  afs_bitmap_test(const uint8_t *map, uint32_t blk);
void afs_bitmap_set(uint8_t *map, uint32_t blk);
void afs_bitmap_clear(uint8_t *map, uint32_t blk);

/* Assemble the volume's whole bitmap into `out` (capacity out_cap bytes).
 * Returns the number of storage blocks read, or -1 on error. */
int afs_bitmap_read(afs_volume_t *v, uint8_t *out, size_t out_cap);

/* How many bitmap storage blocks a volume of `nblocks` blocks needs: one bit
 * per block, 32 bits per long, (bsize/4) longs per page.
 *
 * This lives here because TWO places must agree on it and previously each had
 * its own copy: the verifier reports a page count that disagrees with this as
 * a problem, and the allocator must refuse such a volume outright. A writer
 * that accepts a page list longer than the volume needs is reading map bits
 * that describe blocks outside the volume -- and g1a30c.adf's second page is
 * not merely redundant: block 948 is simultaneously that page and a live
 * directory entry, so allocating from it hands out a block that is in use. */
size_t afs_bitmap_pages_required(uint32_t nblocks, size_t bsize);

/* --- Map rebuild (M18) ---
 *
 * A FULL, UNCONDITIONAL rescan: mark every block the tree references, free
 * everything else. This is the AmigaDOS policy, not a cautious variant of it.
 * bm_Flags in the rootblock is documented as "TRUE if Bitmap on disk is valid",
 * and the historical repair was a boot-time disk validator that
 * write-protected the drive and scanned. ADFlib -- our pinned oracle, read at
 * src/adf_bitm.c:443 -- does the same thing: no pre-verification pass, no
 * consult of bm_Flags, all-or-nothing if any block cannot be read.
 *
 * Because the pass FREES whatever it fails to mark, the safety of the whole
 * operation rests on the free pass's bounds, which mirror ADFlib's exactly:
 *
 *     for (i = 2; i < lastBlock - firstBlock; i++) setFree(i);
 *
 * The bounds make blocks 0, 1 and the volume-tail mirror structurally
 * unreachable rather than relying on the walk remembering to mark them. The
 * rootblock and every bitmap page ARE marked used explicitly, as ADFlib does
 * (lines 572-578).
 *
 * Deliberately matched to AmigaDOS, not "improved on": a link's own block is
 * marked, but a SOFT LINK'S TARGET IS NOT. A dangling soft link therefore loses
 * its target on a rebuild. ADFlib does the same; divergence would be a bug.
 *
 * `stats` may be NULL. Returns 0 on success; on any failure NOTHING is
 * written -- the whole walk completes into a marks array before the first
 * byte is written, which is what makes the abort ADFlib performs on a read
 * failure meaningful here too. */
typedef struct {
    uint32_t total;      /* blocks in the volume */
    uint32_t used;       /* blocks the tree references after the rebuild */
    uint32_t free_blocks;/* blocks free after the rebuild */
    uint32_t reclaimed;  /* marked used before, free after: the point of the exercise */
} afs_bitmap_rebuild_stats_t;

int afs_bitmap_rebuild(afs_volume_t *v, afs_bitmap_rebuild_stats_t *stats,
                       char *errbuf, size_t errlen);

#endif /* AMIGAFS_BITMAP_H */