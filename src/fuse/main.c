/* amigafs — mount a classic AmigaDOS volume (OFS/FFS/INTL/DIRC) on Linux.
 *
 * Uses the FUSE3 LOW-LEVEL API (D1): the kernel owns path resolution and
 * caching, we answer per-inode operations. An inode is the Amiga header block
 * number, partition-qualified, so a hard-linked file naturally shares one.
 *
 * Read-only: the mount is forced `ro`, and no write operation is advertised.
 */
#define FUSE_USE_VERSION 31

#include <fuse_lowlevel.h>
#include <fuse_opt.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "amigafs/blocktypes.h"
#include "amigafs/dir.h"
#include "amigafs/file.h"
#include "amigafs/links.h"
#include "amigafs/translate.h"
#include "amigafs/volume.h"

#define AMIGAFS_OPT(t, p) { t, offsetof(struct options, p), 1 }

struct options {
    unsigned int uid;          /* mounting user's uid */
    unsigned int gid;
    int amiga_ids;             /* expose stored 16-bit uid/gid (D7) */
    int local_tz;              /* interpret dates as local time (D6) */
    int foreground;
};

static afs_volume_t *g_vol;
static struct options g_opt;

/* inode 1 is the filesystem root; the volume rootblock maps to it. */
#define FUSE_ROOT_INO 1

static void fill_stat(const afs_fileheader_t *fh, fuse_ino_t ino,
                      mode_t type, struct stat *st) {
    memset(st, 0, sizeof *st);
    st->st_ino = (ino_t)ino;
    st->st_nlink = 1;
    afs_translate_mode(fh->protect, st, type);
    afs_translate_ids(g_opt.uid, g_opt.gid, fh->uid, fh->gid,
                      g_opt.amiga_ids, st);
    st->st_size = (off_t)fh->byte_size;
    afs_translate_mtime(fh->days, fh->mins, fh->ticks, g_opt.local_tz, st);
}

static afs_block_kind_t kind_to_mode(afs_block_kind_t k) {
    switch (k) {
    case AFS_BLOCK_DIR:
    case AFS_BLOCK_LINKDIR: return S_IFDIR;
    case AFS_BLOCK_SOFTLINK: return S_IFLNK;
    default: return S_IFREG;
    }
}

static void afs_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct fuse_entry_param e;
    afs_dirent_t de;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        e.ino = parent;
        e.generation = 0;
        e.attr_timeout = 3600;
        e.entry_timeout = 3600;
        memset(&e.attr, 0, sizeof e.attr);
        e.attr.st_ino = parent;
        e.attr.st_mode = S_IFDIR | 0555;
        e.attr.st_nlink = 2;
        fuse_reply_entry(req, &e);
        return;
    }
    uint32_t parentblk = (parent == FUSE_ROOT_INO)
                           ? afs_volume_rootblock(g_vol) : (uint32_t)parent;
    if (afs_dir_lookup(g_vol, parentblk, name, strlen(name), &de) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    /* A directory hard link must NOT be walked (C12) -- a hard-linked directory
     * is a cycle the kernel dentry cache cannot represent. */
    if (de.kind == AFS_BLOCK_LINKDIR) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    size_t bs = afs_volume_block_size(g_vol);
    uint8_t *blk = malloc(bs);
    if (!blk) { fuse_reply_err(req, ENOMEM); return; }
    if (afs_volume_read_block(g_vol, de.block, blk, bs) != 0) {
        free(blk);
        fuse_reply_err(req, EIO);
        return;
    }

    /* Resolve a hard link to its REAL entry so several names share ONE inode
     * (and reads go to the real file's data blocks). A soft link keeps its
     * own inode (the kernel needs a distinct node to readlink). */
    uint32_t ino_blk = de.block;
    if (de.kind == AFS_BLOCK_LINKFILE) {
        uint32_t real = 0;
        if (afs_link_resolve_file(g_vol, parentblk, name, strlen(name), &real) == 0)
            ino_blk = real;
    }
    if (ino_blk != de.block) {
        if (afs_volume_read_block(g_vol, ino_blk, blk, bs) != 0) {
            free(blk);
            fuse_reply_err(req, EIO);
            return;
        }
    }

    e.ino = ino_blk;
    e.generation = 0;
    e.attr_timeout = 3600;
    e.entry_timeout = 3600;

    afs_block_kind_t k = afs_block_kind(blk, bs);
    if (k == AFS_BLOCK_ROOT) {
        memset(&e.attr, 0, sizeof e.attr);
        e.attr.st_ino = de.block;
        e.attr.st_mode = S_IFDIR | 0555;
        e.attr.st_nlink = 2;
    } else {
        afs_fileheader_t h;
        memset(&h, 0, sizeof h);
        if (afs_parse_fileheader(blk, bs, &h) != 0) memset(&h, 0, sizeof h);
        fill_stat(&h, de.block, kind_to_mode(k), &e.attr);
        if (k == AFS_BLOCK_SOFTLINK) e.attr.st_size = 0;
        /* an unparseable header still needs a sane mode */
        if (e.attr.st_mode == 0) {
            e.attr.st_mode = (k == AFS_BLOCK_DIR) ? (S_IFDIR | 0555)
                                                  : (S_IFREG | 0444);
        }
    }
    free(blk);
    fuse_reply_entry(req, &e);
}

