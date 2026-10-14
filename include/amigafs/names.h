/* Name encoding conversion: AmigaDOS names are Latin-1 (ISO-8859-1), the
 * kernel/FUSE boundary is UTF-8. We convert at the boundary only; hashing and
 * comparison always happen in the original Latin-1 bytes, because the Amiga
 * hash function is defined over those exact bytes.
 */
#ifndef AMIGAFS_NAMES_H
#define AMIGAFS_NAMES_H

#include <stddef.h>

/* Convert `len` Latin-1 bytes to UTF-8 into `out` (capacity `cap`).
 * Returns the number of bytes written (excluding any NUL), or 0 if it does not
 * fit. `out` is NUL-terminated when cap > 0. */
size_t latin1_to_utf8(const char *in, size_t len, char *out, size_t cap);

/* Convert `len` UTF-8 bytes to Latin-1. Bytes that have no Latin-1
 * representation (U+0080..U+009F and anything above U+00FF) are written as
 * '?' rather than dropped, so the name stays the right length. Returns the
 * number of bytes written, or 0 if it does not fit. */
size_t utf8_to_latin1(const char *in, size_t len, char *out, size_t cap);

/* AmigaDOS name rules (FAQ §4.4): 1..30 bytes; ':' and '/' are forbidden
 * (they separate devices/volumes and directories). Returns non-zero when the
 * name is storable, 0 with no side effects otherwise. */
int afs_name_valid(const char *name, size_t len);

#endif /* AMIGAFS_NAMES_H */