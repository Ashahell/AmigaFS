#ifndef AMIGAFS_VERIFY_H
#define AMIGAFS_VERIFY_H

#include <stddef.h>
#include <stdint.h>

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
    AFS_PROBLEM_BITMAP_MISMATCH,/* a block is used but marked free, or free
                                 * but marked used                          */
    AFS_PROBLEM_DIRCACHE,       /* the dir cache disagrees with the hash table*/
    AFS_PROBLEM_CHAIN,          /* a pointer chain does not terminate       */
    AFS_PROBLEM_ROOTBLOCK       /* the rootblock is malformed               */
} afs_problem_kind_t;

typedef struct {
    afs_problem_kind_t kind;
    uint32_t block;             /* the block the problem was found at */
    char     detail[120];       /* human-readable detail */
} afs_verify_report_t;

/* Verify an in-memory image of `size` bytes with `bsize`-byte blocks. Writes up
 * to `max` problems and returns how many were found (capped at max).
 * `flags` is a bitmask; the only bit defined is AFS_VERIFY_STRICT. */
#define AFS_VERIFY_STRICT 1

int afs_verify_memory_ex(const uint8_t *img, size_t size, size_t bsize,
                         afs_verify_report_t *out, int max, unsigned flags);

/* The everyday check: structural damage, with strict mode OFF. Kept as its own
 * entry point (rather than as flags == 0 at every call site) so that the many
 * existing callers mean what they have always meant. */
int afs_verify_memory(const uint8_t *img, size_t size, size_t bsize,
                      afs_verify_report_t *out, int max);

#endif /* AMIGAFS_VERIFY_H */