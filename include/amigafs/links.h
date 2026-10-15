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

/* The same count by HEADER BLOCK, for callers that hold a block rather than a
 * name -- the FUSE layer's stat path, which resolves an inode to a block and
 * never sees the directory entry it came from. Counts the real entry plus every
 * entry in its next_link chain, so it is the same number afs_file_nlink would
 * report for any of that inode's names. Returns 0 on success and writes at
 * least 1. */
int afs_nlink_for_block(afs_volume_t *v, uint32_t hdrblock, unsigned *out);

/* Pure translator: turn an Amiga softlink target string into a mount-relative
 * POSIX path. `depth` is how many directories deep the link sits (0 = root);
 * a relative multi-component target gains one "../" per level. */
int afs_softlink_translate(const char *raw, unsigned depth, char *out, size_t cap);

/* Read a soft link's target and translate it to a POSIX-relative path written
 * into `out`. Returns 0 on success. */
int afs_softlink_target(afs_volume_t *v, uint32_t dirblock,
                        const char *name, size_t namelen,
                        char *out, size_t cap);

/* Create a SOFT link (M31): a named pointer to a path, stored in the entry's
 * hash-table area.
 *
 * Layout taken from GROUND TRUTH rather than memory. Block 885 of testffs.adf
 * is a soft link written by a real AmigaDOS, and reads as
 *
 *     primary T_HEADER, secType ST_SOFTLINK (3)
 *     byte_size 0, extension 0, parent = the containing directory
 *     the target as plain ASCII at offset 24, ZERO-PADDED -- NOT NUL-terminated
 *
 * ADFlib's AdfEntryBlock agrees on the fields: secType at BSIZE-4, byte_size at
 * BSIZE-188, parent at BSIZE-12.
 *
 * The zero padding is the detail worth stating: for a 5-byte target, writing
 * "dir_1\0" is byte-identical to writing "dir_1" into a zeroed block, so a
 * short-target test passes either way and the difference only appears with a
 * longer target or a reader that scans past the end. The block is zeroed and
 * the target copied in, never NUL-appended.
 *
 * `target` is stored VERBATIM, in AmigaDOS path syntax ("Vol:/x", "/x", "a/b",
 * backslashes all legal). Translating it to a POSIX path is a READ-side
 * concern -- afs_softlink_target -- because the same bytes must round-trip to
 * whatever an Amiga reads back, and the mount needs a different answer.
 *
 * Refuses a duplicate name, an invalid name, an empty target and a target too
 * long for the block, each with a NAMED reason. `*link_out` receives the new
 * header block.
 */
int afs_symlink(afs_volume_t *v, uint32_t dirblock,
                const char *name, size_t namelen,
                const char *target, size_t targetlen,
                uint32_t *link_out, char *errbuf, size_t errlen);

/* Create a HARD link: a second directory name for an existing file (M24).
 *
 * The new block is its OWN entry header -- it does not share the target's
 * header -- carrying `real_entry` pointing at the target, so the two names
 * resolve to ONE inode with an nlink of 2. That is AmigaDOS's arrangement:
 * "A new file header block is added for each link created."
 *
 * Layout is taken from GROUND TRUTH rather than memory. Block 891 of links.adf,
 * written by a real AmigaDOS filesystem, reads as
 *
 *     primary T_HEADER, secType ST_LINKFILE (-4)
 *     real_entry (0x1d4) = the target's header block
 *     byte_size 0, no data blocks, extension 0, parent = the directory
 *
 * ADFlib's AdfEntryBlock puts realEntry at 0x1d4, parent at 0x1f4 and extension
 * at 0x1f8 -- BSIZE-12 and BSIZE-8, the offsets used here.
 *
 * The next_link chain (BSIZE-40) is hung off the TARGET, not off the link: both
 * of links.adf's links read FILE.next_link -> LINKFILE -> 0. afs_file_nlink
 * walks exactly that chain, so a link that was not appended would resolve to the
 * right file and still report nlink 1.
 *
 * FFS only -- hard links do not exist on an OFS volume, which has no link
 * secondary type, so writing one would produce a file no Amiga reads. Refuses a
 * duplicate name, an invalid name, an out-of-range target, and any non-file
 * target, each with a NAMED reason rather than half-written state; a directory
 * target is refused because a directory hard link is the cycle C12 exists to
 * avoid.
 *
 * `*link_out` receives the NEW header block. Unlinking the new name frees only
 * that block and leaves the target and its data untouched; afs_dir_unlink also
 * unhooks the chain so the name stops counting towards nlink. */
int afs_file_link(afs_volume_t *v, uint32_t dirblock,
                  const char *name, size_t namelen, uint32_t target,
                  uint32_t *link_out, char *errbuf, size_t errlen);

/* Called by afs_dir_unlink once a name has been spliced out of its directory,
 * so a removed hard link also leaves the real entry's next_link chain and stops
 * counting towards nlink. A no-op for anything that is not a hard link.
 *
 * Deliberately AFTER the splice: a chain failure then leaves the name already
 * gone, which is the safe direction -- an over-reported nlink is recoverable,
 * a name that survives an unlink is not. */
void afs_link_after_unlink(afs_volume_t *v, uint32_t entryblock);

#endif /* AMIGAFS_LINKS_H */