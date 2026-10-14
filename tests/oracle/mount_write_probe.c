/* mount_write_probe -- drive a LIVE amigafs `-o rw` mount with a random write
 * workload and check it against an in-memory model.
 *
 * WHY THIS EXISTS. mount_probe.c covers the shell's READ arithmetic under
 * hostile images; tests/write_fuzz.c covers the LIBRARY's write path against a
 * model. Between them sits one layer with no coverage: the FUSE write ops
 * themselves -- create, write, setattr(size), mkdir, unlink, rmdir, rename,
 * the O_APPEND redirect, the Amiga protection-bit check, and the reply types
 * (two of which, `fuse_reply_entry` where `fuse_reply_create` was owed and
 * `fuse_reply_err(0)` where `fuse_reply_attr` was owed, both failed as
 * "fuse: writing device: Invalid argument" from the reply WRITER rather than
 * from the operation). Those are only reachable through a real mount, because
 * `fuse_req_t` is opaque and libfuse's custom-I/O path is compiled out of
 * distro builds.
 *
 * WHAT IS ASSERTED:
 *
 *   1. DIFFERENTIAL SELF-CONSISTENCY. Every operation the mount ACCEPTS must
 *      be observable through the mount afterwards: a created file reads back
 *      with the bytes written, a rename moves the content, a truncate changes
 *      the size, an unlink is gone. And every operation it REFUSES must leave
 *      the volume exactly as it was -- so the model is only updated on
 *      success, and a refusal followed by a read-back mismatch is a failure.
 *      This does NOT require the volume to be healthy, which matters because
 *      the gate mounts deliberately mutated images: a corrupt hash chain is
 *      allowed to refuse everything, and is.
 *
 *   2. ERRNO DISCIPLINE. Every errno observed must be one the design
 *      sanctions. EROFS is explicitly NOT sanctioned on an `-o rw` mount --
 *      seeing it means the read-only path fired anyway. EPERM is sanctioned
 *      (chmod/utimes have nowhere to live), as are the structural refusals.
 *
 *   3. NO CRASH / NO SANITIZER REPORT. Checked by the gate from the mount's
 *      stderr; the probe cannot see it.
 *
 * It prints its tallies so the gate can enforce a coverage floor:
 *
 *     wprobe: ops=N create=N ... accepted=N refused=N dirhard=N
 *
 * Without those counts a run where every image was refused at the DOS id check
 * reports "0 failures" and proves nothing -- the failure mode this project has
 * already hit twice.
 *
 *   usage: mount_write_probe <mountpoint> <iterations> [seed]
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>
#include <unistd.h>

/* Sized so a run cannot saturate the floppy and stop exercising anything. An
 * unbounded run filled the 1756-block volume within ~150 operations, after
 * which every operation needing even one block was refused and the differential
 * check compared nothing -- 128 refusals out of 129 operations, reported as a
 * clean run. */
#define MAX_ENT 24
#define MAX_DATA 2000

/* Path widths, chosen so truncation is IMPOSSIBLE rather than merely
 * unlikely -- the compiler is right to warn when `dir` + '/' + `name`
 * can exceed `rel`. MAX_NEST caps how deep the probe nests, so a
 * directory path never exceeds MAX_NEST * (30 + 1) bytes; the buffers
 * are sized well above that bound and a runtime guard refuses to nest
 * past it. AmigaDOS names are at most 30 bytes, so this is the format's
 * own limit doing the work. */
#define MAX_NEST 4
#define MPATH 256
#define MFULL 320

/* One tracked entry. `block` is never learned from the mount -- the mount
 * namespace is POSIX and has no block numbers -- so everything here is keyed
 * by PATH, and the model is checked by reading the path back. */
struct ent {
    int  in_use;
    int  is_dir;
    int  is_root;      /* the scratch root: never removable, see do_rmdir */
    char path[MFULL];      /* relative to the scratch root, no leading slash */
    char name[64];
    char dir[MPATH];
    unsigned char *data;
    size_t len;
};

static struct ent g_ent[MAX_ENT];

/* Names that have been deleted, kept so create can deliberately REUSE one.
 *
 * This is the case that finds the stale-inode hazard: the shell's inode NUMBER
 * is a block number, so after a name is unlinked and then created again the
 * kernel can still be holding the old inode -- and it points at the block the
 * old file's header used to be in. With long dentry timeouts the write lands
 * on a block that is no longer a file, and the shell refuses it with EIO. A
 * probe that only ever invents fresh names never reproduces that, which is
 * exactly why the bug survived the first pass of this lane. */
