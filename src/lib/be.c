#include "amigafs/be.h"

#include <string.h>

uint32_t be32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

uint16_t be16(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

void put_be32(void *p, uint32_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

void put_be16(void *p, uint16_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

size_t bstr(const uint8_t *src, size_t avail, char *out, size_t cap) {
    if (avail == 0 || cap == 0) {
        if (cap > 0) out[0] = '\0';
        return 0;
    }
    size_t len = src[0];
    size_t data_avail = avail - 1;          /* the length byte itself */
    if (len > data_avail) len = data_avail; /* never read past the buffer */
    if (cap > 0 && len > cap - 1) len = cap - 1; /* leave room for the NUL */
    if (len > 0) memcpy(out, src + 1, len);
    if (cap > 0) out[len] = '\0';
    return len;
}