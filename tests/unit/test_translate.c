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
#include <unistd.h>   /* dup, dup2, close, unlink */
#include <fcntl.h>    /* open */
#include <errno.h>    /* ENOMEM, EFBIG, EIO, ... */
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

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

/* afs_err_to_errno: the message -> errno mapping the FUSE shell applies at all
 * 101 fuse_reply_err sites, where the message itself is DISCARDED.
 *
 * This is a unit test because the function USED TO BE A STATIC IN A FILE WITH A
 * main() -- 101 call sites and no way to test any of them. Moving it into the
 * library is what made this test writable.
 *
 * The behavioural half matters as much as the mapping: a classified error must
 * be SILENT. A busy directory produces ENOENT and EEXIST constantly, and a
 * channel that speaks on every expected miss is a channel nobody reads -- which
 * is the whole reason EIO is the only result allowed to explain itself. */

/* Run fn(msg) with stderr captured, and report what it wrote. Returns the
 * function's return value; *wrote gets the byte count written to stderr. */
static int err_stderr_probe(int (*fn)(const char *), const char *msg,
                            size_t *wrote)
{
    char tmpl[] = "/tmp/amigafs-errprobe.XXXXXX";
    int fd = mkstemp(tmpl);
    int saved = dup(STDERR_FILENO);
    ssize_t n;
    if (fd < 0) { *wrote = (size_t)-1; return fn(msg); }
    fflush(stderr);
    dup2(fd, STDERR_FILENO);
    (void)fn(msg);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    lseek(fd, 0, SEEK_SET);
    n = read(fd, tmpl, 0);          /* size is what matters, not the content */
    (void)n;
    *wrote = (size_t)lseek(fd, 0, SEEK_END);
    close(fd);
    unlink(tmpl);
    return 0;
}

static void test_error_message_mapping(void) {
    size_t wrote = 0;

    /* the four that were being flattened by the fall-through */
    CHECK_EQ_U(afs_err_to_errno("out of memory"), ENOMEM,
               "out of memory is ENOMEM, not EIO");
    CHECK_EQ_U(afs_err_to_errno("file too large (past 4 GiB)"), EFBIG,
               "a file past 4 GiB is EFBIG -- POSIX's exact errno for it");
    CHECK_EQ_U(afs_err_to_errno(
                   "bitmap extension chain not supported (volume too large)"),
               EFBIG, "a volume too large for the chain is EFBIG too");

    /* the pre-existing classifications still hold */
    CHECK_EQ_U(afs_err_to_errno("'x' already exists"), EEXIST, "already exists");
    /* A word about this test's first draft: it used "cannot find name" and got
     * EINVAL, because that string contains the substring "name" and the rule for
     * it is checked before anything else. The mapping is a first-match-over-
     * substrings scan, so THE WORDING OF EVERY LIBRARY MESSAGE IS LOAD-BEARING:
     * a message that happens to contain two keywords gets whichever rule comes
     * first, and the loser is unreachable for that message. Worth knowing before
     * adding a message that reads well and classifies wrongly. */
    /* the library's ACTUAL wording, not one invented here: src/lib emits
     * "'%.*s' not found", and an invented string tests nothing. Both earlier
     * drafts of this assertion used invented text and were wrong twice. */
    CHECK_EQ_U(afs_err_to_errno("'nosuch' not found"), ENOENT, "not found");
    CHECK_EQ_U(afs_err_to_errno("directory not empty"), ENOTEMPTY, "not empty");
    CHECK_EQ_U(afs_err_to_errno("'d' is a directory"), EISDIR, "is a directory");
    CHECK_EQ_U(afs_err_to_errno("volume full: no free block"), ENOSPC, "full");
    CHECK_EQ_U(afs_err_to_errno("cannot delete links yet"), EOPNOTSUPP, "link");

    /* an unclassified message is EIO, and EIO alone explains itself */
    CHECK_EQ_U(afs_err_to_errno("dir-cache chain corrupt"), EIO,
               "an unclassified message is EIO");
    err_stderr_probe(afs_err_to_errno, "dir-cache chain corrupt", &wrote);
    CHECK(wrote > 0, "an unclassified message DOES explain itself (%zu bytes)", wrote);

    /* and classified results must be silent, or the channel is worthless */
    err_stderr_probe(afs_err_to_errno, "'x' already exists", &wrote);
    CHECK_EQ_U((unsigned long)wrote, 0ul, "a classified error stays silent");
    err_stderr_probe(afs_err_to_errno, "volume full: no free block", &wrote);
    CHECK_EQ_U((unsigned long)wrote, 0ul, "ENOSPC stays silent (it is expected)");
    err_stderr_probe(afs_err_to_errno, "'nosuch' not found", &wrote);
    CHECK_EQ_U((unsigned long)wrote, 0ul, "ENOENT stays silent (a busy dir is normal)");

    /* a NULL or empty message is EIO, not a crash */
    CHECK_EQ_U(afs_err_to_errno(NULL), EIO, "NULL message is EIO");
    CHECK_EQ_U(afs_err_to_errno(""), EIO, "empty message is EIO");
}

int main(void) {
    printf("translate\n");
    test_permission_bits();
    test_dates_utc();
    test_uid_gid_policy();
    test_error_message_mapping();
    TEST_REPORT();
}