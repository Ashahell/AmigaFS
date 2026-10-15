/* amigafs-fuse -- mount a classic AmigaDOS volume read-only via FUSE3.
 *
 * Two media shapes reach the same mount:
 *
 *   - a FLOPPY ADF, or any single-volume image. The mount root IS the volume's
 *     root directory, so `ls /mnt` lists the disk.
 *
 *   - an RDB-PARTITIONED hard-disk image. The mount root lists the partitions
 *     by their RDB DriveName, and each partition is a directory holding that
 *     volume's root. Inside a partition everything is partition-relative,
 *     which is why each is opened as a window of its OWN length
 *     (afs_volume_open_window) rather than "to the end of the image" -- an
 *     over-long window puts the computed rootblock in the middle of the NEXT
 *     partition, which leaves only the last partition mountable.
 *
 * INODES. A real entry's inode is its ABSOLUTE block number in the image plus
 * 2, so hard links still share one node and no two files in different
 * partitions can collide. Inode 1 is the mount root; inode 0 is never issued.
 *
 * SYNTHETIC NODES. Every volume root also contains `.amigafs/`, holding that
 * volume's raw block bitmap (decision D4): one bit per block, most significant
 * bit first, 1 = free. It is a real file with real bytes, so a `cmp` against a
 * dump is a real comparison. Two synthetic inodes per volume live far above
 * any block number.
 */
#define FUSE_USE_VERSION 31

#include <fuse_lowlevel.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "amigafs/be.h"
#include "amigafs/bitmap.h"
#include "amigafs/blocktypes.h"
#include "amigafs/dir.h"
#include "amigafs/file.h"
#include "amigafs/links.h"
#include "amigafs/rdb.h"
#include "amigafs/translate.h"
#include "amigafs/volume.h"

#define AMIGAFS_OPT(t, p) { t, offsetof(struct options, p), 1 }

struct options {
    unsigned int uid;          /* mounting user's uid */
    unsigned int gid;
    int amiga_ids;             /* expose stored 16-bit uid/gid (D7) */
    int local_tz;              /* interpret dates as local time (D6) */
    int foreground;
    int rw;                    /* allow writes; default is a read-only mount */
};

static struct options g_opt;

/* How long the kernel may cache a name and its attributes. A read-only mount
 * can promise an hour, because nothing in it can change. A WRITABLE one
 * cannot: this shell frees blocks, so after an unlink + recreate the kernel is
 * still holding the old inode, and an inode NUMBER here is a block number --
 * it names a block that may now hold a directory or belong to a different
 * file. The write probe hit precisely that and the shell refused the write
 * with EIO, having been handed an inode that no longer named a file.
 *
 * Shortening the timeout is the only fix available: the hazard is in the NUMBER
 * the kernel cached, not in anything else it remembered about it. */
static double cache_timeout(void) { return g_opt.rw ? 0.0 : 3600.0; }

/* One mounted volume: a floppy is a single part covering the whole image; an
 * RDB image has one part per partition. */
#define AFS_MAX_PARTS 16
struct part {
    afs_volume_t *vol;
    uint32_t start;            /* absolute first block in the image */
    uint32_t size;             /* blocks visible through this window */
    char     name[32];         /* DriveName (RDB) or the volume's own name */
};
static struct part g_parts[AFS_MAX_PARTS];
static unsigned    g_nparts;
static int         g_rdb_mode;     /* the mount root lists partitions */

/* inode 1 is the mount root; real entries are (absolute block + 2) */
#define INO_ROOT        ((fuse_ino_t)1)
#define INO_BLOCK(abs)  ((fuse_ino_t)(abs) + 2u)
/* two synthetic inodes per volume: the .amigafs directory and its bitmap file */
#define SYNTH_BASE      ((fuse_ino_t)0x40000000)
#define SYNTH_MAX       (SYNTH_BASE + 2u * AFS_MAX_PARTS)
#define INO_SYNTH(vol, is_bitmap) \
    (SYNTH_BASE + (fuse_ino_t)(vol) * 2u + (is_bitmap ? 1u : 0u))
#define IS_SYNTH(ino)   ((ino) >= SYNTH_BASE && (ino) < SYNTH_MAX)

/* Map an inode back to (volume index, partition-relative block). Returns 0 on
 * success; non-zero for the mount root, synthetic nodes, and anything outside
 * every mounted volume -- all of which the caller handles itself. */
static int resolve_ino(fuse_ino_t ino, unsigned *vi, uint32_t *rel) {
    if (ino < 2 || IS_SYNTH(ino)) return -1;
    fuse_ino_t abs = ino - 2;
    for (unsigned i = 0; i < g_nparts; i++) {
        uint64_t start = g_parts[i].start;
        if (abs >= start && abs < start + g_parts[i].size) {
            *vi = i;
            *rel = (uint32_t)(abs - start);
            return 0;
        }
    }
    return -1;
}

static fuse_ino_t part_root_ino(unsigned i) {
    return INO_BLOCK(g_parts[i].start + afs_volume_rootblock(g_parts[i].vol));
}

/* Size in bytes of volume i's packed bitmap: one bit per block. */
static size_t bitmap_size(unsigned i) {
    return ((size_t)afs_volume_block_count(g_parts[i].vol) + 7u) / 8u;
}

static int is_volume_root(unsigned vi, uint32_t blk) {
    return blk == afs_volume_rootblock(g_parts[vi].vol);
}

/* `nlink` is passed in rather than hardcoded to 1: a hard link is a real
 * feature (M24) and the kernel uses st_nlink to decide whether an unlink may
 * drop the inode's data. Under-reporting it makes the kernel free a file that
 * still has names pointing at it. Callers that have the volume and the header
 * block get the real count from afs_nlink_for_block; the mount root passes 1
 * for a directory, which is what POSIX expects there. */
