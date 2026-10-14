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

/* Comments.
 *
 * A comment is a BCPL string in the ENTRY BLOCK: the length byte at
 * bsize-184, the data at bsize-183, at most 79 bytes. This holds on every
 * volume -- DIRC or not, file or directory -- because on a DIRC volume the dir
 * cache holds only a REDUNDANT second copy, and the hash table stays the
 * authority for lookup (decision C4). A comment therefore survives a damaged
 * or stale dir cache.
 *
 * The offsets were verified byte-for-byte against amitools 0.8.1's own
 * EntryBlock parser over real fixture images before this was written; see
 * docs/comment-layout.md for the method, the worked example and the two traps
 * (the length is a single byte, so a corrupt one must be clamped, and the
 * DOS\6/DOS\7 long-name field at bsize-46 is a DIFFERENT layout that this
 * reader deliberately does not implement, since those volumes are refused).
 */

/* Read the comment out of a raw entry block (file header, directory, or link).
 * Writes a NUL-terminated string into `out` and returns the number of
 * characters written, EXCLUDING the NUL. A length byte of 0, or an entry with
 * no comment, yields an empty string. A corrupt length byte cannot claim more
 * than the format's 79 bytes and cannot read past the block. A capacity smaller
 * than the comment truncates; a capacity of 0 writes nothing. */
size_t afs_entry_comment(const uint8_t *blk, size_t bsize,
                         char *out, size_t cap);

/* Read the comment of a named entry in a directory. Returns 0 on success
 * (including "found, but it has no comment", which writes an empty string), or
 * non-zero if the name could not be resolved. A hard link resolves to its real
 * entry, so two names for one inode report the same comment. */
int afs_file_comment(afs_volume_t *v, uint32_t dirblock,
                     const char *name, size_t namelen,
                     char *out, size_t cap);

/* Read from a file given its HEADER BLOCK directly (no name lookup). This is
 * what the FUSE read path needs, because FUSE hands us an inode (block
 * number), not a path. `fileblock` may be a plain file header or a hard link
 * (resolved via real_entry). Reads past EOF are clamped; *got receives the
 * byte count. */
int afs_read_file_by_block(afs_volume_t *v, uint32_t fileblock,
                           void *out, size_t len, uint64_t offset, size_t *got);

/* --- D3 write phase ---
 *
 * Create-then-write, mirroring ADFlib's open-"w" semantics without copying
 * its API. `afs put` truncates an existing file (fopen "w"); create alone
 * still refuses a duplicate, like adfFileOpen "w". There is deliberately no
 * rename/delete yet, and no partial-range write: every write carries the
 * whole new content. */

/* Create an empty file in a directory: allocate a header block, write it
 * (byte_size 0, no data blocks -- the ADFlib issue #30 shape), link it into
 * the parent's hash chain, and add its dir-cache record on DIRC volumes.
 * Refuses invalid names and duplicates with named reasons. Sets *hdr_out. */
int afs_file_create(afs_volume_t *v, uint32_t dirblock,
                    const char *name, size_t namelen, uint32_t *hdr_out,
                    char *errbuf, size_t errlen);

/* Write `len` bytes into a file: on a fresh header this allocates and fills;
 * on a live file it truncates first (fopen "w" semantics -- grow, shrink, and
 * shrink-to-empty all rebuild exact tables). FFS data blocks are raw; OFS
 * data blocks carry the T_DATA header. A zero-length write stamps an empty
 * header. Refuses non-plain-file blocks (directories, links) with a named
 * reason. ENOSPC fails clean: deficit blocks are allocated before anything is
 * mutated, so a full volume keeps the old content intact. */
int afs_file_write_all(afs_volume_t *v, uint32_t hdrblock,
                       const void *data, size_t len,
                       char *errbuf, size_t errlen);

#endif /* AMIGAFS_FILE_H */