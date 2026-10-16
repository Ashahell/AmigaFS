#ifndef AMIGAFS_VERIFY_H
#define AMIGAFS_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "amigafs/volume.h"

/* The structure verifier behind `afs verify`.
 *
 * Checksums settle to ONE rule valid for every block type: the stored checksum
 * equals the negation of the sum of the OTHER longs, equivalently the sum
 * INCLUDING the stored value is 0. That single rule covers the checksum-at-20
 * family and bitmap blocks (checksum at byte 0) alike.
 *
 * The invariants checked are the ones that catch a damaged volume:
 * checksums, pointers in range, known block types, no block used twice,
 * dir-cache consistent with the hash table, terminating chains, and the
 * bitmap's own self-consistency (flag, page checksums, page count). Nothing
 * here mutates the image.
 *
 * --strict adds ONE further check: every block the tree references must be
 * marked USED in the allocation map. It is off by default because a stale map
 * is ORDINARY on a real Amiga disk -- of the eleven fixtures here, nine
 * disagree with their own bitmap while being otherwise perfectly healthy
 * (links.adf marks the live header of `file1` free). Reporting that as damage
 * would drown the real findings. It matters because it is exactly what a
 * WRITER depends on: allocating from such a map hands out a block that is
 * still in use, and the resulting corruption is silent, because every pointer
 * still resolves -- two entries merely come to claim the same block.
 */

typedef enum {
    AFS_PROBLEM_CHECKSUM = 0,   /* a block's stored checksum is wrong       */
    AFS_PROBLEM_RANGE,          /* a block pointer is outside the volume    */
    AFS_PROBLEM_TYPE,           /* a block has an unrecognised type         */
    AFS_PROBLEM_DOUBLE_USE,     /* the same physical block is used twice    */
    AFS_PROBLEM_BITMAP_MISMATCH,/* a block is REFERENCED by the tree but the
                                 * bitmap marks it free.                    */
                                 /* ONLY THIS DIRECTION IS CHECKED, and only
                                 * under AFS_VERIFY_STRICT. The other one --
                                 * marked used but referenced by nothing, i.e.
                                 * a LEAK -- is not detected by verify at all,
                                 * because a stale map is ordinary on a real
                                 * Amiga disk and this is a damage checker. A
                                 * writer that leaks must therefore assert on
                                 * the bitmap itself: see afs_link_remove's
                                 * header and tests/oracle/m32_link_unlink.sh.
                                 * (The old comment here claimed both
                                 * directions and was wrong; M32.) */
    AFS_PROBLEM_DIRCACHE,       /* the dir cache disagrees with the hash table*/
    AFS_PROBLEM_CHAIN,          /* a pointer chain does not terminate       */
    AFS_PROBLEM_ORPHAN,         /* block ALLOCATED but referenced by nothing */
    AFS_PROBLEM_ROOTBLOCK       /* the rootblock is malformed               */
} afs_problem_kind_t;

typedef struct {
    afs_problem_kind_t kind;
    uint32_t block;             /* the block the problem was found at */
    char     detail[120];       /* human-readable detail */
} afs_verify_report_t;

/* Verify an in-memory image of `size` bytes with `bsize`-byte blocks. Writes up
 * to `max` problems and returns how many were found (capped at max).
 * `flags` is a bitmask. */
#define AFS_VERIFY_STRICT 1

/* Also report ORPHANS: blocks the bitmap marks ALLOCATED that nothing in the
 * tree references. Opt-in for the same reason strict is -- see below.
 *
 * Strict checks the OTHER direction (referenced by the tree, marked free). This
 * one checks the reverse, and the asymmetry is deliberate in both cases:
 *
 *   - "referenced but marked free" means the MAP is stale. Ordinary on a real
 *     Amiga disk, so it cannot be a default.
 *   - "allocated but unreferenced" means a file was deleted and its blocks were
 *     never returned. ALSO ordinary on a real Amiga disk: AmigaDOS frees a
 *     deleted file's blocks only when an optimiser runs. So it cannot be a
 *     default either.
 *
 * Both are bugs on a volume THIS PROJECT BUILT, and that is the case the flag
 * exists for: the write fuzzer asserts after every operation that the volume is
 * still sound, and before M33 it could not see a leak at all -- which is how
 * M32's unlinked-link block leak survived a fuzzer that had been running for
 * months. Use it on volumes you wrote; leave it off for anything downloaded.
 *
 * MEASURED, not assumed -- how many orphans the fixtures have. (These are the
 * numbers AFTER the tree walk was completed to cover file data blocks; before
 * that it read higher, because unmarked data blocks were being counted as
 * orphans. `afs verify <img> --orphans` prints them.)
 *
 *     g1a30c 998   arccsh 550   testofs 303   testffs 238   testhd 223
 *     ffdisk0049 210   test_link_chains 29   win32-names 20   links 10
 *     blank 5       cache_crash 1
 *
 * Every non-hostile fixture has some, and two have hundreds. That is not damage:
 * it is what an AmigaDOS disk looks like after files have been deleted and no
 * optimiser has run. An everyday check that reported them would be unusable on
 * every real image, which is exactly why this is a flag and why the everyday
 * `afs verify` does not use it. The exclusion set is blocks 0 and 1 (the
 * bootblocks), which appear in no chain on any volume; nothing else is excused. */
#define AFS_VERIFY_ORPHANS 2

int afs_verify_memory_ex(const uint8_t *img, size_t size, size_t bsize,
                         afs_verify_report_t *out, int max, unsigned flags);

/* True when the volume's allocation map marks FREE a block its directory tree
 * references -- the state in which the allocator hands out a block already in
 * use, which is silent data loss. `*block_out` receives the first such block. A
 * volume that cannot be checked counts as disagreeing: "we could not check"
 * must not read as "we checked and it is fine". */
int afs_verify_map_disagrees(afs_volume_t *v, uint32_t *block_out);

/* The everyday check: structural damage, with strict mode OFF. Kept as its own
 * entry point (rather than as flags == 0 at every call site) so that the many
 * existing callers mean what they have always meant. */
int afs_verify_memory(const uint8_t *img, size_t size, size_t bsize,
                      afs_verify_report_t *out, int max);

#endif /* AMIGAFS_VERIFY_H */