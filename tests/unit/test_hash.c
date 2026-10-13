/* Unit tests for the AmigaDOS name hash and the INTL case-fold (hash.c).
 *
 * The hash is the index into a directory's hash table, so a wrong hash means
 * a name that exists becomes unfindable. Two details are easy to get wrong:
 *
 *  1. The accumulator is masked to 0x7FF *inside* the loop, not just at the
 *     end, and the result is then taken modulo the table size.
 *  2. Case folding is flag-dependent. For flags 0/1 (no INTL) only a-z fold.
 *     For flags >= 2 (INTL, which DIRC implies) the Amiga high-byte range
 *     224..254 folds too, EXCEPT byte 247. DIRC volumes (flags 4/5) must fold
 *     exactly like INTL.
 *
 * Golden values were computed independently and include a real name taken from
 * the INTL fixture testofs.adf ("français", which hashes DIFFERENTLY under INTL
 * than under a plain ASCII fold -- the bug this guards against).
 */
#include "test.h"
#include "amigafs/hash.h"

/* table size for the usual 512-byte block: BSIZE/4 - 56 = 72 */
#define TAB512 72u

static void test_capitalch_plain(void) {
    /* flags 0 and 1: only ASCII a-z fold */
    for (int flags = 0; flags <= 1; flags++) {
        CHECK_EQ_U(capitalch('a', (uint8_t)flags), 'A', "lowercase folds");
        CHECK_EQ_U(capitalch('z', (uint8_t)flags), 'Z', "z folds");
        CHECK_EQ_U(capitalch('A', (uint8_t)flags), 'A', "uppercase unchanged");
        CHECK_EQ_U(capitalch('0', (uint8_t)flags), '0', "digit unchanged");
        CHECK_EQ_U(capitalch(224, (uint8_t)flags), 224, "high byte NOT folded without INTL");
    }
}

static void test_capitalch_intl(void) {
    /* flags 2..5: the Amiga high range folds, except 247 */
    for (int flags = 2; flags <= 5; flags++) {
        CHECK_EQ_U(capitalch(224, (uint8_t)flags), 192, "224 folds to 192 under INTL");
        CHECK_EQ_U(capitalch(254, (uint8_t)flags), 222, "254 folds to 222 under INTL");
        CHECK_EQ_U(capitalch(223, (uint8_t)flags), 223, "223 is below the range, unchanged");
        CHECK_EQ_U(capitalch(255, (uint8_t)flags), 255, "255 is above the range, unchanged");
        CHECK_EQ_U(capitalch(247, (uint8_t)flags), 247, "247 is the explicit exception");
    }
}

static void test_dirc_folds_like_intl(void) {
    /* DIRC implies INTL: flags 4 and 5 must behave identically to flag 2.
     * Checked for all 256 byte values, reported once. */
    for (int c = 0; c < 256; c++) {
        uint8_t want = capitalch((uint8_t)c, 2);
        if (capitalch((uint8_t)c, 4) != want || capitalch((uint8_t)c, 5) != want) {
            CHECK(0, "flags 4/5 diverge from INTL at byte 0x%02X", c);
            return;
        }
    }
    CHECK(1, "DIRC flag classes fold identically to INTL for all 256 bytes");
}

static void test_hash_golden(void) {
    /* values computed independently for a 512-byte block (72 entries) */
    CHECK_EQ_U(afs_hash("MOON.GIF", 8, TAB512, 0), 17u, "hash of MOON.GIF, no INTL");
    CHECK_EQ_U(afs_hash("MOON.GIF", 8, TAB512, 2), 17u, "hash of MOON.GIF, INTL");
    CHECK_EQ_U(afs_hash("testofs", 7, TAB512, 0), 47u, "hash of testofs, no INTL");
    CHECK_EQ_U(afs_hash("testofs", 7, TAB512, 2), 47u, "hash of testofs, INTL");
}

static void test_hash_intl_differs_for_accents(void) {
    /* "français" contains 0xE7 (c-cedilla). Under a plain ASCII fold it
     * hashes to 71; under INTL it hashes to 47. This is the specific bug the
     * INTL fold prevents: with the wrong fold, the name lands in the wrong
     * bucket and cannot be found. */
    const unsigned char name[] = "fran\xe7" "ais";
    size_t len = sizeof name - 1;
    CHECK_EQ_U(afs_hash((const char *)name, len, TAB512, 0), 71u,
               "français WITHOUT INTL folds the high byte (hash 71)");
    CHECK_EQ_U(afs_hash((const char *)name, len, TAB512, 2), 47u,
               "français WITH INTL folds c-cedilla (hash 47)");
    CHECK(afs_hash((const char *)name, len, TAB512, 0) !=
              afs_hash((const char *)name, len, TAB512, 2),
          "the two foldings genuinely differ for this name");
}

static void test_hash_properties(void) {
    /* result must always land inside the table */
    for (int t = 1; t <= 200; t += 47) {
        uint32_t h = afs_hash("SomeName", 8, (uint32_t)t, 2);
        CHECK(h < (uint32_t)t, "hash stays within the table size");
        if (t_failures) return;
    }
    /* case-insensitivity: upper/lower must land in the same bucket */
    CHECK_EQ_U(afs_hash("README", 6, TAB512, 2), afs_hash("readme", 6, TAB512, 2),
               "hash is case-insensitive under INTL");
    CHECK_EQ_U(afs_hash("README", 6, TAB512, 0), afs_hash("readme", 6, TAB512, 0),
               "hash is case-insensitive without INTL");

    /* a zero-size table must not divide by zero */
    uint32_t hz = afs_hash("x", 1, 0, 0);
    CHECK_EQ_U(hz, 0u, "zero-size table yields 0 rather than dividing by zero");
}

static void test_hash_name_equal(void) {
    /* comparison uses the same fold, so it must agree with the hash */
    const unsigned char a[] = "Fran\xe7" "ais";
    const unsigned char b[] = "FRAN\xe7" "AIS";
    CHECK(afs_name_equal((const char *)a, 8, (const char *)b, 8, 2),
          "accented names compare equal under INTL");
    /* AmigaDOS names are case-PRESERVING but case-INSENSITIVE, so names that
     * differ only in case are the SAME entry. This is why the mount uses
     * exact-match lookup (D5) -- the filesystem itself cannot tell them apart. */
    CHECK(afs_name_equal("MOON.GIF", 8, "moon.gif", 8, 0),
          "names differing only in case are equal (Amiga is case-insensitive)");
    CHECK(!afs_name_equal("MOON.GIF", 8, "MOON.GIX", 8, 0),
          "genuinely different names compare unequal");
}

int main(void) {
    printf("hash\n");
    test_capitalch_plain();
    test_capitalch_intl();
    test_dirc_folds_like_intl();
    test_hash_golden();
    test_hash_intl_differs_for_accents();
    test_hash_properties();
    test_hash_name_equal();
    TEST_REPORT();
}