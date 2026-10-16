#include "amigafs/translate.h"

#include <time.h>

/* Seconds between the Unix epoch (1970-01-01) and the Amiga epoch
 * (1978-01-01). Verified exactly: 1978-01-01T00:00:00Z is 252460800. */
#define AMIGA_EPOCH_OFFSET (252460800L)
/* Ticks per second in AmigaDOS. */
#define TICKS_PER_SEC 50u

void afs_translate_mode(uint32_t protect, struct stat *st, mode_t type) {
    /* The owner permission bits are 0-3 = D,E,W,R and are ACTIVE-LOW.
     *   bit 0 D (delete forbidden)  -- no direct POSIX equivalent
     *   bit 1 E (not executable)
     *   bit 2 W (not writable)
     *   bit 3 R (not readable)
     * Group/other bits 8-15 mirror the same set ACTIVE-HIGH. */
    mode_t m = type | 0000;   /* start from the type bits */

    /* owner: bit 3 R, bit 2 W, bit 1 E are active-LOW (bit 0 D has no POSIX
     * equivalent and is intentionally dropped) */
    m |= (protect & 0x8) ? 0 : 0400;   /* R active-low */
    m |= (protect & 0x4) ? 0 : 0200;   /* W active-low */
    m |= (protect & 0x2) ? 0 : 0100;   /* E active-low */

    /* group: bits 8-15 active-high, layout D,E,W,R at 8,9,10,11 */
    m |= (protect & (1u << 9))  ? 0040 : 0;   /* group E -> x */
    m |= (protect & (1u << 10)) ? 0020 : 0;   /* group W -> w */
    m |= (protect & (1u << 11)) ? 0040 : 0;   /* group R -> r */

    /* other: bits 12-15, layout D,E,W,R at 12,13,14,15 */
    m |= (protect & (1u << 13)) ? 0001 : 0;   /* other E -> x */
    m |= (protect & (1u << 14)) ? 0002 : 0;   /* other W -> w */
    m |= (protect & (1u << 15)) ? 0004 : 0;   /* other R -> r */

    st->st_mode = m;
}

void afs_translate_mtime(uint32_t days, uint32_t mins, uint32_t ticks,
                         int local_tz, struct stat *st) {
    time_t t = (time_t)(days * 86400u + mins * 60u + ticks / TICKS_PER_SEC)
               + (time_t)AMIGA_EPOCH_OFFSET;
    (void)local_tz;   /* UTC by default (D6); local_tz reserved for -o tz=local */
    st->st_mtime = t;
    st->st_atime = t;
    st->st_ctime = t;
}

void afs_translate_ids(uint32_t mount_uid, uint32_t mount_gid,
                       uint32_t stored_uid, uint32_t stored_gid,
                       int amiga_ids, struct stat *st) {
    if (amiga_ids) {
        st->st_uid = (uid_t)stored_uid;
        st->st_gid = (gid_t)stored_gid;
    } else {
        st->st_uid = (uid_t)mount_uid;
        st->st_gid = (gid_t)mount_gid;
    }
}

void afs_now_amiga(uint32_t *days, uint32_t *mins, uint32_t *ticks) {
    time_t now = time(NULL);
    long since;
    if (days) *days = 0;
    if (mins) *mins = 0;
    if (ticks) *ticks = 0;
    if (now == (time_t)-1) return;
    since = (long)now - AMIGA_EPOCH_OFFSET;
    if (since < 0) return;
    /* 64-bit arithmetic: days * 86400 overflows 32 bits within a year. */
    if (days) *days = (uint32_t)((uint64_t)since / 86400u);
    if (mins) *mins = (uint32_t)(((uint64_t)since % 86400u) / 60u);
    if (ticks) *ticks = (uint32_t)(((uint64_t)since % 60u) * TICKS_PER_SEC);
}
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* Turn a library message into the errno the kernel will see, and NOTHING else.
 *
 * fuse_reply_err() carries an errno and nothing more, so at all 101 call sites
 * the message is DISCARDED. That is the FUSE protocol, not an oversight -- but it
 * means the library's careful diagnostics ("dir-cache chain corrupt", "file too
 * large (past 4 GiB)") reach nobody. Measured over the 32 distinct messages the
 * library can produce, 24 arrive as a bare EIO, which says "something went wrong"
 * and names nothing.
 *
 * So EIO is made to mean what it actually means -- "unclassified" -- and it is the
 * ONE result that is allowed to speak. Classified outcomes stay silent, because a
 * busy directory generates ENOENT and EEXIST constantly and logging those would
 * train the reader to ignore the channel. EIO is the bucket where this code did
 * not understand its own failure, and that is exactly when a human is needed.
 *
 * Four messages have an unambiguous errno and were being flattened by the
 * fall-through: out of memory is ENOMEM, and the two "too large" cases are EFBIG,
 * which is POSIX's exact errno for them. */
int afs_err_to_errno(const char *msg) {
    if (!msg || !msg[0]) return EIO;
    if (strstr(msg, "already exists"))   return EEXIST;
    if (strstr(msg, "not empty"))       return ENOTEMPTY;
    if (strstr(msg, "not found"))       return ENOENT;
    if (strstr(msg, "is a directory"))  return EISDIR;
    if (strstr(msg, "not a directory")) return ENOTDIR;
    if (strstr(msg, "into itself"))     return EINVAL;
    if (strstr(msg, "link"))            return EOPNOTSUPP;
    if (strstr(msg, "name"))            return EINVAL;
    if (strstr(msg, "full") || strstr(msg, "no free block")) return ENOSPC;
    if (strstr(msg, "out of memory"))   return ENOMEM;
    if (strstr(msg, "too large"))       return EFBIG;

    /* Unclassified: say so, once, with the reason. */
    fprintf(stderr, "amigafs: %s (reported as I/O error)\n", msg);
    return EIO;
}
