#include "amigafs/names.h"

/* Latin-1 -> UTF-8: bytes < 0x80 map to themselves; bytes >= 0x80 become the
 * two-byte sequence 0xC0|(c>>6), 0x80|(c&0x3F). */
size_t latin1_to_utf8(const char *in, size_t len, char *out, size_t cap) {
    size_t need = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        need += (c < 0x80) ? 1 : 2;
    }
    if (cap == 0 || need + 1 > cap) return 0;

    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) {
            out[o++] = (char)c;
        } else {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = '\0';
    return o;
}

/* UTF-8 -> Latin-1. Handles the 1- and 2-byte forms that Latin-1 can express
 * (U+0000..U+00FF). C1 control codes U+0080..U+009F are technically
 * representable as bytes but are not characters we want to emit, so they and
 * everything above U+00FF become '?'. Invalid sequences also become '?', which
 * keeps the name length stable rather than truncating it. */
size_t utf8_to_latin1(const char *in, size_t len, char *out, size_t cap) {
    if (cap == 0 || len + 1 > cap) return 0;

    size_t o = 0, i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)in[i];
        unsigned int cp;
        size_t adv;

        if (c < 0x80) {
            cp = c; adv = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            cp = (unsigned int)(c & 0x1F) << 6;
            cp |= (unsigned int)((unsigned char)in[i + 1] & 0x3F);
            adv = 2;
        } else {
            cp = 0xFFFD; adv = 1; /* malformed: emit '?' below */
        }

        if (cp <= 0xFF && !(cp >= 0x80 && cp <= 0x9F)) {
            out[o++] = (char)cp;
        } else {
            out[o++] = '?';
        }
        i += adv;
    }
    out[o] = '\0';
    return o;
}