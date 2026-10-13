#ifndef AMIGAFS_LINKS_H
#define AMIGAFS_LINKS_H

#include <stddef.h>
#include <stdint.h>

#include "amigafs/dir.h"
#include "amigafs/volume.h"

/* Link resolution.
 *
 * HARD links (an FFS feature). An entry of secondary type ST_LINKFILE (-4) or
 * ST_LINKDIR (4) carries `real_entry` pointing at the REAL file or directory
 * header. Every link in the real entry's next_link chain shares that one real
 * entry: ONE inode, an nlink count. This is what makes a hard-linked file read
 * identically through any of its names.
 *
 * A hard link to a DIRECTORY forms a cycle that the kernel dentry cache,
 * find(1) and du(1) cannot represent, so we never expose it as a traversable
 * directory -- it is reported by its on-disk kind (ST_LINKDIR) and translated
 * to a symlink by the caller (C12).
 *
 * SOFT links. The entry's hash-table area (offset 24) holds an Amiga path
 * C-string, which is NOT POSIX syntax (C11):
 *   `Vol:` / `:`  the volume root
 *   `/x`          from the volume root
 *   `//x`         also the volume root (Amiga collapses the double slash)
 *   `x`, `a/b`    relative to the directory holding the link
 * The emitted target is always RELATIVE to the mount root so readlink() can
 * emit it directly.
 */

/* Resolve a name that may be a file or a hard link to the real file header
 * block. Returns 0 and writes *out on success. */
int afs_link_resolve_file(afs_volume_t *v, uint32_t dirblock,
                          const char *name, size_t namelen, uint32_t *out);

/* Count the names sharing one inode: the real entry plus every entry in its
 * next_link chain. Returns 0 on success and writes the count (at least 1). */
int afs_file_nlink(afs_volume_t *v, uint32_t dirblock,
                   const char *name, size_t namelen, unsigned *out);

/* Read a soft link's target and translate it to a POSIX-relative path written
 * into `out`. Returns 0 on success. */
int afs_softlink_target(afs_volume_t *v, uint32_t dirblock,
                        const char *name, size_t namelen,
                        char *out, size_t cap);

#endif /* AMIGAFS_LINKS_H */