static void fill_stat(const afs_fileheader_t *fh, fuse_ino_t ino,
                      mode_t type, unsigned nlink, struct stat *st) {
    memset(st, 0, sizeof *st);
    st->st_ino = (ino_t)ino;
    st->st_nlink = (nlink_t)(nlink ? nlink : 1u);
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

static void synth_dir_stat(fuse_ino_t ino, struct stat *st) {
    memset(st, 0, sizeof *st);
    st->st_ino = (ino_t)ino;
    st->st_mode = S_IFDIR | 0555;
    st->st_nlink = 2;
    st->st_size = 512;
    afs_translate_ids(g_opt.uid, g_opt.gid, 0, 0, g_opt.amiga_ids, st);
}

static void synth_file_stat(fuse_ino_t ino, size_t size, struct stat *st) {
    memset(st, 0, sizeof *st);
    st->st_ino = (ino_t)ino;
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = (off_t)size;
    afs_translate_ids(g_opt.uid, g_opt.gid, 0, 0, g_opt.amiga_ids, st);
}

/* ---------------------------------------------------------------------- */
/* lookup                                                                  */
/* ---------------------------------------------------------------------- */

static void reply_synth(fuse_req_t req, fuse_ino_t ino, mode_t type, size_t size) {
    struct fuse_entry_param e;
    memset(&e, 0, sizeof e);
    e.ino = ino;
    e.generation = 0;
    e.attr_timeout = cache_timeout();
    e.entry_timeout = cache_timeout();
    if (type == S_IFDIR) synth_dir_stat(ino, &e.attr);
    else                synth_file_stat(ino, size, &e.attr);
    fuse_reply_entry(req, &e);
}

static void afs_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct fuse_entry_param e;
    afs_dirent_t de;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        memset(&e, 0, sizeof e);
        e.ino = parent;
        e.attr_timeout = cache_timeout();
        e.entry_timeout = cache_timeout();
        synth_dir_stat(parent, &e.attr);
        fuse_reply_entry(req, &e);
        return;
    }

    /* the mount root of an RDB image lists partitions, not files */
    if (parent == INO_ROOT && g_rdb_mode) {
        for (unsigned i = 0; i < g_nparts; i++) {
            if (strcmp(name, g_parts[i].name) == 0) {
                reply_synth(req, part_root_ino(i), S_IFDIR, 0);
                return;
            }
        }
        fuse_reply_err(req, ENOENT);
        return;
    }

    unsigned vi = 0;
    uint32_t parentblk = 0;

    if (parent == INO_ROOT) {
        vi = 0;
        parentblk = afs_volume_rootblock(g_parts[0].vol);
    } else if (IS_SYNTH(parent)) {
        vi = (unsigned)((parent - SYNTH_BASE) / 2u);
        if ((parent - SYNTH_BASE) % 2u == 0) {
            /* parent is a `.amigafs` DIRECTORY: it holds exactly one entry */
            if (vi >= g_nparts || strcmp(name, "bitmap") != 0) {
                fuse_reply_err(req, ENOENT);
                return;
            }
            reply_synth(req, INO_SYNTH(vi, 1), S_IFREG, bitmap_size(vi));
            return;
        }
        /* parent is the bitmap FILE: nothing can be looked up inside it */
        fuse_reply_err(req, ENOTDIR);
        return;
    } else if (resolve_ino(parent, &vi, &parentblk) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    /* `.amigafs` hangs off every volume's ROOT -- the mount root of a floppy,
     * and the root of EACH PARTITION of an RDB image, so every volume exposes
     * its own bitmap. It is deliberately NOT at the top of an RDB mount, where
     * there is no single volume whose bitmap it could be. The check is on the
     * resolved parent block, not on `parent == INO_ROOT`: a partition root is
     * an ordinary inode, and keying off the mount root alone left every
     * partition's `.amigafs` listed by readdir but unreachable by lookup. */
    if (is_volume_root(vi, parentblk) && strcmp(name, ".amigafs") == 0) {
        reply_synth(req, INO_SYNTH(vi, 0), S_IFDIR, 0);
        return;
    }

    afs_volume_t *vol = g_parts[vi].vol;
    if (afs_dir_lookup(vol, parentblk, name, strlen(name), &de) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    /* A directory hard link must NOT be walked (C12) -- a hard-linked directory
     * is a cycle the kernel dentry cache cannot represent. */
    if (de.kind == AFS_BLOCK_LINKDIR) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    size_t bs = afs_volume_block_size(vol);
    uint8_t *blk = malloc(bs);
    if (!blk) { fuse_reply_err(req, ENOMEM); return; }
    if (afs_volume_read_block(vol, de.block, blk, bs) != 0) {
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
        if (afs_link_resolve_file(vol, parentblk, name, strlen(name), &real) == 0)
            ino_blk = real;
    }
    if (ino_blk != de.block) {
        if (afs_volume_read_block(vol, ino_blk, blk, bs) != 0) {
            free(blk);
            fuse_reply_err(req, EIO);
            return;
        }
    }

    e.ino = INO_BLOCK(g_parts[vi].start + ino_blk);
    e.generation = 0;
    /* A read-only mount may promise its names and attributes are stable for
     * an hour, because nothing can change them. A WRITABLE mount cannot: this
     * shell frees blocks, and after a unlink + recreate the kernel is still
     * holding the old inode -- pointing at a block that now holds a directory,
     * or has been handed to a different file. The write probe hit exactly
     * that: a write landed on a freed block and the shell refused it with EIO,
     * having been handed an inode that no longer names a file.
     *
     * Short timeouts are the fix, not shorter-lived inodes: the inode NUMBER
     * is a block number, so it cannot be made safe by expiring anything the
     * kernel caches about it. */
    e.attr_timeout = cache_timeout();
    e.entry_timeout = cache_timeout();

    afs_block_kind_t k = afs_block_kind(blk, bs);
    if (k == AFS_BLOCK_ROOT) {
        synth_dir_stat(e.ino, &e.attr);
    } else {
        afs_fileheader_t h;
        memset(&h, 0, sizeof h);
        if (afs_parse_fileheader(blk, bs, &h) != 0) memset(&h, 0, sizeof h);
        unsigned nl = 1;
        (void)afs_nlink_for_block(vol, ino_blk, &nl);
        fill_stat(&h, e.ino, kind_to_mode(k), nl, &e.attr);
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

/* ---------------------------------------------------------------------- */
/* getattr / opendir                                                       */
/* ---------------------------------------------------------------------- */

static void afs_ll_getattr(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *fi) {
    (void)fi;
    struct stat st;

    if (ino == INO_ROOT) {
        synth_dir_stat(ino, &st);
        fuse_reply_attr(req, &st, cache_timeout());
        return;
    }
    if (IS_SYNTH(ino)) {
        unsigned vi = (unsigned)((ino - SYNTH_BASE) / 2u);
        if ((ino - SYNTH_BASE) % 2u == 0) synth_dir_stat(ino, &st);
        else synth_file_stat(ino, vi < g_nparts ? bitmap_size(vi) : 0, &st);
        fuse_reply_attr(req, &st, cache_timeout());
        return;
    }

    unsigned vi = 0;
    uint32_t blkno = 0;
    if (resolve_ino(ino, &vi, &blkno) != 0) { fuse_reply_err(req, ENOENT); return; }
    afs_volume_t *vol = g_parts[vi].vol;
    size_t bs = afs_volume_block_size(vol);

    uint8_t *blk = malloc(bs);
    if (!blk) { fuse_reply_err(req, ENOMEM); return; }
    if (afs_volume_read_block(vol, blkno, blk, bs) != 0) {
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
        synth_dir_stat(ino, &st);
    } else {
        afs_fileheader_t h;
        memset(&h, 0, sizeof h);
        afs_parse_fileheader(blk, bs, &h);
        unsigned nl = 1;
        (void)afs_nlink_for_block(g_parts[vi].vol, blkno, &nl);
        fill_stat(&h, ino, kind_to_mode(k), nl, &st);
        if (k == AFS_BLOCK_SOFTLINK) st.st_size = 0;
    }
    free(blk);
    fuse_reply_attr(req, &st, cache_timeout());
}

static void afs_ll_opendir(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info *fi) {
    if (ino == INO_ROOT || IS_SYNTH(ino)) { fuse_reply_open(req, fi); return; }

    unsigned vi = 0;
    uint32_t blkno = 0;
    if (resolve_ino(ino, &vi, &blkno) != 0) { fuse_reply_err(req, ENOTDIR); return; }
    afs_volume_t *vol = g_parts[vi].vol;
    size_t bs = afs_volume_block_size(vol);
    uint8_t *blk = malloc(bs);
    if (!blk) { fuse_reply_err(req, ENOMEM); return; }
    afs_volume_read_block(vol, blkno, blk, bs);
    afs_block_kind_t k = afs_block_kind(blk, bs);
    free(blk);
    if (k != AFS_BLOCK_DIR && k != AFS_BLOCK_ROOT) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }
    fuse_reply_open(req, fi);
}

/* ---------------------------------------------------------------------- */
/* readdir                                                                 */
/* ---------------------------------------------------------------------- */

#define RENT_MAX 1024
struct rent { char name[64]; fuse_ino_t ino; mode_t mode; unsigned nlink; };

static size_t rent_add(struct rent *list, size_t n, const char *name,
                       fuse_ino_t ino, mode_t mode, unsigned nlink) {
    if (n >= RENT_MAX) return n;
    snprintf(list[n].name, sizeof list[n].name, "%s", name);
    list[n].ino = ino;
    list[n].mode = mode;
    list[n].nlink = nlink;
    return n + 1;
}

/* Add a directory's real entries. Shared by the mount root (of a single-volume
 * image) and by every ordinary directory. */
static size_t rent_add_dir(afs_volume_t *vol, unsigned vi, uint32_t dirblk,
                           struct rent *list, size_t n) {
    afs_dirent_t ents[256];
    size_t count = 0;
    if (afs_dir_list(vol, dirblk, ents, 256, &count) != 0) return (size_t)-1;
    for (size_t i = 0; i < count; i++) {
        uint32_t ino_blk = ents[i].block;
        mode_t mode;
        if (ents[i].kind == AFS_BLOCK_LINKFILE) {
            uint32_t real = 0;
            if (afs_link_resolve_file(vol, dirblk, ents[i].name,
                                      strlen(ents[i].name), &real) == 0)
                ino_blk = real;
        }
        if (ents[i].kind == AFS_BLOCK_DIR)           mode = S_IFDIR | 0555;
        else if (ents[i].kind == AFS_BLOCK_SOFTLINK) mode = S_IFLNK | 0777;
        else if (ents[i].kind == AFS_BLOCK_LINKDIR)  mode = S_IFLNK | 0777;
        else                                         mode = S_IFREG | 0444;
        n = rent_add(list, n, ents[i].name,
                     INO_BLOCK(g_parts[vi].start + ino_blk), mode,
                     (mode & S_IFMT) == S_IFDIR ? 2u : 1u);
    }
    return n;
}

static void afs_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                           off_t off, struct fuse_file_info *fi) {
    (void)size; (void)fi;
    struct rent list[RENT_MAX];
    size_t n = 0;

    if (ino == INO_ROOT && g_rdb_mode) {
        for (unsigned i = 0; i < g_nparts; i++)
            n = rent_add(list, n, g_parts[i].name, part_root_ino(i),
                         S_IFDIR | 0555, 2);
    } else if (ino == INO_ROOT) {
        n = rent_add(list, n, ".amigafs", INO_SYNTH(0, 0), S_IFDIR | 0555, 2);
        size_t m = rent_add_dir(g_parts[0].vol, 0,
                                afs_volume_rootblock(g_parts[0].vol),
                                list, n);
        if (m == (size_t)-1) { fuse_reply_err(req, EIO); return; }
        n = m;
    } else if (IS_SYNTH(ino)) {
        unsigned vi = (unsigned)((ino - SYNTH_BASE) / 2u);
        if ((ino - SYNTH_BASE) % 2u == 0 && vi < g_nparts)
            n = rent_add(list, n, "bitmap", INO_SYNTH(vi, 1), S_IFREG | 0444, 1);
    } else {
        unsigned vi = 0;
        uint32_t dirblk = 0;
        if (resolve_ino(ino, &vi, &dirblk) != 0) {
            fuse_reply_err(req, ENOENT);
            return;
        }
        /* a volume's own root also carries `.amigafs` */
        if (is_volume_root(vi, dirblk))
            n = rent_add(list, n, ".amigafs", INO_SYNTH(vi, 0), S_IFDIR | 0555, 2);
        size_t m = rent_add_dir(g_parts[vi].vol, vi, dirblk, list, n);
        if (m == (size_t)-1) { fuse_reply_err(req, EIO); return; }
        n = m;
    }

    /* Each entry carries a stable cookie: its 1-based index, where 1 is "." and
     * 2 is "..". The kernel resumes by handing that cookie back as `off`. */
    char *buf = malloc(16384);
    if (!buf) { fuse_reply_err(req, ENOMEM); return; }
    size_t used = 0;

    if (off == 0) {
        struct stat st;
        memset(&st, 0, sizeof st);
        synth_dir_stat(ino, &st);
        used += (size_t)fuse_add_direntry(req, buf + used, 16384 - used, ".", &st, 1);
        st.st_ino = (ino_t)ino;
        used += (size_t)fuse_add_direntry(req, buf + used, 16384 - used, "..", &st, 2);
    }
    for (size_t i = (off <= 2 ? 0 : (size_t)off - 2); i < n; i++) {
        struct stat st;
        memset(&st, 0, sizeof st);
        st.st_ino = (ino_t)list[i].ino;
        st.st_mode = list[i].mode;
        st.st_nlink = (nlink_t)list[i].nlink;
        size_t need = fuse_add_direntry(req, NULL, 0, list[i].name, &st,
                                        (off_t)(i + 3));
        if (used + need > 16384) break;
        used += (size_t)fuse_add_direntry(req, buf + used, 16384 - used,
                                          list[i].name, &st, (off_t)(i + 3));
    }

    fuse_reply_buf(req, buf, used);
    free(buf);
}

/* ---------------------------------------------------------------------- */
/* open / read / readlink                                                   */
/* ---------------------------------------------------------------------- */

/* Library error text -> errno, in ONE place.
 *
 * Seven write ops each called several library functions that can all refuse,
 * and each had grown its own ad-hoc strstr chain. They had drifted: only the
 * write op knew about ENOSPC, so a create or mkdir that ran out of room
 * answered EIO. The write lane caught it as an "unsanctioned errno" on a
 * perfectly healthy volume that had simply filled up -- which is exactly the
 * failure a user sees as an unexplained I/O error instead of "no space".
 *
 * Order matters: the more specific phrases are tested first ("is a directory"
 * before anything that merely contains "directory").
 */
static int err_to_errno(const char *msg) {
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
    return EIO;
}

static int refuse_if_ro(fuse_req_t req);

/* fi->fh carries the O_APPEND bit only; the block comes from the inode, as
 * every read does. The kernel does NOT apply O_APPEND for us when writeback
 * caching is off (fuse_lowlevel.h), so an appending write must redirect to the
 * current end of file itself. Defined here because open() sets it too. */
#define FH_APPEND 1u

static void afs_ll_open(fuse_req_t req, fuse_ino_t ino,
                        struct fuse_file_info *fi) {
    if (IS_SYNTH(ino)) {
        if ((ino - SYNTH_BASE) % 2u == 0) { fuse_reply_err(req, EISDIR); return; }
        /* the bitmap is DERIVED from the volume's own map blocks: writing it
         * would not change anything a reader ever sees, so it stays read-only
         * even on an rw mount */
        if ((fi->flags & O_ACCMODE) != O_RDONLY) {
            fuse_reply_err(req, EROFS);
            return;
        }
        fuse_reply_open(req, fi);
        return;
    }
    unsigned vi = 0;
    uint32_t blkno = 0;
    if (resolve_ino(ino, &vi, &blkno) != 0) { fuse_reply_err(req, ENOENT); return; }
    afs_volume_t *vol = g_parts[vi].vol;
    size_t bs = afs_volume_block_size(vol);
    uint8_t *blk = malloc(bs);
    if (!blk) { fuse_reply_err(req, ENOMEM); return; }
    afs_volume_read_block(vol, blkno, blk, bs);
    afs_block_kind_t k = afs_block_kind(blk, bs);
    free(blk);
    if (k == AFS_BLOCK_DIR || k == AFS_BLOCK_ROOT) {
        fuse_reply_err(req, EISDIR);
        return;
    }
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        if (refuse_if_ro(req)) return;
        /* O_TRUNC on an open: POSIX says the open itself empties the file. */
        if (fi->flags & O_TRUNC) {
            char err[256] = {0};
            if (afs_file_write_all(vol, blkno, NULL, 0, err, sizeof err) != 0) {
                fuse_reply_err(req, EIO);
                return;
            }
        }
    }
    fi->fh = (fi->flags & O_APPEND) ? FH_APPEND : 0;
    fuse_reply_open(req, fi);
}

