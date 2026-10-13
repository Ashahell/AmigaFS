#ifndef AMIGAFS_BE_H
#define AMIGAFS_BE_H

#include <stddef.h>
#include <stdint.h>

/* Unaligned-safe big-endian field readers and BCPL string helper.
 *
 * Every on-disk AmigaDOS field is big-endian. Fields sit at arbitrary byte
 * offsets inside blocks (hash tables, extension-block tails, dir-cache
 * records), so we NEVER cast a pointer to a uint32_t or uint16_t: we read the
 * bytes individually and assemble the value. That is both correct on
 * strict-alignment hosts and free of aliasing undefined behaviour.
 */

/* Read a 32-bit big-endian value. */
uint32_t be32(const void *p);

/* Read a 16-bit big-endian value. */
uint16_t be16(const void *p);

/* Write helpers, for symmetry with readers (write path comes later; these keep
 * the encoding explicit and testable now). */
void put_be32(void *p, uint32_t v);
void put_be16(void *p, uint16_t v);

/* Read a BCPL string: a single length byte then `avail` bytes of data (NOT
 * NUL-terminated). Copies into `out` (capacity `cap`), NUL-terminating for
 * convenience, and returns the logical length. If the length byte exceeds
 * what is available or the output capacity, it is clamped -- never trusted. */
size_t bstr(const uint8_t *src, size_t avail, char *out, size_t cap);

#endif /* AMIGAFS_BE_H */