#define GRAVE 12
static char g_grave[GRAVE][64];
static int g_ngrave;
static unsigned long c_ops, c_accepted, c_refused;
static unsigned long c_tried[8], c_ok[8];
enum { OP_CREATE, OP_WRITE, OP_APPEND, OP_TRUNC, OP_RENAME, OP_UNLINK,
       OP_MKDIR, OP_RMDIR, OP_COUNT };
static const char *op_name[OP_COUNT] = {
    "create", "write", "append", "trunc", "rename", "unlink", "mkdir", "rmdir"
};

static unsigned g_seed = 1;
static unsigned failures;
static char g_root[3072];
static char g_scratch[4096];      /* g_root + "/.wprobe" */

static int g_trace;        /* WPROBE_TRACE=1 -> narrate every operation */
static int g_allow_eio;    /* --lenient: EIO is a designed refusal on a damaged
                            * volume, and the rw lane runs against deliberately
                            * mutated images. The strict pass (unmutated
                            * images) forbids it, which is the pass that would
                            * notice a healthy volume answering EIO. */
static unsigned g_iter;   /* so a failure names where in the run it happened */
static const char *g_op = "?";

static void fail(const char *what, const char *detail) {
    fprintf(stderr, "wprobe: iter %u op %s: %s: %s\n", g_iter, g_op,
            what, detail ? detail : "");
    failures++;
}

/* deterministic RNG: a failing run must be reproducible from its seed */
static uint32_t rnd(uint32_t n) {
    g_seed = g_seed * 1103515245u + 12345u;
    return n ? ((g_seed >> 16) % n) : 0u;
}

/* Copy with a refusal on overflow: 0 on success, -1 if it would not fit. */
static int setstr(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n + 1 > cap) return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

/* dir + '/' + name, refusing rather than truncating. */
static int join_path(char *dst, size_t cap, const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    size_t sep = dl ? 1u : 0u;
    if (dl + sep + nl + 1 > cap) return -1;
    memcpy(dst, dir, dl);
    if (sep) dst[dl] = '/';
    memcpy(dst + dl + sep, name, nl);
    dst[dl + sep + nl] = '\0';
    return 0;
}

static void path_of(char *out, size_t cap, const char *dir, const char *name) {
    if (dir && dir[0]) snprintf(out, cap, "%s/%s", g_scratch, dir);
    else               snprintf(out, cap, "%s", g_scratch);
    if (name && name[0]) {
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "/%s", name);
    }
}

static struct ent *find_path(const char *p) {
    for (int i = 0; i < MAX_ENT; i++)
        if (g_ent[i].in_use && strcmp(g_ent[i].path, p) == 0) return &g_ent[i];
    return NULL;
}

static struct ent *alloc_ent(void) {
    for (int i = 0; i < MAX_ENT; i++)
        if (!g_ent[i].in_use) return &g_ent[i];
    return NULL;
}

static struct ent *random_file(void) {
    unsigned n = 0;
    for (int i = 0; i < MAX_ENT; i++)
        if (g_ent[i].in_use && !g_ent[i].is_dir) n++;
    if (!n) return NULL;
    unsigned pick = rnd(n);
    for (int i = 0; i < MAX_ENT; i++) {
        if (!g_ent[i].in_use || g_ent[i].is_dir) continue;
        if (pick-- == 0) return &g_ent[i];
    }
    return NULL;
}

static struct ent *random_dir(void) {
    unsigned n = 0;
    for (int i = 0; i < MAX_ENT; i++)
        if (g_ent[i].in_use && g_ent[i].is_dir) n++;
    if (!n) return NULL;
    unsigned pick = rnd(n);
    for (int i = 0; i < MAX_ENT; i++) {
        if (!g_ent[i].in_use || !g_ent[i].is_dir) continue;
        if (pick-- == 0) return &g_ent[i];
    }
    return NULL;
}

/* May we create inside `dir`? Nesting is capped so path lengths stay far below
 * the modelled buffers -- see the MPATH/MFULL note. */