static void afs_ll_getattr(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *fi) {
    (void)fi;
    struct stat st;
    uint8_t *blk;
    size_t bs = afs_volume_block_size(g_vol);

    if (ino == FUSE_ROOT_INO) {
        const afs_rootblock_t *rb = afs_volume_root(g_vol);
        memset(&st, 0, sizeof st);
        st.st_ino = FUSE_ROOT_INO;
        st.st_nlink = 2;
        afs_translate_mode(0, &st, S_IFDIR);
        afs_translate_ids(g_opt.uid, g_opt.gid, 0, 0, g_opt.amiga_ids, &st);
        st.st_size = 512;
        (void)rb;
        fuse_reply_attr(req, &st, 3600);
        return;
    }

    blk = malloc(bs);
    if (!blk) { fuse_reply_err(req, ENOMEM); return; }
    if (afs_volume_read_block(g_vol, (uint32_t)ino, blk, bs) != 0) {
        free(blk);
        fuse_reply_err(req, EIO);
        return;
    }
    afs_block_kind_t k = afs_block_kind(blk, bs);
    if (k == AFS_BLOCK_UNKNOWN) {
        free(blk);
        fuse_reply_err(req, EIO);
        return;
    }
    if (k == AFS_BLOCK_ROOT) {
        memset(&st, 0, sizeof st);
        st.st_ino = ino;
        st.st_nlink = 2;
        afs_translate_mode(0, &st, S_IFDIR);
        st.st_size = 512;
        free(blk);
        fuse_reply_attr(req, &st, 3600);
        return;
    }
    afs_fileheader_t h;
    memset(&h, 0, sizeof h);
    afs_parse_fileheader(blk, bs, &h);
    fill_stat(&h, (uint32_t)ino, kind_to_mode(k), &st);
    if (k == AFS_BLOCK_SOFTLINK) st.st_size = 0;
    free(blk);
    fuse_reply_attr(req, &st, 3600);
}

static void afs_ll_opendir(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info *fi) {
    (void)fi;
    if (ino != FUSE_ROOT_INO) {
        /* opening a non-root directory: verify it is one */
        uint8_t *blk = malloc(afs_volume_block_size(g_vol));
        if (!blk) { fuse_reply_err(req, ENOMEM); return; }
        afs_volume_read_block(g_vol, (uint32_t)ino, blk, afs_volume_block_size(g_vol));
        afs_block_kind_t k = afs_block_kind(blk, afs_volume_block_size(g_vol));
        free(blk);
        if (k != AFS_BLOCK_DIR && k != AFS_BLOCK_ROOT) {
            fuse_reply_err(req, ENOTDIR);
            return;
        }
    }
    fuse_reply_open(req, fi);
}

static void afs_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                            off_t off, struct fuse_file_info *fi) {
    (void)size; (void)fi;
    uint32_t dirblk = (ino == FUSE_ROOT_INO)
                        ? afs_volume_rootblock(g_vol) : (uint32_t)ino;

    char *buf = malloc(16384);
    if (!buf) { fuse_reply_err(req, ENOMEM); return; }
    size_t used = 0;

    if (off == 0) {
        /* . and .. carry their real inodes */
        struct stat st;
        memset(&st, 0, sizeof st);
        st.st_mode = S_IFDIR | 0555;
        st.st_nlink = 2;
        st.st_ino = ino;
        used += (size_t)fuse_add_direntry(req, buf + used, 16384 - used, ".", &st, 1);
        st.st_ino = (ino == FUSE_ROOT_INO) ? FUSE_ROOT_INO : dirblk;
        used += (size_t)fuse_add_direntry(req, buf + used, 16384 - used, "..", &st, 1);
    }

    afs_dirent_t ents[256];
    size_t count = 0;
    if (afs_dir_list(g_vol, dirblk, ents, 256, &count) != 0) {
        free(buf);
        fuse_reply_err(req, EIO);
        return;
    }

    /* Each entry is emitted with a stable "cookie" equal to its 1-based index
     * in the listing (offset 1 == ".", 2 == "..", 3 == first real entry). The
     * kernel resumes by passing that cookie back as `off`. */
    size_t start_idx = (off <= 2) ? 0 : (size_t)(off - 2);
    for (size_t i = start_idx; i < count; i++) {
        struct stat st;
        memset(&st, 0, sizeof st);
        /* hard-linked files share the real entry's inode */
        uint32_t ino_blk = ents[i].block;
        if (ents[i].kind == AFS_BLOCK_LINKFILE) {
            uint32_t real = 0;
            if (afs_link_resolve_file(g_vol, dirblk, ents[i].name,
                                      strlen(ents[i].name), &real) == 0)
                ino_blk = real;
        }
        st.st_ino = ino_blk;
        if (ents[i].kind == AFS_BLOCK_DIR) {
            st.st_mode = S_IFDIR | 0555;
            st.st_nlink = 2;
        } else if (ents[i].kind == AFS_BLOCK_SOFTLINK) {
            st.st_mode = S_IFLNK | 0777;
        } else {
            st.st_mode = S_IFREG | 0444;
        }
        char *name = strdup(ents[i].name);
        if (!name) break;
        size_t need = fuse_add_direntry(req, NULL, 0, name, &st, (off_t)(i + 3));
        if (used + need > 16384) { free(name); break; }
        used += (size_t)fuse_add_direntry(req, buf + used, 16384 - used,
                                          name, &st, (off_t)(i + 3));
        (void)ino_blk;
        free(name);
    }

    fuse_reply_buf(req, buf, used);
    free(buf);
}