static void afs_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                        struct fuse_file_info *fi) {
    (void)fi;

    /* the synthetic bitmap: real bytes, served straight from the volume */
    if (IS_SYNTH(ino) && (ino - SYNTH_BASE) % 2u == 1) {
        unsigned vi = (unsigned)((ino - SYNTH_BASE) / 2u);
        if (vi >= g_nparts) { fuse_reply_err(req, EIO); return; }
        size_t total = bitmap_size(vi);
        if ((size_t)off >= total) { fuse_reply_buf(req, NULL, 0); return; }
        if ((size_t)off + size > total) size = total - (size_t)off;
        uint8_t *map = malloc(total ? total : 1);
        if (!map) { fuse_reply_err(req, ENOMEM); return; }
        if (afs_bitmap_read(g_parts[vi].vol, map, total) < 0) {
            free(map);
            fuse_reply_err(req, EIO);
            return;
        }
        fuse_reply_buf(req, (const char *)map + off, size);
        free(map);
        return;
    }

    unsigned vi = 0;
    uint32_t blkno = 0;
    if (resolve_ino(ino, &vi, &blkno) != 0) { fuse_reply_err(req, EIO); return; }
    afs_volume_t *vol = g_parts[vi].vol;
    size_t bs = afs_volume_block_size(vol);
    uint8_t *blk = malloc(bs);
    if (!blk) { fuse_reply_err(req, ENOMEM); return; }
    if (afs_volume_read_block(vol, blkno, blk, bs) != 0) {
        free(blk);
        fuse_reply_err(req, EIO);
        return;
    }
    afs_fileheader_t h;
    memset(&h, 0, sizeof h);
    afs_parse_fileheader(blk, bs, &h);
    uint32_t byte_size = h.byte_size;
    free(blk);

    if ((uint64_t)off >= byte_size) { fuse_reply_buf(req, NULL, 0); return; }
    if ((uint64_t)off + size > byte_size) size = byte_size - (size_t)off;

    char *buf = malloc(size ? size : 1);
    if (!buf) { fuse_reply_err(req, ENOMEM); return; }
    size_t got = 0;
    /* read by header block: FUSE hands us an inode, not a path */
    if (afs_read_file_by_block(vol, blkno, buf, size, (uint64_t)off, &got) != 0) {
        free(buf);
        fuse_reply_err(req, EIO);
        return;
    }
    fuse_reply_buf(req, buf, got);
    free(buf);
}