static int may_nest(const char *dir) {
    size_t depth = 0;
    for (const char *p = dir; *p; p++) if (*p == '/') depth++;
    return depth < (size_t)MAX_NEST;
}

/* Is `child` strictly inside `dir`? The root's path is "", and a file
 * directly inside it has path "name" with NO leading slash -- so the obvious
 * "look for dir's length then expect '/'" test never matches, and the scratch
 * root looked permanently empty. That produced an unexplained EIO on rmdir:
 * the mount correctly refused to remove a directory that had files in it, and
 * the model insisted it was empty. */
static int is_child_of(const char *child, const char *dir) {
    size_t dl = strlen(dir);
    if (dl == 0) return child[0] != '\0';
    if (strncmp(child, dir, dl) != 0) return 0;
    return child[dl] == '/';
}

static int dir_empty(const char *p) {
    for (int i = 0; i < MAX_ENT; i++) {
        if (!g_ent[i].in_use) continue;
        if (strcmp(g_ent[i].path, p) == 0) continue;
        if (is_child_of(g_ent[i].path, p)) return 0;
    }
    return 1;
}

static void drop_subtree(const char *p) {
    for (int i = 0; i < MAX_ENT; i++) {
        if (!g_ent[i].in_use || g_ent[i].is_root) continue;
        if (strcmp(g_ent[i].path, p) != 0 && !is_child_of(g_ent[i].path, p))
            continue;
        free(g_ent[i].data);
        g_ent[i].data = NULL;
        g_ent[i].in_use = 0;
    }
}

static void random_name(char *out, size_t cap) {
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyz";
    size_t n = 3 + rnd(5);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) out[i] = alpha[rnd(sizeof alpha - 1)];
    out[n] = '\0';
}

static void grave_add(const char *name) {
    if (g_ngrave < GRAVE) snprintf(g_grave[g_ngrave++], 64, "%s", name);
    else { memmove(g_grave[0], g_grave[1], 63); snprintf(g_grave[GRAVE-1], 64, "%s", name); }
}

