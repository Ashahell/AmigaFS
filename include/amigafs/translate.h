#ifndef AMIGAFS_TRANSLATE_H
#define AMIGAFS_TRANSLATE_H

#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

/* AFS attribute -> POSIX struct stat translation.
 *
 * PERMISSIONS (C10): the owner bits 0-3 (D=delete, E=exec, W=write, R=read) are
 * ACTIVE-LOW -- a set bit DENIES. Bits 4-7 (A=archive, P=pure, S=script,
 * Hold) and the group/other bits 8-15 are ACTIVE-HIGH. We invert only bits 0-3.
 *
 * DATES (D6): Amiga stores days since 1978-01-01, minutes past midnight, and
 * ticks of 1/50 second, with NO timezone. By default we emit UTC (deterministic
 * and matching common ADF tooling); tz=local is opt-in.
 *
 * IDS (D7): by default the mounting user's uid/gid are used; amiga_ids exposes
 * the stored 16-bit values.
 */

/* Translate Amiga protection bits into st_mode (including the type bits). */
void afs_translate_mode(uint32_t protect, struct stat *st, mode_t type);

/* Translate an Amiga timestamp (days, mins, ticks) into st_mtime (UTC). */
void afs_translate_mtime(uint32_t days, uint32_t mins, uint32_t ticks,
                         int local_tz, struct stat *st);

/* Apply the uid/gid policy: use (mount_uid, mount_gid) unless amiga_ids, in
 * which case use the stored (uid, gid). */
void afs_translate_ids(uint32_t mount_uid, uint32_t mount_gid,
                       uint32_t stored_uid, uint32_t stored_gid,
                       int amiga_ids, struct stat *st);

/* Current time as an Amiga timestamp: days since 1978-01-01, minutes past
 * midnight, ticks (1/50 s) past the minute. Ticks stay below 3000 by
 * construction, so the value fits the dir-cache record's signed 16-bit
 * fields. Before the Amiga epoch the fields read 0. */
void afs_now_amiga(uint32_t *days, uint32_t *mins, uint32_t *ticks);

#endif /* AMIGAFS_TRANSLATE_H */

/* Turn a library error message into the errno the kernel will see, and NOTHING
 * else. fuse_reply_err() carries an errno and nothing more, so at all 101 call
 * sites in the FUSE shell the message is DISCARDED. That is the FUSE protocol,
 * not an oversight -- but it means the library's diagnostics ("dir-cache chain
 * corrupt", "file too large (past 4 GiB)") reach nobody. Of the 32 distinct
 * messages the library can produce, 24 arrived as a bare EIO, which says
 * "something went wrong" and names nothing.
 *
 * EIO is therefore made to mean what it actually means -- unclassified -- and it
 * is the ONE result allowed to speak: it explains itself on stderr. Classified
 * outcomes stay silent, because a busy directory generates ENOENT and EEXIST
 * constantly and logging those would train the reader to ignore the channel.
 *
 * It lives in the library rather than in the FUSE shell because it is a pure
 * function with 101 call sites, and a function that cannot be linked by a test
 * is a function that is not tested. */
int afs_err_to_errno(const char *msg);
