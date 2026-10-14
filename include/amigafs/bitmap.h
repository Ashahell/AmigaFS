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

#endif /* AMIGAFS_BITMAP_H */