static int fresh_name(const char *dir, char *out, size_t cap) {
    char p[MPATH];
    for (int t = 0; t < 40; t++) {
        random_name(out, cap);
        if (join_path(p, sizeof p, dir, out) != 0) return -1;
        if (!find_path(p)) return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* errno discipline                                                    */
/* ------------------------------------------------------------------ */

/* Every errno a mutating op may legitimately produce on an rw mount. EROFS is
 * deliberately absent: an `-o rw` mount that answers EROFS took the read-only
 * path, which is the exact regression this lane exists to catch. */
static int sanctioned(int e) {
    switch (e) {
    case EEXIST: case ENOENT: case ENOTEMPTY: case EINVAL: case ENOSPC:
    case EISDIR: case ENOTDIR: case EPERM: case EOPNOTSUPP: case EACCES:
    case EXDEV:  case ELOOP:  case ENAMETOOLONG: case EBADF: case ENOMEM:
    case EINTR:  case ENOSYS: case EFAULT:  case ERANGE:
        return 1;
    case EIO:
        /* only on a deliberately damaged volume, where "I met a structure I
         * refuse to write through" is the designed answer */
        return g_allow_eio;
    default:
        return 0;
    }
}

static unsigned long c_eno[64];

/* What does a refusal IMPLY about the entry?
 *
 *   ENOENT     -- it is gone (a refusal to remove something already absent).
 *   ENOTEMPTY  -- it is still there: that is what "not empty" means, and it
 *                 is the only thing that makes rmdir's refusal an assertion
 *                 rather than an assumption.
 *   EEXIST     -- the conflicting name is still there.
 *   anything else -- UNKNOWN, and the probe adopts whatever the mount shows
 *                 rather than guessing. Guessing was the bug: the first
 *                 version asserted "the entry survived" after every refusal,
 *                 and reported "model dir vanished" for an rmdir that had been
 *                 refused with ENOENT -- i.e. for the entry correctly being
 *                 absent. */
typedef enum { RF_ABSENT, RF_PRESENT, RF_UNKNOWN } refusal_t;

static refusal_t refusal_implies(int e) {
    switch (e) {
    case ENOENT:    return RF_ABSENT;
    case ENOTEMPTY: case EEXIST: case EACCES: case EPERM: return RF_PRESENT;
    default:        return RF_UNKNOWN;
    }
}

static void note_errno(const char *what, int e) {
    if (e >= 0 && e < 64) c_eno[e]++;
    if (sanctioned(e)) return;
    char m[512];
    /* the errno VALUE matters: it is the only thing that distinguishes a
     * designed refusal from a designed-but-unreachable one, and the path
     * matters for reproducing it */
    snprintf(m, sizeof m, "%.120s -> %d (%s)", what, e, strerror(e));
    fail("unsanctioned errno", m);
}

/* ------------------------------------------------------------------ */
/* read-back: the differential assertion                               */
/* ------------------------------------------------------------------ */

static int reload(struct ent *e);

static void expect_file(const struct ent *e) {
    char full[4096];
    struct stat st;
    struct ent tmp;
    path_of(full, sizeof full, e->dir, e->name);
    if (stat(full, &st) != 0) {
        fail("model file vanished", full);
        return;
    }
    if ((size_t)st.st_size != e->len) {
        char m[160];
        snprintf(m, sizeof m, "%.90s: model %zu, mount %lld", full, e->len,
                 (long long)st.st_size);
        fail("size mismatch", m);
        return;
    }
    if (e->len == 0) return;
    /* Compare against a re-read, NOT a fixed stack buffer. The first version
     * read into MAX_DATA+16 bytes and compared against the model's length --
     * which "worked" until an append grew a file past that, and then reported
     * "read 2016 of 2297" against a file that was perfectly correct. */
    tmp = *e;
    tmp.data = NULL;
    tmp.len = 0;
    if (reload(&tmp) != 0) { fail("reopen failed", full); return; }
    if (memcmp(tmp.data, e->data, e->len) != 0)
        fail("content mismatch", full);
    free(tmp.data);
}

static void expect_gone(const char *dir, const char *name) {
    char full[4096];
    struct stat st;
    path_of(full, sizeof full, dir, name);
    if (stat(full, &st) == 0) fail("entry survived its removal", full);
    else if (errno != ENOENT) note_errno("stat after removal", errno);
}

/* Re-read an entry's bytes from the mount into the model. The model is always
 * derived from what the mount actually holds, never from what we asked for --
 * otherwise a disagreement cannot be attributed. The assertion is then made
 * separately, against the region we intended to change. */
static int reload(struct ent *e) {
    char full[4096];
    struct stat st;
    int fd;
    unsigned char *buf;
    path_of(full, sizeof full, e->dir, e->name);
    if (stat(full, &st) != 0) return -1;
    size_t n = (size_t)st.st_size;
    buf = malloc(n + 1);
    if (!buf) return -1;
    if (n) {
        size_t got = 0;
        fd = open(full, O_RDONLY);
        if (fd < 0) { free(buf); return -1; }
        /* read() may return SHORT -- it is not a promise to fill the buffer.
         * The first version did one read and compared against the expected
         * length, which reported "short read-back" for a perfectly correct
         * file. */
        while (got < n) {
            ssize_t r = read(fd, buf + got, n - got);
            if (r < 0) { close(fd); free(buf); return -1; }
            if (r == 0) break;
            got += (size_t)r;
        }
        close(fd);
        if (got != n) { free(buf); return -1; }
    }
    free(e->data);
    e->data = buf;
    e->len = n;
    return 0;
}

/* The bytes the mount holds in [off, off+len) must be exactly `want`. Called
 * only when the syscall reported SUCCESS, so this is the assertion that a
 * successful write really landed. */
static void region_is(const struct ent *e, const unsigned char *want,
                      size_t len, off_t off, const char *what) {
    char m[200];
    if (off < 0 || (size_t)off + len > e->len) {
        snprintf(m, sizeof m, "%s: asked for [%lld,%lld) but the file is %zu",
                 what, (long long)off, (long long)off + (long long)len, e->len);
        fail("write landed short", m);
        return;
    }
    if (memcmp(e->data + off, want, len) != 0)
        fail("write landed wrong bytes", what);
}

static void expect_dir(const struct ent *e) {
    char full[4096];
    struct stat st;
    path_of(full, sizeof full, e->dir, e->name);
    if (stat(full, &st) != 0) fail("model dir vanished", full);
    else if (!S_ISDIR(st.st_mode)) fail("model dir is not a directory", full);
}

/* ------------------------------------------------------------------ */
/* operations                                                          */
/* ------------------------------------------------------------------ */

static void do_create(void) {
    char dir[MPATH] = "", name[64], rel[MFULL];
    char full[4096];
    struct ent *d;
    size_t len = rnd(3) == 0 ? rnd(MAX_DATA) : rnd(400);
    unsigned char *buf;
    struct ent *e;
    int fd;
    c_tried[OP_CREATE]++;
    /* Reserve the model slot BEFORE creating anything. The first version
     * created the file first and returned early when the model was full --
     * leaving a real, untracked file in the volume while the model believed
     * the directory was empty. The next rmdir of that directory was then
     * refused by the mount, which the errno discipline reported as an
     * unexplained EIO until the sequence was traced back here. */
    e = alloc_ent();
    if (!e) return;
    if (rnd(3) == 0) {
        d = random_dir();
        if (d && may_nest(d->path) && setstr(dir, sizeof dir, d->path) != 0)
            d = NULL;
    }
    if (g_ngrave > 0 && rnd(4) == 0) {
        /* reuse a name that was just deleted: the hazard lives here */
        snprintf(name, sizeof name, "%s", g_grave[rnd((uint32_t)g_ngrave)]);
    } else if (fresh_name(dir, name, sizeof name) != 0) {
        return;
    }
    if (join_path(rel, sizeof rel, dir, name) != 0) return;
    path_of(full, sizeof full, dir, name);
    buf = malloc(len ? len : 1);
    for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)rnd(256);
    c_ops++;
    if (g_trace) fprintf(stderr, "TRACE iter %u create %s\n", g_iter, full);
    fd = open(full, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        note_errno("create", errno);
        c_refused++;
        free(buf);
        return;                       /* the slot stays unused */
    }
    /* write in pieces, so the shell's write path runs more than once per file */
    size_t off = 0;
    int wrote_all = 1;
    while (off < len) {
        size_t chunk = 1 + rnd(900);
        if (chunk > len - off) chunk = len - off;
        if (pwrite(fd, buf + off, chunk, (off_t)off) != (ssize_t)chunk) {
            note_errno("create write", errno);
            wrote_all = 0;
            break;
        }
        off += chunk;
    }
    close(fd);
    /* Whatever the mount reports is the truth: read it back and adopt THAT. */
    e->in_use = 1;
    e->is_dir = 0;
    e->data = NULL;
    e->len = 0;
    if (setstr(e->path, sizeof e->path, rel) != 0 ||
        setstr(e->name, sizeof e->name, name) != 0 ||
        setstr(e->dir, sizeof e->dir, dir) != 0 ||
        reload(e) != 0) {
        fail("created file not readable", full);
        free(e->data); e->data = NULL; e->in_use = 0; free(buf);
        return;
    }
    if (wrote_all) region_is(e, buf, len, 0, full);
    free(buf);
    c_accepted++;
    c_ok[OP_CREATE]++;
}

static void do_write(void) {
    struct ent *e = random_file();
    char full[4096];
    size_t len;
    unsigned char *buf;
    off_t off;
    int fd;
    c_tried[OP_WRITE]++;
    if (!e) return;
    path_of(full, sizeof full, e->dir, e->name);
    len = 1 + rnd(900);
    buf = malloc(len);
    for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)rnd(256);
    switch (rnd(4)) {
    case 0: off = 0; break;
    case 1: off = (off_t)e->len; break;                    /* at EOF */
    case 2: off = (off_t)e->len + (off_t)rnd(2048); break; /* past EOF: hole */
    default: off = e->len ? (off_t)rnd((uint32_t)e->len) : 0;
    }
    c_ops++;
    if (g_trace) fprintf(stderr, "TRACE iter %u pwrite %s off %lld len %zu\n",
                         g_iter, full, (long long)off, len);
    fd = open(full, O_RDWR);
    if (fd < 0) { note_errno("open for write", errno); c_refused++; free(buf); return; }
    int wrote = (pwrite(fd, buf, len, off) == (ssize_t)len);
    if (!wrote) note_errno("pwrite", errno);
    close(fd);
    /* The model follows OBSERVATION, not intent: a partial write must not be
     * recorded as a whole one, which is what made the first version of this
     * probe report a "content mismatch" that was really its own bookkeeping. */
    if (reload(e) != 0) {
        char m[400];
        struct stat st2;
        snprintf(m, sizeof m, "%.90s: errno=%d(%s) size=%lld",
                 full, errno, strerror(errno),
                 stat(full, &st2) == 0 ? (long long)st2.st_size : -1LL);
        fail("file unreadable after pwrite", m);
        free(buf); return;
    }
    if (wrote) region_is(e, buf, len, off, full);
    free(buf);
    c_accepted++;
    c_ok[OP_WRITE]++;
}