static void afs_ll_readlink(fuse_req_t req, fuse_ino_t ino) {
    unsigned vi = 0;
    uint32_t blkno = 0;
    if (resolve_ino(ino, &vi, &blkno) != 0) { fuse_reply_err(req, EINVAL); return; }
    afs_volume_t *vol = g_parts[vi].vol;
    size_t bs = afs_volume_block_size(vol);
    uint8_t *blk = malloc(bs);
    if (!blk) { fuse_reply_err(req, ENOMEM); return; }
    if (afs_volume_read_block(vol, blkno, blk, bs) != 0) {
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

/* ---------------------------------------------------------------------- */
/* write operations                                                        */
/* ---------------------------------------------------------------------- */

/* Every mutating op refuses on a read-only mount FIRST, before it resolves a
 * name or touches a block. A default mount therefore cannot be half-mutated
 * by a path that fails late, and the refusal does not depend on whether the
 * path happens to exist. */
static int refuse_if_ro(fuse_req_t req) {
    if (!g_opt.rw) { fuse_reply_err(req, EROFS); return 1; }
    return 0;
}

/* AmigaDOS protection bit 0x4 is W, ACTIVE-LOW: set means "no write". Enforced
 * on content mutation only (name operations are not gated on it) so a
 * read-only file in an existing image cannot be rewritten through the mount
 * just because the mount is rw. */
#define AFSF_W 0x4u

static int writable(afs_volume_t *vol, uint32_t blk, size_t bs) {
    uint8_t *b = malloc(bs);
    uint32_t prot;
    if (!b) return 0;
    if (afs_volume_read_block(vol, blk, b, bs) != 0) { free(b); return 0; }
    prot = be32(b + bs - 192);
    free(b);
    return (prot & AFSF_W) == 0;
}

/* Resolve the directory block a mutating op must act inside.
 * Returns 0 on success, or one of the reply codes below so each caller can
 * forward it without inventing an errno. */
static int parent_dirblock(fuse_ino_t parent, unsigned *vi, uint32_t *blk) {
    if (parent == INO_ROOT) {
        /* the mount root of an RDB image lists partitions; it is not a
         * directory anyone can create in */
        if (g_rdb_mode) return EOPNOTSUPP;
        *vi = 0;
        *blk = afs_volume_rootblock(g_parts[0].vol);
        return 0;
    }
    if (IS_SYNTH(parent)) return EROFS;   /* .amigafs is derived, never written */
    if (resolve_ino(parent, vi, blk) != 0) return ENOENT;
    return 0;
}

/* Fill an entry param from a block: the same stat shape lookup hands back, so
 * a freshly created file and a re-opened one are indistinguishable. Returns 0
 * on success, -1 if the block cannot be read (caller owns the reply). */
static int entry_for_block(unsigned vi, uint32_t blk,
                           struct fuse_entry_param *e) {
    afs_volume_t *vol = g_parts[vi].vol;
    size_t bs = afs_volume_block_size(vol);
    uint8_t *b = malloc(bs);
    if (!b) return -1;
    if (afs_volume_read_block(vol, blk, b, bs) != 0) { free(b); return -1; }
    memset(e, 0, sizeof *e);
    e->ino = INO_BLOCK(g_parts[vi].start + blk);
    e->generation = 0;
    e->attr_timeout = 0;      /* a block we just wrote has fresh attrs */
    e->entry_timeout = 0;
    afs_block_kind_t k = afs_block_kind(b, bs);
    if (k == AFS_BLOCK_ROOT) {
        synth_dir_stat(e->ino, &e->attr);
    } else {
        afs_fileheader_t h;
        memset(&h, 0, sizeof h);
        if (afs_parse_fileheader(b, bs, &h) != 0) memset(&h, 0, sizeof h);
        unsigned nl = 1;
        (void)afs_nlink_for_block(vol, blk, &nl);
        fill_stat(&h, e->ino, kind_to_mode(k), nl, &e->attr);
        if (e->attr.st_mode == 0) e->attr.st_mode = S_IFREG | 0644;
    }
    free(b);
    return 0;
}

/* Entry reply for a block we just created or found. `created` must be non-zero
 * ONLY from the create op, which owes the kernel an open reply too --
 * fuse_reply_entry there leaves the create request half-answered, and the
 * first write afterwards fails with EINVAL from libfuse ("writing device:
 * Invalid argument") with no diagnostic anywhere near the cause. */
static void reply_entry_block(fuse_req_t req, unsigned vi, uint32_t blk,
                              struct fuse_file_info *fi, int created) {
    struct fuse_entry_param e;
    if (entry_for_block(vi, blk, &e) != 0) { fuse_reply_err(req, EIO); return; }
    if (created) fuse_reply_create(req, &e, fi);
    else         fuse_reply_entry(req, &e);
}

static void afs_ll_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                          mode_t mode, struct fuse_file_info *fi) {
    (void)mode;
    if (refuse_if_ro(req)) return;
    unsigned vi = 0;
    uint32_t pblk = 0, hdr = 0;
    int rc = parent_dirblock(parent, &vi, &pblk);
    if (rc != 0) { fuse_reply_err(req, rc); return; }
    afs_volume_t *vol = g_parts[vi].vol;
    char err[256] = {0};
    afs_dirent_t existing;

    if (afs_dir_lookup(vol, pblk, name, strlen(name), &existing) == 0) {
        /* POSIX open(O_CREAT) without O_EXCL on an existing file just opens it;
         * O_EXCL is an error. O_TRUNC empties it (same fopen "w" path the CLI
         * uses, so a truncating open cannot leave a stale tail). */
        if (fi->flags & O_EXCL) { fuse_reply_err(req, EEXIST); return; }
        if (fi->flags & O_TRUNC) {
            if (afs_file_write_all(vol, existing.block, NULL, 0,
                                   err, sizeof err) != 0) {
                fuse_reply_err(req, EIO);
                return;
            }
        }
        fi->fh = (fi->flags & O_APPEND) ? FH_APPEND : 0;
        reply_entry_block(req, vi, existing.block, fi, 1);
        return;
    }

    if (afs_file_create(vol, pblk, name, strlen(name), &hdr,
                        err, sizeof err) != 0) {
        fuse_reply_err(req, err_to_errno(err));
        return;
    }
    fi->fh = (fi->flags & O_APPEND) ? FH_APPEND : 0;
    reply_entry_block(req, vi, hdr, fi, 1);
}

static void afs_ll_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
                         size_t size, off_t off, struct fuse_file_info *fi) {
    if (refuse_if_ro(req)) return;
    if (off < 0) { fuse_reply_err(req, EIO); return; }
    if (size == 0) { fuse_reply_write(req, 0); return; }
    unsigned vi = 0;
    uint32_t blk = 0;
    if (resolve_ino(ino, &vi, &blk) != 0) { fuse_reply_err(req, ENOENT); return; }
    afs_volume_t *vol = g_parts[vi].vol;
    size_t bs = afs_volume_block_size(vol);
    if (!writable(vol, blk, bs)) { fuse_reply_err(req, EACCES); return; }

    uint64_t at = (uint64_t)off;
    if (fi && (fi->fh & FH_APPEND)) {
        uint8_t *hb = malloc(bs);
        if (!hb) { fuse_reply_err(req, ENOMEM); return; }
        if (afs_volume_read_block(vol, blk, hb, bs) != 0) {
            free(hb);
            fuse_reply_err(req, EIO);
            return;
        }
        at = be32(hb + bs - 188);
        free(hb);
    }

    char err[256] = {0};
    if (afs_file_write_range(vol, blk, buf, size, at, err, sizeof err) != 0) {
        /* a full volume is the common real failure and deserves its own errno;
         * everything else here is an I/O or structure refusal */
        fuse_reply_err(req, err_to_errno(err));
        return;
    }
    fuse_reply_write(req, size);
}

