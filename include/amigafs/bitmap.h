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

#endif /* AMIGAFS_BITMAP_H */