static void do_append(void) {
    struct ent *e = random_file();
    char full[4096];
    size_t len = 1 + rnd(300);
    unsigned char *buf;
    int fd;
    c_tried[OP_APPEND]++;
    if (!e) return;
    path_of(full, sizeof full, e->dir, e->name);
    buf = malloc(len);
    for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)rnd(256);
    c_ops++;
    /* O_APPEND must be the filesystem's job: with writeback caching off the
     * kernel does not apply it, so a shell that ignored the flag would
     * overwrite at the offset the kernel passed (always 0) and the size would
     * not grow. */
    fd = open(full, O_WRONLY | O_APPEND);
    if (fd < 0) { note_errno("open for append", errno); c_refused++; free(buf); return; }
    if (g_trace) fprintf(stderr, "TRACE iter %u append %s (model len %zu)\n",
                         g_iter, full, e->len);
    off_t at = (off_t)e->len;          /* where an append MUST land */
    int wrote = (write(fd, buf, len) == (ssize_t)len);
    if (!wrote) note_errno("append write", errno);
    close(fd);
    if (reload(e) != 0) { fail("file unreadable after append", full); free(buf); return; }
    if (wrote) region_is(e, buf, len, at, full);
    free(buf);
    c_accepted++;
    c_ok[OP_APPEND]++;
}