static void afs_ll_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
                         mode_t mode) {
    (void)mode;
    if (refuse_if_ro(req)) return;
    unsigned vi = 0;
    uint32_t pblk = 0, nb = 0;
    int rc = parent_dirblock(parent, &vi, &pblk);
    if (rc != 0) { fuse_reply_err(req, rc); return; }
    char err[256] = {0};
    if (afs_dir_mkdir(g_parts[vi].vol, pblk, name, strlen(name), &nb,
                      err, sizeof err) != 0) {
        fuse_reply_err(req, err_to_errno(err));
        return;
    }
    reply_entry_block(req, vi, nb, NULL, 0);
}

static void afs_ll_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
    if (refuse_if_ro(req)) return;
    unsigned vi = 0;
    uint32_t pblk = 0;
    int rc = parent_dirblock(parent, &vi, &pblk);
    if (rc != 0) { fuse_reply_err(req, rc); return; }
    afs_volume_t *vol = g_parts[vi].vol;
    afs_dirent_t e;
    if (afs_dir_lookup(vol, pblk, name, strlen(name), &e) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    char err[256] = {0};
    if (e.kind == AFS_BLOCK_FILE) {
        if (afs_file_delete(vol, pblk, name, strlen(name), err, sizeof err) != 0) {
            fuse_reply_err(req, EIO);
            return;
        }
    } else if (e.kind == AFS_BLOCK_DIR) {
        if (afs_dir_rmdir(vol, pblk, name, strlen(name), err, sizeof err) != 0) {
            /* non-empty is ENOTEMPTY, not EIO: `rm -r` and every POSIX caller
             * branch on it */
            fuse_reply_err(req, err_to_errno(err));
            return;
        }
    } else if (e.kind == AFS_BLOCK_LINKFILE) {
        /* A hard link is just a name. Removing it frees only its own header
         * block; the target and its data are untouched, and afs_dir_unlink
         * also unhooks the next_link chain so nlink stays right (M24).
         * Previously every non-file/dir fell through to err_to_errno() with an
         * EMPTY errbuf, so the errno came from a string that never said
         * anything. */
        uint32_t gone = 0;
        if (afs_dir_unlink(vol, pblk, name, strlen(name), &gone,
                           err, sizeof err) != 0) {
            fuse_reply_err(req, err_to_errno(err));
            return;
        }
    } else {
        /* A soft link cannot be removed without touching its target's name, so
         * it stays refused -- but now with a REASON rather than an empty one. */
        snprintf(err, sizeof err, "soft link removal is not supported");
        fuse_reply_err(req, EACCES);
        return;
    }
    fuse_reply_err(req, 0);
}

