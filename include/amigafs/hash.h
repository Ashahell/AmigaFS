#ifndef AMIGAFS_HASH_H
#define AMIGAFS_HASH_H

#include <stddef.h>
#include <stdint.h>

/* DOS type flag bits (the low three bits of the bootblock's first long). */
#define AFS_DOSFS_FFS      1u   /* 001 */
#define AFS_DOSFS_INTL     2u   /* 010 */
#define AFS_DOSFS_DIRCACHE 4u   /* 100 */

/* OFS is the absence of the FFS bit. Note this is a MASK TEST, not an
 * equality against 0: DOS\2 (OFS/INTL) and DOS\4 (OFS/DIRC&INTL) are both OFS.
 * (AROS's `afs` handler tests `dosflags == 0`, which misreads those two.) */
#define afs_is_ofs(flags)    (((flags) & AFS_DOSFS_FFS) == 0u)
#define afs_is_ffs(flags)    (((flags) & AFS_DOSFS_FFS) != 0u)
#define afs_has_intl(flags)  (((flags) & AFS_DOSFS_INTL) != 0u || \
                              ((flags) & AFS_DOSFS_DIRCACHE) != 0u)
#define afs_has_dircache(flags) (((flags) & AFS_DOSFS_DIRCACHE) != 0u)

/* Fold one byte to its Amiga "capital" form for case-insensitive comparison
 * and hashing. `flags` is the DOS type flag byte; values >= 2 additionally fold
 * the Amiga high range 224..254, except byte 247. */
uint8_t capitalch(uint8_t ch, uint8_t flags);

/* AmigaDOS name hash: h starts at the length, each byte folds and multiplies
 * by 13 while masking to 0x7FF, and the result is reduced modulo the table
 * size. `len` is the name length in bytes; only the first 30 are considered
 * (the Amiga name limit). */
uint32_t afs_hash(const char *name, size_t len, uint32_t table_size, uint8_t flags);

/* Case-insensitive name comparison using the same fold as afs_hash. */
int afs_name_equal(const char *a, size_t alen, const char *b, size_t blen,
                   uint8_t flags);

#endif /* AMIGAFS_HASH_H */