static void do_trunc(void) {
    struct ent *e = random_file();
    char full[4096];
    off_t want;
    int fd;
    c_tried[OP_TRUNC]++;
    if (!e) return;
    path_of(full, sizeof full, e->dir, e->name);
    want = (off_t)rnd(3) == 0 ? (off_t)rnd(MAX_DATA) : (off_t)rnd(300);
    c_ops++;
    fd = open(full, O_WRONLY);
    if (fd < 0) { note_errno("open for truncate", errno); c_refused++; return; }
    int ok = (ftruncate(fd, want) == 0);
    if (!ok) note_errno("ftruncate", errno);
    close(fd);
    if (reload(e) != 0) { fail("file unreadable after truncate", full); return; }
    if (ok && (off_t)e->len != want) {
        char m[160];
        snprintf(m, sizeof m, "asked for %lld, mount says %zu",
                 (long long)want, e->len);
        fail("truncate size wrong", m);
    }
    c_accepted++;
    c_ok[OP_TRUNC]++;
}

static void do_rename(void) {
    struct ent *e = random_file();
    char from[4096], to[4096], name[64], rel[MFULL], dir[MPATH];
    c_tried[OP_RENAME]++;
    if (!e) return;
    if (rnd(3) == 0) {
        struct ent *d = random_dir();
        if (!(d && may_nest(d->path) &&
              setstr(dir, sizeof dir, d->path) == 0) &&
            setstr(dir, sizeof dir, e->dir) != 0)
            return;
    } else {
        snprintf(dir, sizeof dir, "%s", e->dir);
    }
    if (fresh_name(dir, name, sizeof name) != 0) return;
    if (join_path(rel, sizeof rel, dir, name) != 0) return;
    path_of(from, sizeof from, e->dir, e->name);
    path_of(to, sizeof to, dir, name);
    c_ops++;
    /* rename(a, a) is a POSIX no-op SUCCESS; the kernel does not shortcut it,
     * so this is a real exercise of the shell's own same-name handling. */
    if (g_trace) fprintf(stderr, "TRACE iter %u rename %s -> %s\n", g_iter, from, to);
    if (strcmp(from, to) != 0 && rename(from, to) != 0) {
        int re = errno;
        note_errno("rename", re);
        c_refused++;
        if (refusal_implies(re) != RF_ABSENT) expect_file(e);
        return;
    }
    expect_gone(e->dir, e->name);
    if (setstr(e->dir, sizeof e->dir, dir) != 0 ||
        setstr(e->name, sizeof e->name, name) != 0 ||
        setstr(e->path, sizeof e->path, rel) != 0)
        return;
    expect_file(e);
    c_accepted++;
    c_ok[OP_RENAME]++;
}

