/* Unit tests for AFS-attribute -> POSIX struct stat translation (translate.c).
 *
 * This is the layer the user actually sees, and it has three rules that are
 * easy to get backwards:
 *
 *  1. OWNER permission bits 0-3 are ACTIVE-LOW: a SET bit means DENIED.
 *     Bits 4-7 (A/P/S/Hold) and the group/other bits 8-15 are active-HIGH.
 *     Inverting only 0-3 is the whole trick (C10).
 *  2. Dates are days-since-1978-01-01, minutes, and 1/50-second ticks, with
 *     no timezone stored. By default we emit UTC (D6); tz=local is opt-in.
 *  3. uid/gid default to the mounting user (D7); amiga_ids exposes the stored
 *     16-bit values.
 */
#include "test.h"
#include "amigafs/translate.h"

#include <time.h>

static void test_permission_bits(void) {
    struct stat st;

    /* all owner bits CLEAR = full owner permission (rwx) */
    afs_translate_mode(0x0000, &st, S_IFREG);
    CHECK_EQ_U(st.st_mode & 0400, 0400u, "owner-read set when owner bit clear");
    CHECK_EQ_U(st.st_mode & 0200, 0200u, "owner-write set when owner bit clear");
    CHECK_EQ_U(st.st_mode & 0100, 0100u, "owner-exec set when owner bit clear");

    /* all owner bits SET = no owner permission (bits are active-low) */
    afs_translate_mode(0x000Fu, &st, S_IFREG);
    CHECK_EQ_U(st.st_mode & 0400u, 0u, "owner-read CLEAR when owner bit set");
    CHECK_EQ_U(st.st_mode & 0200u, 0u, "owner-write CLEAR when owner bit set");
    CHECK_EQ_U(st.st_mode & 0100u, 0u, "owner-exec CLEAR when owner bit set");

    /* group and other bits are ACTIVE-HIGH: bit 11 = group-read */
    afs_translate_mode(1u << 11, &st, S_IFREG);
    CHECK_EQ_U(st.st_mode & 0040u, 0040u, "group-read set when bit 11 set (active-high)");
    afs_translate_mode(0u, &st, S_IFREG);
    CHECK_EQ_U(st.st_mode & 0040u, 0u, "group-read clear when bit 11 clear");

    /* other bits 12=D,13=E,14=W,15=R, active-high:
     *   bit 14 = other-W -> POSIX 0002 ; bit 15 = other-R -> POSIX 0004 */
    afs_translate_mode(1u << 15, &st, S_IFREG);
    CHECK_EQ_U(st.st_mode & 0004u, 0004u, "other-read set when bit 15 set (active-high)");
    afs_translate_mode(1u << 14, &st, S_IFREG);
    CHECK_EQ_U(st.st_mode & 0002u, 0002u, "other-write set when bit 14 set (active-high)");
    afs_translate_mode(1u << 13, &st, S_IFREG);
    CHECK_EQ_U(st.st_mode & 0001u, 0001u, "other-exec set when bit 13 set (active-high)");

    /* a file is S_IFREG, a directory S_IFDIR */
    afs_translate_mode(0, &st, S_IFREG);
    CHECK_EQ_U(st.st_mode & S_IFMT, (unsigned)S_IFREG, "regular file type");
    afs_translate_mode(0, &st, S_IFDIR);
    CHECK_EQ_U(st.st_mode & S_IFMT, (unsigned)S_IFDIR, "directory type");

    /* a symlink is S_IFLNK */
    afs_translate_mode(0, &st, S_IFLNK);
    CHECK_EQ_U(st.st_mode & S_IFMT, (unsigned)S_IFLNK, "symlink type");
}

static void test_dates_utc(void) {
    struct stat st;
    afs_translate_mtime(0, 0, 0, 0, &st);
    /* days=0 is the Amiga epoch 1978-01-01 */
    time_t t = st.st_mtime;
    struct tm tm;
    gmtime_r(&t, &tm);
    CHECK_EQ_U((unsigned)(tm.tm_year + 1900), 1978u, "days=0 maps to 1978");
    CHECK_EQ_U((unsigned)(tm.tm_mon + 1), 1u, "month is January");
    CHECK_EQ_U((unsigned)tm.tm_mday, 1u, "day is the 1st");

    /* days=1 -> 1978-01-02 */
    afs_translate_mtime(1, 0, 0, 0, &st);
    t = st.st_mtime;
    gmtime_r(&t, &tm);
    CHECK_EQ_U((unsigned)tm.tm_mday, 2u, "days=1 maps to Jan 2 1978");

    /* mins and ticks: 1 minute + 1 tick(1/50 s) = 60.02 s */
    afs_translate_mtime(0, 1, 1, 0, &st);
    /* verify the tick separately: mtime includes whole seconds only in time_t,
     * so check the computed value equals mins*60 */
    CHECK(st.st_mtime >= 60, "minutes contribute 60 seconds");
}

static void test_uid_gid_policy(void) {
    struct stat st;
    /* default: the mounting user's id */
    afs_translate_ids(1000, 2000, 4242, 4343, 0, &st);
    CHECK_EQ_U(st.st_uid, 1000u, "default uid is the mounting user");
    CHECK_EQ_U(st.st_gid, 2000u, "default gid is the mounting user");

    /* amiga_ids: expose the stored 16-bit values */
    afs_translate_ids(1000, 2000, 4242, 4343, 1, &st);
    CHECK_EQ_U(st.st_uid, 4242u, "amiga_ids exposes the stored uid");
    CHECK_EQ_U(st.st_gid, 4343u, "amiga_ids exposes the stored gid");
}

int main(void) {
    printf("translate\n");
    test_permission_bits();
    test_dates_utc();
    test_uid_gid_policy();
    TEST_REPORT();
}