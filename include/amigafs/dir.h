#ifndef AMIGAFS_DIR_H
#define AMIGAFS_DIR_H

#include <stddef.h>
#include <stdint.h>

#include "amigafs/blocktypes.h"
#include "amigafs/volume.h"

/* Directory traversal.
 *
 * The hash table is the AUTHORITY for both lookup and listing. The directory
 * cache (DIRC) exists only to speed up listing and is never consulted here, so
 * a stale or damaged cache cannot hide a file (C4). `dircache.c` parses and
 * verifies the cache for `afsverify` and may later serve as an optional fast
 * path.
 *
 * Traversal: bucket = hash(name); entry = ht[bucket]; follow `hash_chain`
 * for collisions; listing walks every bucket and every chain.
 *
 * ROBUSTNESS: every block pointer on this path comes from untrusted on-disk
 * data. Pointers are bounds-checked before use, and chains are bounded by the
 * volume's block count, so a corrupt image can neither hang the walk nor make
 * it read outside the volume.
 */

/* Maximum name length on disk (Amiga limit is 30; we allow the NUL). */
#define AFS_NAME_MAX 31

/* One directory entry as reported by listing/lookup. */
typedef struct {
    uint32_t block;                    /* the entry's own header block */
    char     name[AFS_NAME_MAX];       /* stored spelling, Latin-1 */
    uint8_t  kind;                     /* afs_block_kind_t */
} afs_dirent_t;

/* List a directory. `dirblock` is the directory's header block (for the root,
 * the rootblock). Writes up to `max` entries and sets *count.
 * Returns 0 on success, non-zero if the directory could not be read. */
int afs_dir_list(afs_volume_t *v, uint32_t dirblock,
                 afs_dirent_t *out, size_t max, size_t *count);

/* Look up one name in a directory using the Amiga hash and the same
 * case-insensitive comparison the hash uses. Returns 0 and fills *out on
 * success, non-zero if not found or on error. The returned name is the STORED
 * spelling, which may differ in case from the one asked for. */
int afs_dir_lookup(afs_volume_t *v, uint32_t dirblock,
                   const char *name, size_t namelen, afs_dirent_t *out);

/* --- D3 write phase ---
 *
 * Linking and cache maintenance shared by file create and mkdir. Unlike the
 * read path -- which ends a corrupt chain defensively -- these REFUSE with a
 * named reason: linking a new entry onto a corrupt chain would bake the
 * corruption into a volume we are supposed to be improving. */

/* Link an already-written entry block into a directory's hash chain (bucket
 * head or tail append) and write back every touched block restamped. The new
 * block's own hash_chain field must already be 0. */
int afs_dir_link_entry(afs_volume_t *v, uint32_t dirblock, uint32_t entryblock,
                       char *errbuf, size_t errlen);

/* Insert-or-refresh one dir-cache record for `dirblock` (no-op on non-DIRC
 * volumes). A record for the same header block is patched in place (size and
 * timestamp only -- comments and unknown bytes are preserved byte-for-byte);
 * otherwise the record is appended, allocating and linking a new cache block
 * when the last one is full. Every touched block is restamped. */
int afs_dircache_upsert(afs_volume_t *v, uint32_t dirblock,
                        const afs_dircache_rec_t *rec,
                        char *errbuf, size_t errlen);

/* Create a subdirectory. Refuses invalid names, duplicates (by the same
 * case-insensitive lookup a later `ls` will use), and unwritable parents with
 * named reasons. Sets *dir_out to the new directory's block. */
int afs_dir_mkdir(afs_volume_t *v, uint32_t dirblock,
                  const char *name, size_t namelen, uint32_t *dir_out,
                  char *errbuf, size_t errlen);

#endif /* AMIGAFS_DIR_H */