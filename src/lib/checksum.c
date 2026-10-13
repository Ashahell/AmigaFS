#include "amigafs/checksum.h"
#include "amigafs/be.h"

/* Normal block checksum: sum big-endian longs, treating the checksum long as
 * zero. The stored value must be the two's-complement negation of the sum. */
uint32_t afs_block_checksum(const uint8_t *blk, size_t len, size_t chk_off) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t v = be32(blk + i);
        if (i == chk_off) v = 0;   /* the checksum long counts as zero */
        sum += v;
    }
    return 0u - sum;               /* store the negation */
}

int afs_block_valid(const uint8_t *blk, size_t len, size_t chk_off) {
    /* sum INCLUDING the stored checksum; valid iff the total is zero */
    uint32_t total = 0;
    for (size_t i = 0; i + 4 <= len; i += 4) total += be32(blk + i);
    (void)chk_off;
    return total == 0u;
}

/* Bootblock checksum: 32-bit add with end-around carry (68000 bcc idiom), the
 * checksum long at byte 4 counted as zero, then bitwise NOT. */
uint32_t afs_bootblock_checksum(const uint8_t *boot, size_t len) {
    uint32_t total = 0;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t v = be32(boot + i);
        if (i == 4) v = 0;         /* the checksum long counts as zero */
        uint32_t before = total;
        total += v;
        if (total < before) total++;   /* end-around carry */
    }
    return ~total;
}

int afs_bootblock_valid(const uint8_t *boot, size_t len) {
    return be32(boot + 4) == afs_bootblock_checksum(boot, len);
}