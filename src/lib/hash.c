#include "amigafs/hash.h"

#include <string.h>

uint8_t capitalch(uint8_t ch, uint8_t flags) {
    /* flags 0 and 1 are the non-INTL filesystems: fold ASCII a-z only.
     * flags >= 2 are INTL (which DIRC implies) and also fold the Amiga
     * accented range 224..254, with byte 247 deliberately excluded. */
    int intl = (flags >= 2);
    if ((ch >= 'a' && ch <= 'z') ||
        (intl && ch >= 224 && ch <= 254 && ch != 247)) {
        return (uint8_t)(ch - ('a' - 'A'));
    }
    return ch;
}

uint32_t afs_hash(const char *name, size_t len, uint32_t table_size, uint8_t flags) {
    /* the Amiga name limit is 30 bytes; never hash past it */
    if (len > 30) len = 30;

    uint32_t h = (uint32_t)len;
    for (size_t i = 0; i < len; i++) {
        h = h * 13u + capitalch((uint8_t)name[i], flags);
        h &= 0x7FFu;                 /* mask INSIDE the loop */
    }
    if (table_size == 0) return 0;   /* never divide by zero */
    return h % table_size;
}

int afs_name_equal(const char *a, size_t alen, const char *b, size_t blen,
                   uint8_t flags) {
    if (alen > 30) alen = 30;
    if (blen > 30) blen = 30;
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        if (capitalch((uint8_t)a[i], flags) != capitalch((uint8_t)b[i], flags))
            return 0;
    }
    return 1;
}