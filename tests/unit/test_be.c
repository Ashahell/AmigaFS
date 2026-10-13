/* Unit tests for unaligned-safe big-endian readers (be.c) and name conversion.
 *
 * These are the foundation every other module reads through. A bug here is a
 * silent misread of the whole disk, so the cases are deliberately exhaustive:
 * every byte position, every Latin-1 byte, and the alignment traps.
 */
#include "test.h"
#include "amigafs/be.h"
#include "amigafs/names.h"

static void test_be32_known(void) {
    const uint8_t b[4] = {0x12, 0x34, 0x56, 0x78};
    CHECK_EQ_U(be32(b), 0x12345678u, "be32 of 12 34 56 78");

    const uint8_t z[4] = {0x00, 0x00, 0x00, 0x00};
    CHECK_EQ_U(be32(z), 0u, "be32 of all zero");

    const uint8_t f[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ_U(be32(f), 0xFFFFFFFFu, "be32 of all ones");

    /* a real DOS type long, byte-for-byte as it appears on disk */
    const uint8_t dos[4] = {0x44, 0x4F, 0x53, 0x05};
    CHECK_EQ_U(be32(dos), 0x444F5305u, "be32 of on-disk 'DOS\\5'");
}

static void test_be32_unaligned(void) {
    /* real Amiga structures place fields at arbitrary byte offsets; a cast to
     * uint32_t* would be undefined and would trap on strict-alignment hosts */
    uint8_t buf[16];
    for (int i = 0; i < 16; i++) buf[i] = (uint8_t)i;

    for (int off = 0; off <= 12; off++) {
        uint32_t got = be32(buf + off);
        uint32_t want = ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1] << 16) |
                        ((uint32_t)buf[off + 2] << 8) | (uint32_t)buf[off + 3];
        CHECK_EQ_U(got, want, "be32 at byte offset");
    }
}

static void test_be16_known(void) {
    const uint8_t b[2] = {0xAB, 0xCD};
    CHECK_EQ_U(be16(b), 0xABCDu, "be16 of AB CD");

    const uint8_t z[2] = {0x00, 0x00};
    CHECK_EQ_U(be16(z), 0u, "be16 of all zero");
}

static void test_be16_unaligned(void) {
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)i;
    for (int off = 0; off <= 6; off++) {
        uint16_t got = be16(buf + off);
        uint16_t want = (uint16_t)(((uint16_t)buf[off] << 8) | buf[off + 1]);
        CHECK_EQ_U(got, want, "be16 at byte offset");
    }
}

static void test_bstr(void) {
    /* BCPL string: a single length byte then bytes, NOT NUL-terminated.
     * Amiga names are exactly this (max 30) and may contain no NUL. */
    const uint8_t raw[6] = {0x03, 'a', 'b', 'c', 0x00, 0x00};
    char out[8];
    size_t n = bstr(raw, 6, out, sizeof out);
    CHECK_EQ_U(n, 3u, "bstr length from the length byte");
    CHECK_EQ_S(out, "abc", "bstr content");

    /* a length byte larger than the buffer must be clamped, not trusted */
    const uint8_t over[8] = {0xFF, 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    size_t m = bstr(over, 8, out, sizeof out);
    CHECK(m < sizeof out, "bstr clamps an oversized length byte to capacity");
}

/* Latin-1 -> UTF-8. Amiga names are ISO-8859-1; the kernel boundary is UTF-8. */
static void test_latin1_to_utf8(void) {
    char out[8];
    size_t n;

    /* pure ASCII passes through (len excludes any NUL; cap must fit len+1) */
    char buf16[16];
    n = latin1_to_utf8("MOON.GIF", 8, buf16, sizeof buf16);
    CHECK_EQ_U(n, 8u, "latin1_to_utf8 ASCII byte count");
    CHECK_EQ_S(buf16, "MOON.GIF", "latin1_to_utf8 ASCII content");

    /* 0xE9 = e-acute, the character we saw in testofs.adf's "français" */
    n = latin1_to_utf8("\xE9", 1, out, sizeof out);
    CHECK_EQ_U(n, 2u, "latin1 0xE9 expands to two UTF-8 bytes");
    CHECK_EQ_U((unsigned char)out[0], 0xC3u, "latin1 0xE9 UTF-8 lead byte");
    CHECK_EQ_U((unsigned char)out[1], 0xA9u, "latin1 0xE9 UTF-8 trail byte");

    /* the high half must round-trip, except the C1 control range which we
     * deliberately map to '?' (they are not printable Latin-1 characters) */
    for (int c = 0x80; c <= 0xFF; c++) {
        if (c >= 0x80 && c <= 0x9F) continue;
        char in[1];
        in[0] = (char)c;
        char u[4];
        size_t k = latin1_to_utf8(in, 1, u, sizeof u);
        char back[4];
        size_t r = utf8_to_latin1(u, k, back, sizeof back);
        if (k != 2 || r != 1 || back[0] != in[0]) {
            CHECK(0, "latin1 round-trip for byte 0x%02X", c);
            return;
        }
    }
    CHECK(1, "latin1->utf8->latin1 round-trips for bytes 0xA0-0xFF");

    /* C1 controls 0x80-0x9F and any codepoint above 0xFF collapse to '?', and
     * collapse must NOT change the byte count */
    for (int c = 0x80; c <= 0x9F; c++) {
        char in[1];
        in[0] = (char)c;
        char u[4], back[4];
        size_t k = latin1_to_utf8(in, 1, u, sizeof u);
        size_t r = utf8_to_latin1(u, k, back, sizeof back);
        if (r != 1 || back[0] != '?') {
            CHECK(0, "C1 control 0x%02X maps to '?'", c);
            return;
        }
    }
    CHECK(1, "C1 controls 0x80-0x9F decode to '?' at the same byte length");
}

int main(void) {
    printf("be / names\n");
    test_be32_known();
    test_be32_unaligned();
    test_be16_known();
    test_be16_unaligned();
    test_bstr();
    test_latin1_to_utf8();
    TEST_REPORT();
}