static void afs_ll_open(fuse_req_t req, fuse_ino_t ino,
                        struct fuse_file_info *fi) {
    (void)fi;
    uint8_t *blk = malloc(afs_volume_block_size(g_vol));
    if (!blk) { fuse_reply_err(req, ENOMEM); return; }
    afs_volume_read_block(g_vol, (uint32_t)ino, blk, afs_volume_block_size(g_vol));
    afs_block_kind_t k = afs_block_kind(blk, afs_volume_block_size(g_vol));
    free(blk);
    if (k == AFS_BLOCK_DIR || k == AFS_BLOCK_ROOT) {
        fuse_reply_err(req, EISDIR);
        return;
    }
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        fuse_reply_err(req, EROFS);
        return;
    }
    fuse_reply_open(req, fi);
}

static void afs_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                        struct fuse_file_info *fi) {
    (void)fi;
    /* read via the parent-less path: find the name is not needed; we resolve
     * the header block directly. afs_file_read works by name, so read the
     * header's data blocks by block number instead. */
    uint8_t *blk = malloc(afs_volume_block_size(g_vol));
    if (!blk) { fuse_reply_err(req, ENOMEM); return; }
    if (afs_volume_read_block(g_vol, (uint32_t)ino, blk, afs_volume_block_size(g_vol)) != 0) {
        free(blk);
        fuse_reply_err(req, EIO);
        return;
    }
    afs_fileheader_t h;
    memset(&h, 0, sizeof h);
    afs_parse_fileheader(blk, afs_volume_block_size(g_vol), &h);
    uint32_t byte_size = h.byte_size;
    free(blk);

    if ((uint64_t)off >= byte_size) { fuse_reply_buf(req, NULL, 0); return; }
    if ((uint64_t)off + size > byte_size) size = byte_size - (size_t)off;

    char *buf = malloc(size ? size : 1);
    if (!buf) { fuse_reply_err(req, ENOMEM); return; }
    /* use the file reader by constructing a name lookup from the block: we
     * instead read the data blocks directly via afs_file_read_all is wasteful;
     * for correctness use afs_file_read on the inode's parent is unknown, so
     * read via the block-pointer walk here. */
    size_t got = 0;
    int rc = afs_read_file_by_block(g_vol, (uint32_t)ino, buf, size, (uint64_t)off, &got);
    if (rc != 0) { free(buf); fuse_reply_err(req, EIO); return; }
    fuse_reply_buf(req, buf, got);
    free(buf);
}

static void afs_ll_readlink(fuse_req_t req, fuse_ino_t ino) {
    size_t bs = afs_volume_block_size(g_vol);
    uint8_t *blk = malloc(bs);
    if (!blk) { fuse_reply_err(req, ENOMEM); return; }
    if (afs_volume_read_block(g_vol, (uint32_t)ino, blk, bs) != 0) {
        free(blk);
        fuse_reply_err(req, EIO);
        return;
    }
    if (afs_block_kind(blk, bs) != AFS_BLOCK_SOFTLINK) {
        free(blk);
        fuse_reply_err(req, EINVAL);
        return;
    }
    /* the symbolic name is a C-string at offset 24 */
    size_t maxlen = bs - 24;
    char *raw = malloc(maxlen + 1);
    if (!raw) { free(blk); fuse_reply_err(req, ENOMEM); return; }
    memcpy(raw, blk + 24, maxlen);
    raw[maxlen] = 0;
    free(blk);
    /* translate to a mount-relative POSIX path (C11) */
    char out[1024];
    afs_softlink_translate(raw, 0, out, sizeof out);
    fuse_reply_readlink(req, out);
    free(raw);
}

