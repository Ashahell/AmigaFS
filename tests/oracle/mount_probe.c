/* mount_probe -- drive a LIVE amigafs mount and report what it reached.
 *
 * The library fuzzer exercises the parser with hostile images, but the FUSE
 * shell's own arithmetic -- inode encoding, readdir list construction, the
 * synthetic `.amigafs` bitmap sizing, the reply paths -- is only reachable
 * through a real mount, because `fuse_req_t` is opaque and libfuse's
 * custom-I/O path is compiled out of distro builds. So the coverage has to
 * come from actually mounting.
 *
 * This walks a mountpoint the way a user would, and COUNTS what it touched.
 * The counts are the point: a fuzz run that reports "0 failures" while
 * mounting nothing has tested nothing, and only a count can tell the
 * difference. It prints:
 *
 *     probe: dirs=N entries=N files=N symlinks=N xattrs=N bytes=N
 *
 * and, on a non-zero exit, a line naming what went wrong.
 *
 * It also issues the requests a well-behaved kernel would NOT: a readdir
 * resumed at a hostile cookie (lseek to a huge offset, then getdents64), which
 * is the one kernel-supplied scalar that reaches the shell unfiltered.
 *
 *   usage: mount_probe <mountpoint> [max_depth]
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

static unsigned long c_dirs, c_entries, c_files, c_symlinks, c_xattrs, c_bytes;
static unsigned long c_dirhard;   /* C12 dir hard links: refused by design */
static unsigned long c_unreachable; /* name no longer hashes to its own bucket */
static int failures;

/* Resume a directory listing at an arbitrary cookie. The kernel normally only
 * ever hands back a cookie this filesystem issued; a hostile or buggy caller
 * can hand back anything, and readdir must not index off the end of anything
 * because of it. */
static void hostile_offset(const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    int fd = dirfd(d);
    static const off_t offs[] = {
        (off_t)-1, 0, 1, 2, 3, 4, 1L << 30, (off_t)1 << 62, (off_t)-2,
    };
    for (size_t i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        char buf[8192];
        if (lseek(fd, offs[i], SEEK_SET) == (off_t)-1 && errno != 0) continue;
        memset(buf, 0, sizeof buf);
        long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
        if (n < 0 && errno != EINVAL) {
            fprintf(stderr, "probe: getdents64 at offset %lld failed: %s\n",
                    (long long)offs[i], strerror(errno));
            failures++;
        }
    }
    closedir(d);
}

static void walk(const char *path, int depth, int maxdepth);

static void visit(const char *path, int depth, int maxdepth) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        /* ENOTDIR is the DESIGNED answer for a hard-linked directory (C12): a
         * dir hard link is a cycle the kernel dentry cache cannot represent,
         * so the mount refuses to traverse it. It is counted, not failed --
         * but ONLY that one errno is expected. An entry that fails for any
         * other reason is a real defect, and lumping them together would let a
         * genuine failure hide behind a designed refusal. */
        if (errno == ENOTDIR) { c_dirhard++; return; }
        if (errno == ENOENT) {
            /* A name whose bytes no longer hash to its own bucket is
             * unreachable BY THE FORMAT, not by the mount. readdir walks EVERY
             * bucket so it still shows such a name; lookup computes hash(name)
             * and reads that bucket, where the entry is not filed. Measured:
             * 'multidef' -> bucket 46, one byte mutated to 0xFE -> bucket 16,
             * and the entry stays at 46. ADFlib uses the same hash and the same
             * bucket lookup, so it answers identically.
             *
             * Counted separately and REPORTED, never failed. Whitelisting it
             * here without a cross-check would be catastrophic masking: a mount
             * whose lookup was broken entirely would ENOENT on every name and
             * this probe would report zero failures. So mount_fuzz.sh asks the
             * library -- the format's own authority -- about each one, and
             * fails the gate if the library CAN find what the mount could not.
             */
            c_unreachable++;
            fprintf(stderr, "probe: unreachable %s\n", path);
            return;
        }
        fprintf(stderr, "probe: lstat %s failed: %s\n", path, strerror(errno));
        failures++;
        return;
    }
    c_entries++;
    if (S_ISDIR(st.st_mode)) { walk(path, depth + 1, maxdepth); return; }
    if (S_ISLNK(st.st_mode)) {
        char tgt[4096];
        ssize_t n = readlink(path, tgt, sizeof tgt - 1);
        if (n < 0) {
            fprintf(stderr, "probe: readlink %s failed: %s\n", path,
                    strerror(errno));
            failures++;
        } else {
            c_symlinks++;
        }
        return;
    }
    if (!S_ISREG(st.st_mode)) return;

    /* xattrs: list then get, and an attribute nobody provides */
    char list[4096];
    ssize_t ln = llistxattr(path, list, sizeof list - 1);
    if (ln > 0) {
        list[ln] = '\0';
        char *p = list;
        while (*p) {
            char val[1024];
            ssize_t vn = lgetxattr(path, p, val, sizeof val - 1);
            if (vn < 0) {
                fprintf(stderr, "probe: lgetxattr %s [%s] failed: %s\n",
                        path, p, strerror(errno));
                failures++;
            } else {
                c_xattrs++;
            }
            p += strlen(p) + 1;
        }
    }
    /* an attribute this filesystem never provides must be refused, not
     * invented: size query with a NULL buffer is the cheapest form */
    (void)lgetxattr(path, "user.amiga.nonexistent", NULL, 0);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "probe: open %s failed: %s\n", path, strerror(errno));
        failures++;
        return;
    }
    unsigned char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) c_bytes += (unsigned long)n;
    if (n < 0) {
        fprintf(stderr, "probe: read %s failed: %s\n", path, strerror(errno));
        failures++;
    } else {
        c_files++;
    }
    close(fd);
}

static void walk(const char *path, int depth, int maxdepth) {
    if (depth > maxdepth) return;
    DIR *d = opendir(path);
    if (!d) return;
    c_dirs++;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char sub[4096];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        visit(sub, depth, maxdepth);
    }
    closedir(d);
    hostile_offset(path);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mount_probe <mountpoint> [maxdepth]\n");
        return 2;
    }
    int maxdepth = argc > 2 ? atoi(argv[2]) : 12;
    walk(argv[1], 0, maxdepth);
    printf("probe: dirs=%lu entries=%lu files=%lu symlinks=%lu xattrs=%lu "
           "bytes=%lu dirhard=%lu unreachable=%lu\n",
           c_dirs, c_entries, c_files, c_symlinks, c_xattrs, c_bytes, c_dirhard,
           c_unreachable);
    return failures ? 1 : 0;
}