/* POSIX link(old, new) -- a hard link to an existing FILE (M24).
 *
 * libfuse's low-level signature hands us the TARGET'S INODE rather than a name,
 * and the directory the NEW name goes in, so the two may live in different
 * directories: `ln a sub/b` arrives with the inode of `a` and newparent = sub. */
static void afs_ll_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                        const char *newname) {
    unsigned tvi = 0, nvi = 0;
    uint32_t tblk = 0, nblk = 0;
    size_t bs;
    uint8_t *tb;
    afs_block_kind_t kind;

    if (refuse_if_ro(req)) return;
    if (resolve_ino(ino, &tvi, &tblk) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    if (newparent == FUSE_ROOT_ID) newparent = INO_ROOT;
    if (parent_dirblock(newparent, &nvi, &nblk) != 0) {
        fuse_reply_err(req, EIO);
        return;
    }
    /* One filesystem: a link across volumes would be a cross-device rename. */
    if (tvi != nvi) {
        fuse_reply_err(req, EXDEV);
        return;
    }

    afs_volume_t *vol = g_parts[tvi].vol;
    bs = afs_volume_block_size(vol);
    tb = malloc(bs);
    if (!tb) { fuse_reply_err(req, ENOMEM); return; }
    if (afs_volume_read_block(vol, tblk, tb, bs) != 0) {
        free(tb);
        fuse_reply_err(req, EIO);
        return;
    }
    kind = afs_block_kind(tb, bs);
    free(tb);

    /* POSIX link() on a directory is EPERM, and AmigaDOS has no directory hard
     * link we could create anyway -- it would be the cycle C12 avoids. */
    if (kind == AFS_BLOCK_DIR || kind == AFS_BLOCK_LINKDIR) {
        fuse_reply_err(req, EPERM);
        return;
    }
    if (kind != AFS_BLOCK_FILE && kind != AFS_BLOCK_LINKFILE) {
        fuse_reply_err(req, EINVAL);
        return;
    }

    /* Linking a name that is ITSELF a hard link must point at the real file,
     * not nest another link: afs_file_link resolves through real_entry. */
    char err[256] = {0};
    uint32_t link = 0;
    if (afs_file_link(vol, nblk, newname, strlen(newname), tblk,
                      &link, err, sizeof err) != 0) {
        fuse_reply_err(req, err_to_errno(err));
        return;
    }
    /* A `.link` op accepts ONLY fuse_reply_entry or fuse_reply_err
     * (fuse_lowlevel.h). A bare success fuse_reply_err(req, 0) makes libfuse
     * fail the reply with "writing device: Invalid argument", so `ln` reports
     * EIO even though the link was correctly written to the volume. Reply with
     * the new entry, as mkdir does. */
    reply_entry_block(req, nvi, link, NULL, 0);
}

static void afs_ll_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
    if (refuse_if_ro(req)) return;
    unsigned vi = 0;
    uint32_t pblk = 0;
    int rc = parent_dirblock(parent, &vi, &pblk);
    if (rc != 0) { fuse_reply_err(req, rc); return; }
    afs_volume_t *vol = g_parts[vi].vol;
    char err[256] = {0};
    if (afs_dir_rmdir(vol, pblk, name, strlen(name), err, sizeof err) != 0) {
        fuse_reply_err(req, err_to_errno(err));
        return;
    }
    fuse_reply_err(req, 0);
}

static void afs_ll_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                          fuse_ino_t newparent, const char *newname,
                          unsigned int flags) {
    if (refuse_if_ro(req)) return;
    /* RENAME_NOREPLACE / RENAME_EXCHANGE need a two-name transaction we do
     * not have; say so rather than doing a plain rename under a flag that
     * promises something else (a plain rename here would silently overwrite
     * for NOREPLACE and silently do nothing for EXCHANGE). */
    if (flags != 0) { fuse_reply_err(req, EINVAL); return; }
    unsigned vi = 0, nvi = 0;
    uint32_t sblk = 0, nblk = 0;
    int rc = parent_dirblock(parent, &vi, &sblk);
    if (rc != 0) { fuse_reply_err(req, rc); return; }
    rc = parent_dirblock(newparent, &nvi, &nblk);
    if (rc != 0) { fuse_reply_err(req, rc); return; }
    /* Two partitions of one image are two volumes: a rename across them is a
     * copy+delete, and POSIX says a rename that cannot be done in place fails
     * with EXDEV (GNU mv falls back by itself on seeing it). */
    if (vi != nvi) { fuse_reply_err(req, EXDEV); return; }

    char err[256] = {0};
    if (afs_dir_rename(g_parts[vi].vol, sblk, name, strlen(name),
                       nblk, newname, strlen(newname), err, sizeof err) != 0) {
        fuse_reply_err(req, err_to_errno(err));
        return;
    }
    fuse_reply_err(req, 0);
}

/* setattr: size changes are real writes; everything else (mode, owner, times)
 * has nowhere to live in an AmigaDOS entry and is refused rather than
 * silently ignored -- a caller that chmods and believes it worked is worse
 * than one that gets EPERM. */