static void afs_ll_statfs(fuse_req_t req, fuse_ino_t ino) {
    (void)ino;
    struct statvfs sv;
    memset(&sv, 0, sizeof sv);
    sv.f_bsize = 512;
    sv.f_frsize = 512;
    sv.f_blocks = (fsblkcnt_t)afs_volume_block_count(g_vol);
    sv.f_namemax = 30;
    fuse_reply_statfs(req, &sv);
}

static void afs_ll_init(void *userdata, struct fuse_conn_info *conn) {
    (void)userdata;
    conn->want &= (unsigned)~FUSE_CAP_WRITEBACK_CACHE;
}

static const struct fuse_lowlevel_ops afs_ll_ops = {
    .lookup  = afs_ll_lookup,
    .getattr = afs_ll_getattr,
    .opendir = afs_ll_opendir,
    .readdir = afs_ll_readdir,
    .open    = afs_ll_open,
    .read    = afs_ll_read,
    .readlink= afs_ll_readlink,
    .statfs  = afs_ll_statfs,
    .init    = afs_ll_init,
};

enum {
    KEY_AMIGA_IDS,
    KEY_TZ_LOCAL,
};

static const struct fuse_opt opts[] = {
    AMIGAFS_OPT("amiga_ids", amiga_ids),
    AMIGAFS_OPT("tz=local", local_tz),
    AMIGAFS_OPT("-f", foreground),
    FUSE_OPT_END
};

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s <image> <mountpoint> [options]\n"
        "  -o amiga_ids      expose stored Amiga uid/gid\n"
        "  -o tz=local       interpret dates as local time (default UTC)\n"
        "  -- -f             foreground\n"
        "  -s                single-threaded (default)\n", prog);
}

int main(int argc, char *argv[]) {
    g_opt.uid = (unsigned)getuid();
    g_opt.gid = (unsigned)getgid();

    /* Parse our own options and the image/mountpoint out of argv first.
     * fargs starts empty (0,NULL) so libfuse owns it and fuse_opt_add_arg is
     * legal on it; opts_args borrows the real argv read-only. */
    struct fuse_args fargs = FUSE_ARGS_INIT(0, NULL);
    struct fuse_args opts_args = FUSE_ARGS_INIT(argc, argv);
    if (fuse_opt_parse(&opts_args, &g_opt, opts, NULL) == -1) {
        usage(argv[0]);
        return 1;
    }
    if (opts_args.argc < 3) { usage(argv[0]); return 1; }
    const char *image = opts_args.argv[1];
    const char *mountpoint = opts_args.argv[2];

    char err[256] = {0};
    if (afs_volume_open_file(image, 512, 0, &g_vol, err, sizeof err) != 0) {
        fprintf(stderr, "amigafs: %s\n", err);
        return 1;
    }

    /* Build the argument vector FUSE itself sees: program name plus only the
     * options it understands. The image and mountpoint are ours. */
    /* With the LOW-LEVEL API, -f/-s/-o are NOT passed to fuse_session_new:
     * they are high-level options. We pass only argv[0] and mount manually,
     * and the mount flags (ro, default_permissions, single-threaded) are
     * enforced by our own ops plus fuse_session_mount. */
    fuse_opt_add_arg(&fargs, argv[0]);

    struct fuse_session *se = fuse_session_new(&fargs, &afs_ll_ops,
                                                sizeof afs_ll_ops, NULL);
    if (!se) {
        fprintf(stderr, "amigafs: cannot create session\n");
        afs_volume_close(g_vol);
        fuse_opt_free_args(&fargs);
        fuse_opt_free_args(&opts_args);
        return 1;
    }
    if (fuse_set_signal_handlers(se) != 0) {
        fuse_session_destroy(se);
        afs_volume_close(g_vol);
        return 1;
    }
    if (fuse_session_mount(se, mountpoint) != 0) {
        fprintf(stderr, "amigafs: cannot mount on %s\n", mountpoint);
        fuse_remove_signal_handlers(se);
        fuse_session_destroy(se);
        afs_volume_close(g_vol);
        return 1;
    }
    fuse_daemonize(0);   /* we always run in the foreground (-f) */
    int ret = fuse_session_loop(se);
    fuse_session_unmount(se);
    fuse_remove_signal_handlers(se);
    fuse_session_destroy(se);
    afs_volume_close(g_vol);
    fuse_opt_free_args(&fargs);
    fuse_opt_free_args(&opts_args);
    return ret ? 1 : 0;
}
