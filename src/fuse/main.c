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
};

static struct options g_opt;

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
    e.attr_timeout = 3600;
    e.entry_timeout = 3600;
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
        e.attr_timeout = 3600;
        e.entry_timeout = 3600;
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
    e.attr_timeout = 3600;
    e.entry_timeout = 3600;

    afs_block_kind_t k = afs_block_kind(blk, bs);
    if (k == AFS_BLOCK_ROOT) {
        synth_dir_stat(e.ino, &e.attr);
    } else {
        afs_fileheader_t h;
        memset(&h, 0, sizeof h);
        if (afs_parse_fileheader(blk, bs, &h) != 0) memset(&h, 0, sizeof h);
        fill_stat(&h, e.ino, kind_to_mode(k), &e.attr);
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
        fuse_reply_attr(req, &st, 3600);
        return;
    }
    if (IS_SYNTH(ino)) {
        unsigned vi = (unsigned)((ino - SYNTH_BASE) / 2u);
        if ((ino - SYNTH_BASE) % 2u == 0) synth_dir_stat(ino, &st);
        else synth_file_stat(ino, vi < g_nparts ? bitmap_size(vi) : 0, &st);
        fuse_reply_attr(req, &st, 3600);
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
        fill_stat(&h, ino, kind_to_mode(k), &st);
        if (k == AFS_BLOCK_SOFTLINK) st.st_size = 0;
    }
    free(blk);
    fuse_reply_attr(req, &st, 3600);
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

static void afs_ll_open(fuse_req_t req, fuse_ino_t ino,
                        struct fuse_file_info *fi) {
    if (IS_SYNTH(ino)) {
        if ((ino - SYNTH_BASE) % 2u == 0) { fuse_reply_err(req, EISDIR); return; }
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
        fuse_reply_err(req, EROFS);
        return;
    }
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
    .readlink = afs_ll_readlink,
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
            if (afs_volume_open_window(image, 512, p->start_block, p->size_blocks,
                                       &v, e, sizeof e) != 0) {
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
    if (afs_volume_open_file(image, 512, 0, &v, err, errlen) != 0) return -1;
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
    fuse_daemonize(0);   /* we always run in the foreground (-f) */
    int ret = fuse_session_loop(se);
    fuse_session_unmount(se);
    fuse_remove_signal_handlers(se);
    fuse_session_destroy(se);
    close_all();
    fuse_opt_free_args(&fargs);
    fuse_opt_free_args(&opts_args);
    return ret ? 1 : 0;
}
