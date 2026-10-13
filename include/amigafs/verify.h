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
 * used-vs-bitmap agreement, dir-cache consistent with the hash table, and
 * terminating chains. Nothing here mutates the image.
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
 * to `max` problems and returns how many were found (capped at max). */
int afs_verify_memory(const uint8_t *img, size_t size, size_t bsize,
                      afs_verify_report_t *out, int max);

#endif /* AMIGAFS_VERIFY_H */