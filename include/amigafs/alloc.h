#ifndef AMIGAFS_ALLOC_H
#define AMIGAFS_ALLOC_H

#include <stddef.h>
#include <stdint.h>

#include "amigafs/volume.h"

/* Block allocation for the write phase (D3).
 *
 * The allocator works storage-page by storage-page, never assembling the
 * whole bitmap: for each bitmap page in rootblock order it reads the page,
 * scans its data longs for the first free bit below the volume size, clears
 * it, restamps that page's checksum, and writes the page back. Scanning
 * starts at block 2 (blocks 0-1 are the bootblock and must never allocate,
 * even if their bits read free).
 *
 * Every bitmap storage block carries its own checksum long at byte 0 (127
 * data longs follow; one page covers (bsize-4)*8 blocks). The rootblock flag
 * does the INVALID -> write pages -> VALID dance so a crash mid-flush leaves
 * a volume the verifier reports as suspect rather than silently wrong.
 *
 * LIMIT: only the 25 inline bitmap pages are supported (25 * 4064 = 101600
 * blocks at 512 bytes/block, ~50 MiB). A volume carrying a bitmap EXTENSION
 * chain is refused with a named reason rather than half-supported: none of
 * our fixtures need one, and guessing the chain layout risks silent
 * corruption. */

/* Allocate one block. On success returns 0 and sets *out; when no free block
 * remains, or the bitmap uses an extension chain, returns non-zero with a
 * named reason in errbuf (when non-NULL). */
int afs_alloc_block(afs_volume_t *v, uint32_t *out,
                    char *errbuf, size_t errlen);

/* Return a block to the bitmap (truncate path). The bit is set free, the page
 * restamped and written back, and the rootblock flag danced, exactly mirroring
 * allocation. Refuses structural blocks (< 2) and out-of-volume numbers with
 * named reasons. */
int afs_free_block(afs_volume_t *v, uint32_t blk,
                   char *errbuf, size_t errlen);

#endif /* AMIGAFS_ALLOC_H */
