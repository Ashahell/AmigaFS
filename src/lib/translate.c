#include "amigafs/translate.h"

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