static void do_unlink(void) {
    struct ent *e = random_file();
    char full[4096];
    c_tried[OP_UNLINK]++;
    if (!e) return;
    path_of(full, sizeof full, e->dir, e->name);
    c_ops++;
    if (g_trace) fprintf(stderr, "TRACE iter %u unlink %s\n", g_iter, full);
    if (unlink(full) != 0) {
        int re = errno;
        note_errno("unlink", re);
        c_refused++;
        if (refusal_implies(re) == RF_PRESENT) expect_file(e);
        else {
            char probe[4096];
            struct stat st;
            if (stat(full, &st) != 0) {
                free(e->data); e->data = NULL; e->in_use = 0;
            }
            (void)probe;
        }
        return;
    }
    expect_gone(e->dir, e->name);
    if (e->dir[0] == '\0') grave_add(e->name);
    free(e->data);
    e->data = NULL;
    e->in_use = 0;
    c_accepted++;
    c_ok[OP_UNLINK]++;
}

static void do_mkdir(void) {
    char dir[MPATH] = "", name[64], rel[MFULL], full[4096];
    struct ent *d, *e;
    c_tried[OP_MKDIR]++;
    e = alloc_ent();
    if (!e) return;                   /* before mkdir, for the same reason */
    if (rnd(3) == 0) {
        d = random_dir();
        if (d && may_nest(d->path) && setstr(dir, sizeof dir, d->path) != 0)
            d = NULL;
    }
    if (fresh_name(dir, name, sizeof name) != 0) return;
    if (join_path(rel, sizeof rel, dir, name) != 0) return;
    path_of(full, sizeof full, dir, name);
    c_ops++;
    if (g_trace) fprintf(stderr, "TRACE iter %u mkdir %s\n", g_iter, full);
    if (mkdir(full, 0755) != 0) {
        note_errno("mkdir", errno);
        c_refused++;
        return;
    }
    {
        e->in_use = 1;
        e->is_dir = 1;
        if (setstr(e->path, sizeof e->path, rel) == 0 &&
            setstr(e->name, sizeof e->name, name) == 0 &&
            setstr(e->dir, sizeof e->dir, dir) == 0)
            expect_dir(e);
    }
    c_accepted++;
    c_ok[OP_MKDIR]++;
}

