#ifndef AMIGAFS_CHECKSUM_H
#define AMIGAFS_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

/* AmigaDOS uses TWO different checksums. Conflating them is a classic bug.

 * NORMAL block checksum (rootblock, file header, file extension, OFS data
 * block, dir-cache block, bitmap):
 *   sum every 32-bit big-endian long in the block, counting the checksum long
 *   itself as ZERO; the stored checksum must equal the two's-complement
 *   NEGATION of that sum. Equivalently, re-summing the block INCLUDING the
 *   stored checksum yields 0.
 *   The checksum long sits at byte offset 20 for the header family and byte
 *   offset 0 for bitmap blocks.
 *
 * BOOTBLOCK checksum (the boot block only):
 *   32-bit add WITH end-around carry across the bootblock (the 68000 `bcc`
 *   idiom), the checksum long at byte 4 counted as zero, then bitwise NOT.
 *   It is a separate function and never reuses the normal algorithm.
 */

/* Compute the normal checksum for a block of `len` bytes whose checksum long
 * is at byte offset `chk_off`. */
uint32_t afs_block_checksum(const uint8_t *blk, size_t len, size_t chk_off);

/* True if the block's stored checksum is correct. */
int afs_block_valid(const uint8_t *blk, size_t len, size_t chk_off);

/* Compute the bootblock checksum over `len` bytes (normally 1024), the
 * checksum long at byte 4 counted as zero. */
uint32_t afs_bootblock_checksum(const uint8_t *boot, size_t len);

/* True if the bootblock's stored checksum (byte 4) is correct. */
int afs_bootblock_valid(const uint8_t *boot, size_t len);

#endif /* AMIGAFS_CHECKSUM_H */