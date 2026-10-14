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
 * volumes). An existing record for the same header is dropped and the new one
 * appended (possibly chaining a new block): no in-place patching, so a rename
 * cannot strand half a record. `comment` (NULL/0 = none, clamped to 79) is
 * stored with the record, which is how renames preserve comments. Every
 * touched block is restamped. */
int afs_dircache_upsert(afs_volume_t *v, uint32_t dirblock,
                        const afs_dircache_rec_t *rec,
                        const char *comment, size_t commentlen,
                        char *errbuf, size_t errlen);

/* Drop one dir-cache record by entry-block header, compacting the block
 * (later records slide down) and decrementing the count. Empty blocks stay
 * chained (valid, just wasteful -- matches ADFlib's tolerance, not its leak:
 * rmdir frees the whole chain explicitly). No-op on non-DIRC volumes and
 * when no record names that header. */
int afs_dircache_del(afs_volume_t *v, uint32_t dirblock, uint32_t header,
                     char *errbuf, size_t errlen);

/* Create a subdirectory. Refuses invalid names, duplicates (by the same
 * case-insensitive lookup a later `ls` will use), and unwritable parents with
 * named reasons. Sets *dir_out to the new directory's block. */
int afs_dir_mkdir(afs_volume_t *v, uint32_t dirblock,
                  const char *name, size_t namelen, uint32_t *dir_out,
                  char *errbuf, size_t errlen);

/* Unlink a name from its directory's hash chain (bucket head or chain
 * relink), restamping every touched block. The entry block itself is NOT
 * freed -- the caller owns what happens to the storage. Sets *block_out to
 * the unlinked block. Refuses missing names and corrupt chains with named
 * reasons (linking already refused those on the way in; unlinking must not
 * paper over them on the way out). */
int afs_dir_unlink(afs_volume_t *v, uint32_t dirblock,
                   const char *name, size_t namelen, uint32_t *block_out,
                   char *errbuf, size_t errlen);

/* Remove an empty subdirectory: refuse non-directories and non-empty
 * directories with named reasons, unlink, free its whole dir-cache chain
 * (empty dirs can still own cache blocks from deleted children), then free
 * the directory block itself. */
int afs_dir_rmdir(afs_volume_t *v, uint32_t dirblock,
                  const char *name, size_t namelen,
                  char *errbuf, size_t errlen);

/* Rename/move within one volume: rewrite the entry's name+parent in place,
 * unlink it from the source chain, append it to the destination chain with a
 * duplicate check, and move its dir-cache record. Same name in the same
 * directory is a no-op success (ADFlib parity). Refuses: invalid new names,
 * missing sources, occupied destinations (victim untouched), links (ADFlib
 * handles FILE/DIR only), and moves of a directory into its own subtree (a
 * cycle). Cross-partition moves do not reach here -- the caller resolves
 * volumes first and refuses those itself. */
int afs_dir_rename(afs_volume_t *v,
                   uint32_t src_dir, const char *oldname, size_t oldlen,
                   uint32_t dst_dir, const char *newname, size_t newlen,
                   char *errbuf, size_t errlen);

#endif /* AMIGAFS_DIR_H */