static void do_rmdir(void) {
    unsigned dirs = 0, pick;
    struct ent *e = NULL;
    char full[4096];
    /* The scratch root is a tracked directory so its children nest correctly,
     * but it must never be a REMOVAL candidate: while it is still empty it
     * satisfies dir_empty, and rmdir of it succeeds -- deleting the probe's
     * own working directory. Every later operation then failed with ENOENT,
     * which is exactly what two seeds showed. */
    for (int i = 0; i < MAX_ENT; i++)
        if (g_ent[i].in_use && g_ent[i].is_dir && !g_ent[i].is_root &&
            dir_empty(g_ent[i].path))
            dirs++;
    c_tried[OP_RMDIR]++;
    if (!dirs) return;
    pick = rnd(dirs);
    for (int i = 0; i < MAX_ENT; i++) {
        if (!g_ent[i].in_use || !g_ent[i].is_dir || g_ent[i].is_root) continue;
        if (!dir_empty(g_ent[i].path)) continue;
        if (pick-- == 0) { e = &g_ent[i]; break; }
    }
    if (!e) return;
    path_of(full, sizeof full, e->dir, e->name);
    c_ops++;
    if (g_trace) fprintf(stderr, "TRACE iter %u rmdir %s\n", g_iter, full);
    if (rmdir(full) != 0) {
        int re = errno;
        note_errno(full, re);
        c_refused++;
        if (refusal_implies(re) == RF_PRESENT) {
            expect_dir(e);
        } else {
            /* adopt reality: a refused removal may still have unlinked the
             * entry (the shell unlinks BEFORE freeing blocks, so a later I/O
             * failure leaks storage rather than half-linking -- on a damaged
             * volume "refused" and "already gone" can overlap) */
            char probe[4096];
            struct stat st;
            if (stat(full, &st) != 0) drop_subtree(e->path);
            else expect_dir(e);
            (void)probe;
        }
        return;
    }
    expect_gone(e->dir, e->name);
    drop_subtree(e->path);
    c_accepted++;
    c_ok[OP_RMDIR]++;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mount_write_probe <mountpoint> <iterations> "
                        "[seed]\n");
        return 2;
    }
    unsigned iters = (unsigned)strtoul(argv[2], NULL, 10);
    g_trace = getenv("WPROBE_TRACE") != NULL;
    if (argc > 4 && strcmp(argv[4], "--lenient") == 0) g_allow_eio = 1;
    if (argc > 3) g_seed = (unsigned)strtoul(argv[3], NULL, 10);
    /* the scratch path is g_root + "/.wprobe": size the buffer for that, and
     * refuse a root too long to append rather than truncating it into a
     * different directory */
    if (strlen(argv[1]) + sizeof("/.wprobe") > sizeof g_scratch) {
        fprintf(stderr, "wprobe: mountpoint path too long\n");
        return 2;
    }
    snprintf(g_root, sizeof g_root, "%s", argv[1]);
    snprintf(g_scratch, sizeof g_scratch, "%s/.wprobe", g_root);

    {
        int mrc = mkdir(g_scratch, 0755);
        if (g_trace)
            fprintf(stderr, "TRACE mkdir %s -> %d (%s)\n", g_scratch, mrc,
                    mrc ? strerror(errno) : "ok");
        if (mrc != 0 && errno != EEXIST) {
            /* Not a failure: the mount is deliberately read-only-hostile or
             * the volume refuses writes (a bitmap flagged invalid, a page
             * count that cannot be trusted -- both refusals the write path
             * makes on purpose). Say so and let the gate skip the image. */
            printf("wprobe: mount is not writable (%s); skipped\n",
                   strerror(errno));
            return 3;
        }
    }
    /* `.wprobe` itself becomes a tracked directory (relative path ""), so
     * its children nest correctly. is_root marks it as never removable: while
     * it is still empty it satisfies dir_empty, and do_rmdir would otherwise
     * delete the probe's own working directory -- after which every operation
     * fails with ENOENT and the run reports 161 refusals and no coverage. */
    {
        struct ent *e = alloc_ent();
        if (e) {
            e->in_use = 1;
            e->is_dir = 1;
            e->is_root = 1;
            e->data = NULL;
            e->len = 0;
            e->path[0] = '\0';
            e->dir[0] = '\0';
            e->name[0] = '\0';
        }
    }

    for (unsigned it = 0; it < iters; it++) {
        unsigned pick = rnd(100);
        g_iter = it;
        g_op = pick < 26 ? "create" : pick < 44 ? "write" : pick < 54 ? "append"
             : pick < 62 ? "trunc" : pick < 72 ? "rename" : pick < 82 ? "unlink"
             : pick < 92 ? "mkdir" : "rmdir";
        if (pick < 26)      do_create();
        else if (pick < 44) do_write();
        else if (pick < 54) do_append();
        else if (pick < 62) do_trunc();
        else if (pick < 72) do_rename();
        else if (pick < 82) do_unlink();
        else if (pick < 92) do_mkdir();
        else                do_rmdir();
        if (failures) break;
    }

    printf("wprobe: ops=%lu accepted=%lu refused=%lu\n",
           c_ops, c_accepted, c_refused);
    {
        static const int interesting[] = { ENOSPC, EEXIST, ENOENT, ENOTEMPTY,
                                           EINVAL, EACCES, EPERM, EOPNOTSUPP,
                                           EISDIR, ENOTDIR, EXDEV, ELOOP,
                                           EIO };
        for (size_t i = 0; i < sizeof interesting / sizeof *interesting; i++)
            if (c_eno[interesting[i]])
                printf("wprobe: refusals by errno %-12s %lu\n",
                       strerror(interesting[i]), c_eno[interesting[i]]);
    }
    for (int i = 0; i < OP_COUNT; i++)
        printf("wprobe: %-8s tried=%-5lu accepted=%-5lu\n",
               op_name[i], c_tried[i], c_ok[i]);
    for (int i = 0; i < MAX_ENT; i++)
        if (g_ent[i].in_use) free(g_ent[i].data);
    return failures ? 1 : 0;
}