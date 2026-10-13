#ifndef AMIGAFS_FILE_H
#define AMIGAFS_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "amigafs/blocktypes.h"
#include "amigafs/dir.h"
#include "amigafs/volume.h"

/* File data reads.
 *
 * BOTH OFS and FFS read data through the `data_blocks[]` TABLE, not through
 * OFS's `next_data` chain. The table is stored in REVERSE: the first data
 * block occupies the LAST slot, so file block i is at table index
 * (entries-1-i). OFS's seq_num/next_data/data_size are used only as
 * cross-checks; they never define the read order.
 *
 * `high_seq` is a PER-BLOCK count of the pointers held in that one header or
 * extension block. The file's total block count is derived from `byte_size`.
 */

typedef struct {
    uint32_t block;        /* the file header block */
    uint32_t byte_size;
    uint32_t protect;
    uint16_t uid, gid;
    uint8_t  kind;
    char     name[AFS_NAME_MAX];
} afs_filestat_t;

/* Stat a file by name in a directory. Returns 0 on success. */
int afs_file_stat(afs_volume_t *v, uint32_t dirblock,
                  const char *name, size_t namelen, afs_filestat_t *out);

/* Read `len` bytes from `offset` into `buf`, writing the number of bytes
 * actually read to *got. A read extending past EOF is CLAMPED to the file
 * size. An offset at or beyond EOF yields *got == 0. Returns 0 on success,
 * non-zero only on a structural error. */
int afs_file_read(afs_volume_t *v, uint32_t dirblock,
                  const char *name, size_t namelen,
                  void *buf, size_t len, uint64_t offset, size_t *got);

/* Read the whole file into a freshly malloc'd buffer. The caller frees it.
 * *out_len receives the file size. Returns 0 on success; on failure returns
 * non-zero and sets *out to NULL. */
int afs_file_read_all(afs_volume_t *v, uint32_t dirblock,
                       const char *name, size_t namelen,
                       void **out, size_t *out_len);

/* Read from a file given its HEADER BLOCK directly (no name lookup). This is
 * what the FUSE read path needs, because FUSE hands us an inode (block
 * number), not a path. `fileblock` may be a plain file header or a hard link
 * (resolved via real_entry). Reads past EOF are clamped; *got receives the
 * byte count. */
int afs_read_file_by_block(afs_volume_t *v, uint32_t fileblock,
                           void *out, size_t len, uint64_t offset, size_t *got);

#endif /* AMIGAFS_FILE_H */