static void afs_ll_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                           int to_set, struct fuse_file_info *fi) {
    (void)fi;
    if (to_set & ~(FUSE_SET_ATTR_SIZE)) { fuse_reply_err(req, EPERM); return; }
    if (!(to_set & FUSE_SET_ATTR_SIZE)) {
        /* even a no-op setattr owes the kernel its attrs (see below) */
        unsigned vi0 = 0;
        uint32_t blk0 = 0;
        struct fuse_entry_param e;
        if (resolve_ino(ino, &vi0, &blk0) != 0) { fuse_reply_err(req, ENOENT); return; }
        if (entry_for_block(vi0, blk0, &e) != 0) { fuse_reply_err(req, EIO); return; }
        fuse_reply_attr(req, &e.attr, e.attr_timeout);
        return;
    }
    if (refuse_if_ro(req)) return;
    unsigned vi = 0;
    uint32_t blk = 0;
    if (resolve_ino(ino, &vi, &blk) != 0) { fuse_reply_err(req, ENOENT); return; }
    afs_volume_t *vol = g_parts[vi].vol;
    size_t bs = afs_volume_block_size(vol);
    if (!writable(vol, blk, bs)) { fuse_reply_err(req, EACCES); return; }
    if (attr->st_size < 0) { fuse_reply_err(req, EINVAL); return; }

    uint32_t want = (uint32_t)attr->st_size;
    char err[256] = {0};
    /* POSIX truncate: shrink drops the tail, grow zero-fills. Doing it as
     * "read `want` bytes into a zeroed buffer, write them back" is exact for
     * BOTH, and it reuses the one tested rewrite path (write_all) instead of
     * adding a second truncate implementation that could disagree with it. */
    uint8_t *buf = calloc(want ? want : 1, 1);
    if (!buf) { fuse_reply_err(req, ENOMEM); return; }
    size_t got = 0;
    if (afs_read_file_by_block(vol, blk, buf, want, 0, &got) != 0) {
        free(buf);
        fuse_reply_err(req, EIO);
        return;
    }
    if (afs_file_write_all(vol, blk, buf, want, err, sizeof err) != 0) {
        free(buf);
        fuse_reply_err(req, err_to_errno(err));
        return;
    }
    free(buf);
    /* setattr MUST answer with the attributes, not a bare error code: the
     * kernel's FUSE_SETATTR reply carries a struct attr_out, and replying with
     * a zero-length error payload makes libfuse's write of the reply fail with
     * EINVAL ("writing device: Invalid argument") -- the truncate appears to be
     * refused by the filesystem while in fact it succeeded. */
    {
        struct fuse_entry_param e;
        if (entry_for_block(vi, blk, &e) != 0) { fuse_reply_err(req, EIO); return; }
        fuse_reply_attr(req, &e.attr, e.attr_timeout);
    }
}

/* flush/release: no per-open state to flush (every write reached the image via
 * pwrite before it replied) and nothing to keep alive, so both are success.
 * They are here rather than NULL so the intent is explicit -- libfuse's
 * defaults happen to be the same. */
static void afs_ll_flush(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi) {
    (void)ino; (void)fi;
    fuse_reply_err(req, 0);
}

static void afs_ll_release(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info *fi) {
    (void)ino; (void)fi;
    fuse_reply_err(req, 0);
}

/* ---------------------------------------------------------------------- */
/* extended attributes: the AmigaDOS comment                               */
/* ---------------------------------------------------------------------- */

/* One attribute, and it is the only thing an AmigaDOS entry carries that a
 * POSIX node has no other way to express. */
#define AFS_XATTR_COMMENT "user.amiga.comment"

/* Read the comment for an inode into `out`. Returns 0 on success (which
 * includes "this entry has no comment", yielding an empty string), or non-zero
 * if the inode is not a real entry block. */
static int comment_for_ino(fuse_ino_t ino, char *out, size_t cap) {
    if (out && cap) out[0] = '\0';
    unsigned vi = 0;
    uint32_t blkno = 0;
    if (resolve_ino(ino, &vi, &blkno) != 0) return -1;
    afs_volume_t *vol = g_parts[vi].vol;
    size_t bs = afs_volume_block_size(vol);
    uint8_t *blk = malloc(bs);
    if (!blk) return -1;
    if (afs_volume_read_block(vol, blkno, blk, bs) != 0) {
        free(blk);
        return -1;
    }
    afs_entry_comment(blk, bs, out, cap);
    free(blk);
    return 0;
}

/* An entry that has no comment must NOT advertise one: listing a name that
 * getxattr then refuses to serve is worse than listing nothing. */
static int ino_has_comment(fuse_ino_t ino) {
    char c[256];
    return comment_for_ino(ino, c, sizeof c) == 0 && c[0] != '\0';
}

static void afs_ll_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                            size_t size) {
    if (strcmp(name, AFS_XATTR_COMMENT) != 0) {
        fuse_reply_err(req, ENODATA);
        return;
    }
    if (ino == INO_ROOT || IS_SYNTH(ino)) {
        /* the mount root and the synthetic nodes are not AmigaDOS entries */
        fuse_reply_err(req, ENODATA);
        return;
    }
    char cmt[256];
    if (comment_for_ino(ino, cmt, sizeof cmt) != 0) {
        fuse_reply_err(req, ENODATA);
        return;
    }
    /* The value is the comment's bytes with NO trailing NUL. That is what every
     * other user.* attribute on Linux holds, and the length is already carried
     * by the reply, so a NUL would be a surprise rather than information.
     * listxattr only advertises the attribute for a NON-EMPTY comment, so
     * "no comment" and "empty comment" stay distinguishable. */
    size_t vlen = strlen(cmt);
    if (size == 0) { fuse_reply_xattr(req, vlen); return; }
    if (size < vlen) { fuse_reply_err(req, ERANGE); return; }
    fuse_reply_buf(req, cmt, vlen);
}

static void afs_ll_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
    int has = (ino != INO_ROOT && !IS_SYNTH(ino)) && ino_has_comment(ino);
    size_t len = 0;
    if (has) len = strlen(AFS_XATTR_COMMENT) + 1;   /* NUL-terminated list */
    if (size == 0) { fuse_reply_xattr(req, len); return; }
    if (size < len) { fuse_reply_err(req, ERANGE); return; }
    if (!has) { fuse_reply_buf(req, NULL, 0); return; }
    fuse_reply_buf(req, AFS_XATTR_COMMENT, len);
}

static void afs_ll_statfs(fuse_req_t req, fuse_ino_t ino) {
    (void)ino;
    struct statvfs sv;
    memset(&sv, 0, sizeof sv);
    sv.f_bsize = 512;
    sv.f_frsize = 512;
    fsblkcnt_t total = 0;
    for (unsigned i = 0; i < g_nparts; i++)
        total += (fsblkcnt_t)afs_volume_block_count(g_parts[i].vol);
    sv.f_blocks = total;
    sv.f_namemax = 30;
    fuse_reply_statfs(req, &sv);
}

