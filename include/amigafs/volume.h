#ifndef AMIGAFS_VOLUME_H
#define AMIGAFS_VOLUME_H

#include <stddef.h>
#include <stdint.h>

#include "amigafs/blocktypes.h"

/* An opened AmigaDOS volume.
 *
 * The volume owns the parsed rootblock, the DOS type flags and the block
 * device it reads through. Everything above this layer works in partition-
 * relative block numbers via blockio.
 */

typedef struct afs_volume afs_volume_t;

/* Open a volume from an in-memory image. `bsize` is the block size (512 for
 * floppies). On success returns 0 and sets *out; on failure returns non-zero,
 * leaves *out NULL, and (when `errbuf` is non-NULL) writes a named reason.
 *
 * Detection: the DOS id is the ASCII 'D','O','S' plus a flag byte, found on
 * block 0 OR block 1. Flag values DOS\0..DOS\5 are supported; DOS\6/DOS\7
 * (AmigaOS 3.2 long filenames) and any non-'DOS' id (PFS, muFS) are refused.
 */
int afs_volume_open_memory(void *data, size_t size, size_t bsize,
                           afs_volume_t **out, char *errbuf, size_t errlen);

/* Open a PARTITION of an RDB-partitioned image.
 *
 * Inside a partition every AmigaDOS block pointer is partition-relative, so a
 * partition is a WINDOW of exactly its own block range -- `nblocks` long, not
 * "from here to the end of the image". Getting that length wrong is not a
 * rounding error: the rootblock is computed from the volume's own block count
 * as (2 + last)/2, so an over-long window puts the rootblock in the middle of
 * the NEXT partition and the open fails. On a six-partition disk that leaves
 * exactly one partition mountable -- the last, whose window length happens to
 * be right by accident.
 *
 * `nblocks` == 0 means "to the end of the image", which is what a whole
 * floppy wants; afs_volume_open_file() is the nblocks==0 spelling of this. */
int afs_volume_open_window(const char *path, size_t bsize,
                           uint32_t start_block, uint32_t nblocks,
                           afs_volume_t **out, char *errbuf, size_t errlen);

/* Writable window (D3 write phase). Identical validation and layout to the
 * read-only open; only the file backend differs. */
int afs_volume_open_window_rw(const char *path, size_t bsize,
                              uint32_t start_block, uint32_t nblocks,
                              afs_volume_t **out, char *errbuf, size_t errlen);

/* Open a volume from a file. */
int afs_volume_open_file(const char *path, size_t bsize, uint32_t start_block,
                         afs_volume_t **out, char *errbuf, size_t errlen);

/* Close the volume and its block device. */
void afs_volume_close(afs_volume_t *v);

/* DOS type flag byte (the low 3 bits of the bootblock's first long). */
uint8_t afs_volume_flags(const afs_volume_t *v);

/* The DOS type as a printable number, 0..5. */
unsigned afs_volume_dostype(const afs_volume_t *v);

/* Volume name from the rootblock (Latin-1, NUL-terminated). */
const char *afs_volume_name(const afs_volume_t *v);

/* Block number of the rootblock, located by the single formula
 * INT((Reserved + highKey)/2) and validated as T_HEADER/ST_ROOT. */
uint32_t afs_volume_rootblock(const afs_volume_t *v);

/* Total blocks in the volume. */
uint32_t afs_volume_block_count(const afs_volume_t *v);

/* Block size in bytes. */
size_t afs_volume_block_size(const afs_volume_t *v);

/* The parsed rootblock. */
const afs_rootblock_t *afs_volume_root(const afs_volume_t *v);

/* OFS is the ABSENCE of the FFS bit; DOS\2 and DOS\4 are OFS too. */
int afs_volume_is_ofs(const afs_volume_t *v);
int afs_volume_is_ffs(const afs_volume_t *v);
int afs_volume_has_intl(const afs_volume_t *v);      /* DIRC implies INTL */
int afs_volume_has_dircache(const afs_volume_t *v);

/* Read one partition-relative block. Returns 0 on success. */
int afs_volume_read_block(afs_volume_t *v, uint32_t blockno, void *buf, size_t len);

/* Write one partition-relative block. Returns 0 on success. Refused on a
 * read-only volume, like every other out-of-contract write. */
int afs_volume_write_block(afs_volume_t *v, uint32_t blockno, const void *buf,
                           size_t len);

#endif /* AMIGAFS_VOLUME_H */