static void afs_ll_init(void *userdata, struct fuse_conn_info *conn) {
    (void)userdata;
    conn->want &= (unsigned)~FUSE_CAP_WRITEBACK_CACHE;
}

static const struct fuse_lowlevel_ops afs_ll_ops = {
    .lookup   = afs_ll_lookup,
    .getattr  = afs_ll_getattr,
    .opendir  = afs_ll_opendir,
    .readdir  = afs_ll_readdir,
    .open     = afs_ll_open,
    .read     = afs_ll_read,
    .create   = afs_ll_create,
    .write    = afs_ll_write,
    .setattr  = afs_ll_setattr,
    .mkdir    = afs_ll_mkdir,
    .unlink   = afs_ll_unlink,
    .link     = afs_ll_link,
    .rmdir    = afs_ll_rmdir,
    .rename   = afs_ll_rename,
    .flush    = afs_ll_flush,
    .release  = afs_ll_release,
    .readlink = afs_ll_readlink,
    .getxattr = afs_ll_getxattr,
    .listxattr= afs_ll_listxattr,
    .statfs   = afs_ll_statfs,
    .init     = afs_ll_init,
};

enum {
    KEY_AMIGA_IDS,
    KEY_TZ_LOCAL,
};

static const struct fuse_opt opts[] = {
    AMIGAFS_OPT("amiga_ids", amiga_ids),
    AMIGAFS_OPT("tz=local", local_tz),
    AMIGAFS_OPT("rw", rw),
    AMIGAFS_OPT("-f", foreground),
    FUSE_OPT_END
};

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s <image> <mountpoint> [options]\n"
        "  -o amiga_ids      expose stored Amiga uid/gid\n"
        "  -o tz=local       interpret dates as local time (default UTC)\n"
        "  -- -f             foreground\n"
        "  -s                single-threaded (default)\n"
        "\n"
        "  An RDB-partitioned image mounts its partitions as directories\n"
        "  named after their RDB DriveName; a floppy mounts its root directly.\n"
        "  Every volume's root contains .amigafs/bitmap (1 = free block).\n",
        prog);
}

static void close_all(void) {
    for (unsigned i = 0; i < g_nparts; i++) {
        afs_volume_close(g_parts[i].vol);
        g_parts[i].vol = NULL;
    }
    g_nparts = 0;
}

/* Open the image. An RDB image becomes one part per partition; anything else
 * becomes a single part covering the whole image. A floppy that merely HAPPENS
 * to carry an RDB-looking block 0 still mounts as a single volume unless the
 * RDB really parses and names at least one partition. */
static int open_image(const char *image, char *err, size_t errlen) {
    afs_rdb_t rdb;
    uint8_t *img = NULL;
    size_t size = 0;

    FILE *f = fopen(image, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long s = ftell(f);
            if (s > 0 && fseek(f, 0, SEEK_SET) == 0) {
                img = malloc((size_t)s);
                if (img && fread(img, 1, (size_t)s, f) == (size_t)s)
                    size = (size_t)s;
                else { free(img); img = NULL; }
            }
        }
        fclose(f);
    }

    if (img && size >= 512 && memcmp(img, "RDSK", 4) == 0 &&
        afs_rdb_parse(img, size, 512, &rdb) == 0 && rdb.partition_count > 0) {
        unsigned opened = 0;
        for (unsigned i = 0; i < rdb.partition_count && i < AFS_MAX_PARTS; i++) {
            const afs_rdb_partition_t *p = &rdb.part[i];
            afs_volume_t *v = NULL;
            char e[256] = {0};
            /* a partition whose length the RDB does not state runs to the end */
            /* `rw` picks the writable backend, which goes to the image file
             * with pwrite instead of to the in-memory copy read above (that
             * copy exists only to sniff the RDB and is freed here). */
            int rc = g_opt.rw
                ? afs_volume_open_window_rw(image, 512, p->start_block,
                                            p->size_blocks, &v, e, sizeof e)
                : afs_volume_open_window(image, 512, p->start_block,
                                         p->size_blocks, &v, e, sizeof e);
            if (rc != 0) {
                snprintf(err, errlen, "partition '%s': %s", p->name, e);
                free(img);
                close_all();
                return -1;
            }
            g_parts[opened].vol = v;
            g_parts[opened].start = p->start_block;
            g_parts[opened].size = p->size_blocks ? p->size_blocks
                                                  : afs_volume_block_count(v);
            snprintf(g_parts[opened].name, sizeof g_parts[opened].name,
                     "%s", p->name);
            opened++;
        }
        free(img);
        g_nparts = opened;
        g_rdb_mode = 1;
        if (g_nparts == 0) { snprintf(err, errlen, "no mountable partition"); return -1; }
        fprintf(stderr, "amigafs: %u partition(s) from the RDB\n", g_nparts);
        return 0;
    }
    free(img);

    afs_volume_t *v = NULL;
    int orc = g_opt.rw
        ? afs_volume_open_window_rw(image, 512, 0, 0, &v, err, errlen)
        : afs_volume_open_file(image, 512, 0, &v, err, errlen);
    if (orc != 0) return -1;
    g_parts[0].vol = v;
    g_parts[0].start = 0;
    g_parts[0].size = afs_volume_block_count(v);
    snprintf(g_parts[0].name, sizeof g_parts[0].name, "%s", afs_volume_name(v));
    g_nparts = 1;
    g_rdb_mode = 0;
    return 0;
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
    if (open_image(image, err, sizeof err) != 0) {
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
        close_all();
        fuse_opt_free_args(&fargs);
        fuse_opt_free_args(&opts_args);
        return 1;
    }
    if (fuse_set_signal_handlers(se) != 0) {
        fuse_session_destroy(se);
        close_all();
        return 1;
    }
    if (fuse_session_mount(se, mountpoint) != 0) {
        fprintf(stderr, "amigafs: cannot mount on %s\n", mountpoint);
        fuse_remove_signal_handlers(se);
        fuse_session_destroy(se);
        close_all();
        return 1;
    }
    /* fuse_daemonize(fg) DAEMONIZES WHEN fg IS ZERO. Passing 0 while the
     * comment claimed the opposite forked the process, made the caller's PID
     * exit immediately so `kill $!` never stopped the filesystem, and
     * redirected stdio to /dev/null so every diagnostic after this point --
     * including a sanitizer report -- was discarded. A detached child kept
     * serving the mount, so `ls` still worked and nothing noticed. */
    fuse_daemonize(1);   /* 1 == stay in the foreground; -f really means -f */
    int ret = fuse_session_loop(se);
    fuse_session_unmount(se);
    fuse_remove_signal_handlers(se);
    fuse_session_destroy(se);
    close_all();
    fuse_opt_free_args(&fargs);
    fuse_opt_free_args(&opts_args);
    return ret ? 1 : 0;
}
