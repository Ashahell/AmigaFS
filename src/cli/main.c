/* afs -- command-line inspector for classic AmigaDOS volumes.
 *
 * A thin front-end over the libafs core: the format logic lives in the library
 * and is unit-tested there; this only parses arguments and prints. The same
 * core backs the FUSE mount, so the CLI and the mount can never disagree about
 * what a disk contains.
 *
 *   afs info   <image>              volume type, DOS type, rootblock, name
 *   afs ls     <image> [dir] [-R]    list a directory (recursively with -R)
 *   afs cat    <image> <path>       write a file's contents to stdout
 *   afs verify <image> [--strict]   structure check; exit 1 if problems
 *   afs bitmap <image>              free/used block bitmap summary
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "amigafs/bitmap.h"
#include "amigafs/dir.h"
#include "amigafs/file.h"
#include "amigafs/hash.h"
#include "amigafs/links.h"
#include "amigafs/rdb.h"
#include "amigafs/verify.h"
#include "amigafs/volume.h"

static int usage(void) {
    fprintf(stderr,
        "usage: afs <command> <image> [args]\n"
        "  afs info   <image> [partition]\n"
        "  afs ls     <image> [partition/dir] [-R]\n"
        "  afs cat    <image> <partition/path>\n"
        "  afs verify <image> [--strict]\n"
        "  afs bitmap <image> <partition>\n"
        "  afs bitmap-rebuild <image> [--dry-run] [--yes]\n"
        "  afs put    <image> <partition/path> <local-file>\n"
        "  afs mkdir  <image> <partition/dir>\n"
        "  afs rm     <image> <partition/path>\n"
        "  afs link   <image> <partition/path> <newname>\n"
        "  afs symlink <image> <partition/path> <target>\n"
        "  afs mv     <image> <partition/src> <partition/dst>\n"
        "On an RDB-partitioned image the first path component names the\n"
        "partition (as in the FUSE mount); `info` alone lists the partitions.\n"
        "Writes go to the image file itself: work on a copy, never a fixture.\n");
    return 2;
}

static int open_volume(const char *image, afs_volume_t **out) {
    char err[256] = {0};
    if (afs_volume_open_file(image, 512, 0, out, err, sizeof err) != 0) {
        fprintf(stderr, "afs: %s\n", err);
        return 1;
    }
    return 0;
}

/* Writable whole-image open (floppies). RDB images address partitions, never
 * the whole disk, for writes: block addresses are partition-relative, so a
 * whole-disk write path would be nonsense. There is no `open_volume_rw` that
 * takes a bare RDB image. */
static int open_volume_rw(const char *image, afs_volume_t **out) {
    char err[256] = {0};
    if (afs_volume_open_window_rw(image, 512, 0, 0, out, err, sizeof err) != 0) {
        fprintf(stderr, "afs: %s\n", err);
        return 1;
    }
    return 0;
}

/* Slurp a whole file; NULL on any failure. */
static uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *b;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b); fclose(f); return NULL;
    }
    fclose(f);
    *len_out = (size_t)sz;
    return b;
}

/* If `image` carries an RDB, parse it into `rdb` and return 1; return 0 for a
 * plain image, -1 when the file cannot even be read. */
static int probe_rdb(const char *image, afs_rdb_t *rdb) {
    size_t sz = 0;
    uint8_t *img = read_file(image, &sz);
    int rc;
    if (!img) return -1;
    rc = (sz >= 512 && memcmp(img, "RDSK", 4) == 0 &&
          afs_rdb_parse(img, sz, 512, rdb) == 0 &&
          rdb->partition_count > 0) ? 1 : 0;
    free(img);
    return rc;
}

static const afs_rdb_partition_t *find_partition(const afs_rdb_t *rdb,
                                                 const char *name) {
    unsigned i;
    for (i = 0; i < rdb->partition_count; i++)
        if (strcmp(rdb->part[i].name, name) == 0)
            return &rdb->part[i];
    return NULL;
}

/* Open the named partition of an RDB image as its own window. Reads stay
 * read-only; writes go through the _rw spelling so a read path can never gain
 * write permission by a flag flip. */
static int open_partition_rw(const char *image, const afs_rdb_partition_t *p,
                             afs_volume_t **out, int rw) {
    char err[256] = {0};
    int rc = rw ? afs_volume_open_window_rw(image, 512, p->start_block,
                                            p->size_blocks, out,
                                            err, sizeof err)
                : afs_volume_open_window(image, 512, p->start_block,
                                         p->size_blocks, out,
                                         err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "afs: partition '%s': %s\n", p->name, err);
        return 1;
    }
    return 0;
}

static int open_partition(const char *image, const afs_rdb_partition_t *p,
                          afs_volume_t **out) {
    return open_partition_rw(image, p, out, 0);
}

static const char *dostype_name(uint8_t flags) {
    static const char *names[8] = {
        "DOS\\0 OFS", "DOS\\1 FFS", "DOS\\2 OFS/INTL", "DOS\\3 FFS/INTL",
        "DOS\\4 OFS/DIRC&INTL", "DOS\\5 FFS/DIRC&INTL",
        "DOS\\6 (unsupported long-name)", "DOS\\7 (unsupported long-name)"
    };
    return names[flags & 7u];
}

static int cmd_info(const char *image, const char *part) {
    afs_volume_t *v;
    afs_rdb_t rdb;
    int has_rdb = probe_rdb(image, &rdb);
    if (has_rdb < 0) { fprintf(stderr, "afs: cannot open %s\n", image); return 1; }
    if (has_rdb && !part) {
        /* the volume here is the disk: report its partition table, the same
         * shape rdbtool prints, so the two can be diffed exactly. */
        unsigned i;
        printf("image:        %s\n", image);
        printf("partitions:   %u\n", rdb.partition_count);
        for (i = 0; i < rdb.partition_count; i++)
            printf("part %u name=%s dos=0x%08x start=%u size=%u\n", i,
                   rdb.part[i].name, rdb.part[i].dos_type,
                   rdb.part[i].start_block, rdb.part[i].size_blocks);
        return 0;
    }
    if (has_rdb) {
        const afs_rdb_partition_t *p = find_partition(&rdb, part);
        if (!p) {
            fprintf(stderr, "afs: no partition named '%s'\n", part);
            return 1;
        }
        if (open_partition(image, p, &v) != 0) return 1;
    } else {
        if (part) {
            fprintf(stderr, "afs: %s is not partitioned; no partition '%s'\n",
                    image, part);
            return 1;
        }
        if (open_volume(image, &v) != 0) return 1;
    }
    const afs_rootblock_t *rb = afs_volume_root(v);
    printf("image:        %s\n", image);
    printf("volume name:  %s\n", afs_volume_name(v));
    printf("dos type:     %s\n", dostype_name(afs_volume_flags(v)));
    printf("rootblock:    %u\n", afs_volume_rootblock(v));
    printf("blocks:       %u (%zu bytes)\n",
           afs_volume_block_count(v),
           afs_volume_block_count(v) * afs_volume_block_size(v));
    printf("block size:   %zu\n", (size_t)afs_volume_block_size(v));
    printf("ofs/ffs:      %s\n", afs_volume_is_ofs(v) ? "OFS" : "FFS");
    printf("intl:         %s\n", afs_volume_has_intl(v) ? "yes" : "no");
    printf("dircache:     %s\n", afs_volume_has_dircache(v) ? "yes" : "no");
    printf("bitmap:       %s\n",
           (rb && rb->bm_flag == 0xFFFFFFFFu) ? "valid" : "INVALID");
    printf("hash entries: %zu\n", afs_hash_table_size(afs_volume_block_size(v)));
    afs_volume_close(v);
    return 0;
}

static void ls_rec(afs_volume_t *v, uint32_t dirblk, const char *prefix,
                   int depth, int maxdepth, int nlinkmode) {
    afs_dirent_t ents[256];
    size_t n = 0;
    if (afs_dir_list(v, dirblk, ents, 256, &n) != 0) return;
    for (size_t i = 0; i < n; i++) {
        char kind = 'f';
        unsigned nl = 1;
        switch (ents[i].kind) {
        case AFS_BLOCK_DIR:          kind = 'd'; break;
        case AFS_BLOCK_SOFTLINK:     kind = 'l'; break;
        case AFS_BLOCK_LINKFILE:     kind = 'h'; break;
        case AFS_BLOCK_LINKDIR:      kind = 'D'; break;
        default: break;
        }
        if (nlinkmode && (ents[i].kind == AFS_BLOCK_LINKFILE ||
                          ents[i].kind == AFS_BLOCK_FILE)) {
            if (afs_file_nlink(v, dirblk, ents[i].name, strlen(ents[i].name), &nl) == 0)
                kind = (nl > 1) ? 'h' : 'f';
        }
        printf("%c%u %s/%s\n", kind, nl, prefix, ents[i].name);

        /* never descend into a directory hard link (C12: it is a cycle) */
        if (depth < maxdepth && ents[i].kind == AFS_BLOCK_DIR)
            ls_rec(v, ents[i].block, ents[i].name, depth + 1, maxdepth, nlinkmode);
    }
}

static int cmd_ls(const char *image, const char *dir, int recursive, int nlink) {
    afs_volume_t *v;
    afs_rdb_t rdb;
    int has_rdb = probe_rdb(image, &rdb);
    if (has_rdb < 0) { fprintf(stderr, "afs: cannot open %s\n", image); return 1; }
    if (has_rdb) {
        /* Mirror the mount namespace: the first component names the partition.
         * No dir (or "/" / "") lists the partitions, as the mount root does. */
        const char *p = dir ? dir : "";
        char first[256];
        size_t o = 0;
        const afs_rdb_partition_t *part;
        while (*p == '/') p++;
        while (*p && *p != '/' && o < sizeof first - 1) first[o++] = *p++;
        first[o] = '\0';
        if (!o) {
            unsigned i;
            for (i = 0; i < rdb.partition_count; i++)
                printf("d1 %s\n", rdb.part[i].name);
            return 0;
        }
        part = find_partition(&rdb, first);
        if (!part) {
            fprintf(stderr, "afs: no partition named '%s'\n", first);
            return 1;
        }
        if (open_partition(image, part, &v) != 0) return 1;
        while (*p == '/') p++;
        dir = p;
    } else {
        if (open_volume(image, &v) != 0) return 1;
    }
    uint32_t root = afs_volume_rootblock(v);

    uint32_t target = root;
    char lastname[256] = {0};
    if (dir && *dir) {
        /* resolve a leading '/' as the volume root, else walk from root */
        const char *p = dir;
        while (*p == '/') p++;
        target = root;
        if (*p) {
            afs_dirent_t e;
            char comp[256];
            size_t o = 0;
            for (;;) {
                while (*p && *p != '/') { if (o < sizeof comp - 1) comp[o++] = *p; p++; }
                comp[o] = '\0';
                if (!o) break;
                if (afs_dir_lookup(v, target, comp, o, &e) != 0) {
                    fprintf(stderr, "afs: %s: not found\n", dir);
                    afs_volume_close(v);
                    return 1;
                }
                target = e.block;
                /* keep the STORED spelling: lookup is case-insensitive, so
                 * what was typed may not be what is on the volume */
                snprintf(lastname, sizeof lastname, "%s", e.name);
                o = 0;
                while (*p == '/') p++;
                if (!*p) break;
            }
        }
    }
    /* A path that resolves to something that is NOT a directory is listed as
     * its own single entry, the way `ls file` prints the file's name. Listing
     * a file block as though it were a directory produced rc 0 and NO output,
     * which is the worst shape a CLI failure can take: an error would at least
     * stop `afs ls ... && ...`, but silence with a success code lets a script
     * proceed as though it had been handed a listing.
     *
     * The name printed is the STORED spelling, which may differ in case from
     * what the user typed, and matches what the containing directory lists. */
    if (!(dir && *dir)) {
        /* listing the volume root: always a directory */
    } else {
        afs_block_kind_t tk = AFS_BLOCK_ROOT;
        uint8_t *tb = malloc(afs_volume_block_size(v));
        if (tb && afs_volume_read_block(v, target, tb,
                                        afs_volume_block_size(v)) == 0) {
            tk = afs_block_kind(tb, afs_volume_block_size(v));
        }
        free(tb);
        if (tk != AFS_BLOCK_ROOT && tk != AFS_BLOCK_DIR) {
            char kind = 'f';
            switch (tk) {
            case AFS_BLOCK_SOFTLINK: kind = 'l'; break;
            case AFS_BLOCK_LINKFILE: kind = 'h'; break;
            case AFS_BLOCK_LINKDIR:  kind = 'D'; break;
            default: break;
            }
            printf("%c1 /%s\n", kind, lastname);
            afs_volume_close(v);
            return 0;
        }
    }

    ls_rec(v, target, "", 0, recursive ? 64 : 0, nlink);
    afs_volume_close(v);
    return 0;
}

static int cmd_cat(const char *image, const char *path) {
    afs_volume_t *v;
    afs_rdb_t rdb;
    int has_rdb = probe_rdb(image, &rdb);
    if (has_rdb < 0) { fprintf(stderr, "afs: cannot open %s\n", image); return 1; }
    if (has_rdb) {
        char first[256];
        size_t o = 0;
        const afs_rdb_partition_t *part;
        const char *p = path;
        while (*p == '/') p++;
        while (*p && *p != '/' && o < sizeof first - 1) first[o++] = *p++;
        first[o] = '\0';
        if (!o) {
            fprintf(stderr, "afs: cat needs a path inside a partition\n");
            return 1;
        }
        part = find_partition(&rdb, first);
        if (!part) {
            fprintf(stderr, "afs: no partition named '%s'\n", first);
            return 1;
        }
        if (open_partition(image, part, &v) != 0) return 1;
        while (*p == '/') p++;
        path = p;
        if (!*path) {
            fprintf(stderr, "afs: cat needs a path inside a partition\n");
            afs_volume_close(v);
            return 1;
        }
    } else {
        if (open_volume(image, &v) != 0) return 1;
    }
    uint32_t root = afs_volume_rootblock(v);

    /* split into directory + basename */
    char dir[512];
    const char *base = path;
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t dl = (size_t)(slash - path);
        if (dl >= sizeof dir) dl = sizeof dir - 1;
        memcpy(dir, path, dl);
        dir[dl] = '\0';
        base = slash + 1;
    } else {
        dir[0] = '\0';
    }

    uint32_t dirblk = root;
    if (dir[0]) {
        const char *p = dir;
        while (*p == '/') p++;
        while (*p) {
            char comp[256];
            size_t o = 0;
            while (*p && *p != '/') { if (o < sizeof comp - 1) comp[o++] = *p; p++; }
            comp[o] = '\0';
            while (*p == '/') p++;
            if (!o) continue;
            afs_dirent_t e;
            if (afs_dir_lookup(v, dirblk, comp, o, &e) != 0) {
                fprintf(stderr, "afs: %s: not found\n", dir);
                afs_volume_close(v);
                return 1;
            }
            dirblk = e.block;
        }
    }

    void *buf = NULL;
    size_t len = 0;
    if (afs_file_read_all(v, dirblk, base, strlen(base), &buf, &len) != 0) {
        fprintf(stderr, "afs: cannot read %s\n", path);
        afs_volume_close(v);
        return 1;
    }
    if (len) fwrite(buf, 1, len, stdout);
    free(buf);
    afs_volume_close(v);
    return 0;
}

/* Split `path` into parent chain + basename for creation commands. The parent
 * must already exist (plain mkdir semantics, no -p): each component resolves
 * by lookup, and a missing one is a named refusal, not an implicit mkdir. */
static int resolve_parent(afs_volume_t *v, uint32_t root, const char *path,
                          uint32_t *dirblk_out, const char **base_out,
                          char *dirbuf, size_t dirbuf_cap) {
    const char *base = path, *slash = strrchr(path, '/');
    size_t dl;
    if (slash) {
        dl = (size_t)(slash - path);
        if (dl >= dirbuf_cap) dl = dirbuf_cap - 1;
        memcpy(dirbuf, path, dl);
        dirbuf[dl] = '\0';
        base = slash + 1;
    } else {
        dirbuf[0] = '\0';
    }
    if (!*base) return -1;
    *dirblk_out = root;
    *base_out = base;
    if (dirbuf[0]) {
        const char *p = dirbuf;
        while (*p == '/') p++;
        while (*p) {
            char comp[256];
            size_t o = 0;
            afs_dirent_t e;
            while (*p && *p != '/') {
                if (o < sizeof comp - 1) comp[o++] = *p;
                p++;
            }
            comp[o] = '\0';
            while (*p == '/') p++;
            if (!o) continue;
            if (afs_dir_lookup(v, *dirblk_out, comp, o, &e) != 0) return -1;
            if (e.kind != AFS_BLOCK_DIR) return -1;
            *dirblk_out = e.block;
        }
    }
    return 0;
}

/* Open `image` writable and resolve `path` to (volume, parent dirblock,
 * basename), applying the M12 partition rule: on an RDB image the first
 * component names the partition. All failures are named. */
static int open_parent_rw(const char *image, const char *path,
                          afs_volume_t **vol_out, uint32_t *dirblk_out,
                          const char **base_out, char *dirbuf,
                          size_t dirbuf_cap) {
    afs_rdb_t rdb;
    int has_rdb = probe_rdb(image, &rdb);
    if (has_rdb < 0) { fprintf(stderr, "afs: cannot open %s\n", image); return 1; }
    if (has_rdb) {
        char first[256];
        size_t o = 0;
        const afs_rdb_partition_t *part;
        const char *p = path;
        while (*p == '/') p++;
        while (*p && *p != '/' && o < sizeof first - 1) first[o++] = *p++;
        first[o] = '\0';
        if (!o) {
            fprintf(stderr, "afs: need a path inside a partition\n");
            return 1;
        }
        part = find_partition(&rdb, first);
        if (!part) {
            fprintf(stderr, "afs: no partition named '%s'\n", first);
            return 1;
        }
        if (open_partition_rw(image, part, vol_out, 1) != 0) return 1;
        while (*p == '/') p++;
        path = p;
        if (!*path) {
            fprintf(stderr, "afs: need a path inside a partition\n");
            afs_volume_close(*vol_out);
            return 1;
        }
    } else {
        if (open_volume_rw(image, vol_out) != 0) return 1;
    }
    if (resolve_parent(*vol_out, afs_volume_rootblock(*vol_out), path,
                       dirblk_out, base_out, dirbuf, dirbuf_cap) != 0) {
        fprintf(stderr, "afs: %s: no such directory\n", path);
        afs_volume_close(*vol_out);
        return 1;
    }
    return 0;
}

/* Rebuild the allocation map by walking the tree (M18).
 *
 * This is the operation AmigaDOS means by bm_Flags = "TRUE if bitmap valid":
 * a full unconditional rescan. It FREES every block the walk does not mark, so
 * it is offered as an explicit operator action and never run automatically on
 * open -- the allocator's refusal of a suspect map stays exactly as it was.
 * `--dry-run` reports what would be reclaimed and writes nothing. */
static int cmd_bitmap_rebuild(const char *image, const char *part,
                              int dry_run, int assume_yes) {
    afs_volume_t *v = NULL;
    afs_bitmap_rebuild_stats_t st;
    char err[256] = {0};

    if (!dry_run && !assume_yes) {
        fprintf(stderr, "afs: rebuilding the allocation map of %s FREES every\n", image);
        fprintf(stderr, "afs: block the directory tree does not reference.\n");
        fprintf(stderr, "afs: re-run with --yes to proceed, or work on a copy.\n");
        return 2;
    }

    if (dry_run) {
        /* The dry run must be trustworthy, and "trust me, the flag was set" is
         * not trustworthy. Rather than add a second code path through the
         * library -- where the dry-run branch is exactly the branch never
         * exercised on a real write -- the dry run rebuilds a COPY and reports
         * what that copy produced. The original is never opened for writing,
         * so a bug in the rebuild cannot reach it. */
        char tmp[] = "/tmp/amigafs-rebuild-XXXXXX";
        int fd = mkstemp(tmp);
        FILE *in, *outf;
        char buf[65536];
        size_t n;
        if (fd < 0) { fprintf(stderr, "afs: cannot create a scratch copy\n"); return 1; }
        close(fd);
        in = fopen(image, "rb");
        outf = fopen(tmp, "wb");
        if (!in || !outf) {
            fprintf(stderr, "afs: cannot copy %s\n", image);
            if (in) fclose(in);
            if (outf) fclose(outf);
            remove(tmp);
            return 1;
        }
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, n, outf) != n) { fclose(in); fclose(outf); remove(tmp);
                                                 fprintf(stderr, "afs: short write on the scratch copy\n"); return 1; }
        fclose(in);
        fclose(outf);

        if (open_volume_rw(tmp, &v) != 0) { remove(tmp); return 1; }
        if (afs_bitmap_rebuild(v, &st, err, sizeof err) != 0) {
            fprintf(stderr, "afs: %s\n", err);
            remove(tmp);
            return 1;
        }
        remove(tmp);
        printf("dry run: %u blocks, %u used, %u free, %u would be reclaimed\n",
               st.total, st.used, st.free_blocks, st.reclaimed);
        printf("%s was not modified\n", image);
        return 0;
    }

    (void)part;
    if (open_volume_rw(image, &v) != 0) return 1;
    if (afs_bitmap_rebuild(v, &st, err, sizeof err) != 0) {
        fprintf(stderr, "afs: %s\n", err);
        return 1;
    }
    printf("%s: %u blocks, %u used, %u free, %u reclaimed\n",
           image, st.total, st.used, st.free_blocks, st.reclaimed);
    return 0;
}

static int cmd_put(const char *image, const char *path, const char *local) {
    afs_volume_t *v = NULL;
    uint32_t dirblk = 0, hdr = 0;
    const char *base = NULL;
    char dirbuf[512], err[256] = {0};
    uint8_t *data = NULL;
    size_t len = 0;

    if (!local) { fprintf(stderr, "afs: put needs a local file\n"); return 2; }
    /* stat first: read_file returns NULL both for a missing file and for a
     * 0-byte file, and those must not be confused (a missing input must never
     * silently become an empty Amiga file). */
    {
        FILE *f = fopen(local, "rb");
        long sz = -1;
        if (!f) { fprintf(stderr, "afs: cannot open %s\n", local); return 1; }
        if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
        fclose(f);
        if (sz < 0) { fprintf(stderr, "afs: cannot read %s\n", local); return 1; }
        if (sz == 0) {
            len = 0;
            data = NULL;
        } else {
            data = read_file(local, &len);
            if (!data) { fprintf(stderr, "afs: cannot read %s\n", local); return 1; }
        }
    }
    if (open_parent_rw(image, path, &v, &dirblk, &base, dirbuf, sizeof dirbuf) != 0) {
        free(data);
        return 1;
    }
    /* fopen "w" semantics: create, else truncate the live file. A directory
     * (or anything not a plain file) at that name keeps create's refusal. */
    if (afs_file_create(v, dirblk, base, strlen(base), &hdr, err, sizeof err) != 0) {
        afs_dirent_t e;
        char cerr[256];
        strncpy(cerr, err, sizeof cerr - 1);
        cerr[sizeof cerr - 1] = '\0';
        if (afs_dir_lookup(v, dirblk, base, strlen(base), &e) != 0 ||
            e.kind != AFS_BLOCK_FILE) {
            fprintf(stderr, "afs: cannot create %s: %s\n", path, cerr);
            afs_volume_close(v);
            free(data);
            return 1;
        }
        hdr = e.block;
    }
    if (afs_file_write_all(v, hdr, data, len, err, sizeof err) != 0) {
        fprintf(stderr, "afs: cannot write %s: %s\n", path, err);
        afs_volume_close(v);
        free(data);
        return 1;
    }
    printf("%s: %zu bytes\n", path, len);
    afs_volume_close(v);
    free(data);
    return 0;
}

static int cmd_mkdir(const char *image, const char *path) {
    afs_volume_t *v = NULL;
    uint32_t dirblk = 0, nb = 0;
    const char *base = NULL;
    char dirbuf[512], err[256] = {0};

    if (open_parent_rw(image, path, &v, &dirblk, &base, dirbuf, sizeof dirbuf) != 0)
        return 1;
    if (afs_dir_mkdir(v, dirblk, base, strlen(base), &nb, err, sizeof err) != 0) {
        fprintf(stderr, "afs: cannot mkdir %s: %s\n", path, err);
        afs_volume_close(v);
        return 1;
    }
    afs_volume_close(v);
    return 0;
}

/* afs symlink <image> <link-path> <target> -- a SOFT link at link-path.
 *
 * Unlike `afs link`, the target is NOT resolved and need not exist: a soft link
 * is a stored string, so pointing at something that has not been created yet is
 * legal and is exactly what a symlink is for. That is also why the two commands
 * have different shapes -- `link` takes the existing path plus a bare name,
 * `symlink` takes the path to create plus a free-form target. */
static int cmd_symlink(const char *image, const char *path, const char *target) {
    afs_volume_t *v = NULL;
    uint32_t dirblk = 0;
    const char *base = NULL;
    char dirbuf[512], err[256] = {0};
    uint32_t link = 0;

    if (!target || !*target) {
        fprintf(stderr, "afs: symlink needs a non-empty target\n");
        return 2;
    }
    if (open_parent_rw(image, path, &v, &dirblk, &base, dirbuf, sizeof dirbuf) != 0)
        return 1;
    if (afs_symlink(v, dirblk, base, strlen(base), target, strlen(target),
                    &link, err, sizeof err) != 0) {
        fprintf(stderr, "afs: cannot symlink %s: %s\n", path, err);
        afs_volume_close(v);
        return 1;
    }
    /* Print what a reader will actually see, not what was stored -- the two
     * differ for an absolute or "Vol:"-prefixed target, and only the translated
     * form is what the mount resolves. */
    char resolved[1024];
    if (afs_softlink_target(v, dirblk, base, strlen(base),
                            resolved, sizeof resolved) != 0)
        snprintf(resolved, sizeof resolved, "%s", target);
    printf("%s -> %s (block %u)\n", base, resolved, link);
    afs_volume_close(v);
    return 0;
}

/* afs link <image> <target-path> <newname> -- a HARD link beside the target. */
static int cmd_link(const char *image, const char *path, const char *newname) {
    afs_volume_t *v = NULL;
    uint32_t dirblk = 0;
    const char *base = NULL;
    char dirbuf[512], err[256] = {0};
    afs_dirent_t e;

    if (!newname || !*newname) {
        fprintf(stderr, "afs: link needs a new name\n");
        return 2;
    }
    /* The new name goes in the SAME directory as the target. Unlike mv, a link
     * cannot move the entry, so there is no second path to resolve -- and that
     * is also what keeps both names on one volume. */
    if (open_parent_rw(image, path, &v, &dirblk, &base, dirbuf, sizeof dirbuf) != 0)
        return 1;
    if (afs_dir_lookup(v, dirblk, base, strlen(base), &e) != 0) {
        fprintf(stderr, "afs: %s: not found\n", path);
        afs_volume_close(v);
        return 1;
    }
    uint32_t link = 0;
    if (afs_file_link(v, dirblk, newname, strlen(newname), e.block,
                      &link, err, sizeof err) != 0) {
        fprintf(stderr, "afs: cannot link %s: %s\n", path, err);
        afs_volume_close(v);
        return 1;
    }
    unsigned nl = 0;
    (void)afs_file_nlink(v, dirblk, base, strlen(base), &nl);
    printf("%s -> %s (%u name%s)\n", newname, base, nl, nl == 1 ? "" : "s");
    afs_volume_close(v);
    return 0;
}

static int cmd_rm(const char *image, const char *path) {
    afs_volume_t *v = NULL;
    uint32_t dirblk = 0;
    const char *base = NULL;
    char dirbuf[512], err[256] = {0};
    afs_dirent_t e;

    if (open_parent_rw(image, path, &v, &dirblk, &base, dirbuf, sizeof dirbuf) != 0)
        return 1;
    if (afs_dir_lookup(v, dirblk, base, strlen(base), &e) != 0) {
        fprintf(stderr, "afs: %s: not found\n", path);
        afs_volume_close(v);
        return 1;
    }
    if (e.kind == AFS_BLOCK_FILE) {
        if (afs_file_delete(v, dirblk, base, strlen(base), err, sizeof err) != 0) {
            fprintf(stderr, "afs: cannot rm %s: %s\n", path, err);
            afs_volume_close(v);
            return 1;
        }
    } else if (e.kind == AFS_BLOCK_DIR) {
        if (afs_dir_rmdir(v, dirblk, base, strlen(base), err, sizeof err) != 0) {
            fprintf(stderr, "afs: cannot rm %s: %s\n", path, err);
            afs_volume_close(v);
            return 1;
        }
    } else if (e.kind == AFS_BLOCK_LINKFILE || e.kind == AFS_BLOCK_SOFTLINK) {
        /* A link is only a name. Removing it frees its own header block and
         * leaves the target and its data untouched -- so the CLI must be able to
         * undo what `afs link` created (M24), and M31's `afs symlink` too.
         *
         * This used to call afs_dir_unlink directly, which splices the NAME out
         * and nothing else, under a comment claiming the block was freed. It was
         * not: measured, unlinking a hard link left the free-block count at 1412
         * where the comment said 1413, and no gate could see it. afs_link_remove
         * is the complete operation (name, dir-cache record, block) so that no
         * shell can do half of it. */
        if (afs_link_remove(v, dirblk, base, strlen(base),
                            err, sizeof err) != 0) {
            fprintf(stderr, "afs: cannot rm %s: %s\n", path, err);
            afs_volume_close(v);
            return 1;
        }
    } else {
        fprintf(stderr, "afs: cannot rm %s: %s\n", path,
                "unsupported entry type");
        afs_volume_close(v);
        return 1;
    }
    afs_volume_close(v);
    return 0;
}

/* First path component: the partition selector on RDB images. */
static void first_component(const char *path, char *out, size_t cap) {
    size_t o = 0;
    if (cap == 0) return;
    while (*path == '/') path++;
    while (*path && *path != '/' && o + 1 < cap) out[o++] = *path++;
    out[o] = '\0';
}

static int cmd_mv(const char *image, const char *src, const char *dst) {
    afs_volume_t *vs = NULL, *vd = NULL;
    uint32_t sdir = 0, ddir = 0;
    const char *sbase = NULL, *dbase = NULL;
    char sdirbuf[512], ddirbuf[512], err[256] = {0};
    afs_rdb_t rdb;
    int has_rdb = probe_rdb(image, &rdb);
    if (has_rdb < 0) { fprintf(stderr, "afs: cannot open %s\n", image); return 1; }

    if (has_rdb) {
        /* cross-partition moves do not exist: block addresses are
         * partition-relative, so a move would be copy+delete, not a rename.
         * Refuse with both names rather than half-moving. Selectors are
         * exact-match DriveNames, so equal strings name one volume. */
        char ss[256], ds[256];
        first_component(src, ss, sizeof ss);
        first_component(dst, ds, sizeof ds);
        if (!ss[0] || !ds[0] || strcmp(ss, ds) != 0) {
            fprintf(stderr, "afs: cannot move across partitions ('%s' vs '%s')\n",
                    ss[0] ? ss : "(none)", ds[0] ? ds : "(none)");
            return 1;
        }
    }
    /* Two handles on the same window: partition-relative block numbers from
     * either resolve identically, and each side reuses the tested open path
     * instead of duplicating selector-stripping a third time. */
    if (open_parent_rw(image, src, &vs, &sdir, &sbase, sdirbuf, sizeof sdirbuf) != 0)
        return 1;
    if (open_parent_rw(image, dst, &vd, &ddir, &dbase, ddirbuf, sizeof ddirbuf) != 0) {
        afs_volume_close(vs);
        return 1;
    }
    if (afs_dir_rename(vs, sdir, sbase, strlen(sbase),
                       ddir, dbase, strlen(dbase), err, sizeof err) != 0) {
        fprintf(stderr, "afs: cannot mv %s: %s\n", src, err);
        afs_volume_close(vs);
        afs_volume_close(vd);
        return 1;
    }
    afs_volume_close(vs);
    afs_volume_close(vd);
    return 0;
}

static int cmd_verify(const char *image, int strict) {
    unsigned flags = strict ? AFS_VERIFY_STRICT : 0u;
    size_t sz = 0;
    unsigned char *img = read_file(image, &sz);
    afs_verify_report_t rep[256];
    int n;
    if (!img) { fprintf(stderr, "afs: cannot open %s\n", image); return 1; }

    /* On an RDB image block addresses are partition-relative, so verifying
     * the whole file as one volume is meaningless; verify each partition's
     * own window and name the partition in every report. */
    if (sz >= 512 && memcmp(img, "RDSK", 4) == 0) {
        afs_rdb_t rdb;
        unsigned i;
        int total = 0;
        if (afs_rdb_parse(img, sz, 512, &rdb) != 0 || rdb.partition_count == 0) {
            fprintf(stderr, "afs: %s has an RDB that does not parse\n", image);
            free(img);
            return 1;
        }
        for (i = 0; i < rdb.partition_count; i++) {
            const afs_rdb_partition_t *p = &rdb.part[i];
            size_t off = (size_t)p->start_block * 512;
            size_t len = (size_t)p->size_blocks * 512;
            if (off + len > sz || len == 0) {
                printf("%s [%s]: cannot verify (partition outside image)\n",
                       image, p->name);
                total++;
                continue;
            }
            n = afs_verify_memory_ex(img + off, len, 512, rep, 256, flags);
            if (n == 0) {
                printf("%s [%s]: OK, no structural problems found\n",
                       image, p->name);
            } else {
                int k;
                printf("%s [%s]: %d problem(s)\n", image, p->name, n);
                for (k = 0; k < n; k++)
                    printf("  block %-6u %s\n", rep[k].block, rep[k].detail);
                total += n;
            }
        }
        free(img);
        return total == 0 ? 0 : 1;
    }

    n = afs_verify_memory_ex(img, sz, 512, rep, 256, flags);
    if (n == 0) {
        printf("%s: OK, no structural problems found\n", image);
    } else {
        int i;
        printf("%s: %d problem(s)\n", image, n);
        for (i = 0; i < n; i++)
            printf("  block %-6u %s\n", rep[i].block, rep[i].detail);
    }
    free(img);
    return n == 0 ? 0 : 1;
}

static int cmd_bitmap(const char *image, const char *part) {
    afs_volume_t *v;
    afs_rdb_t rdb;
    int has_rdb = probe_rdb(image, &rdb);
    if (has_rdb < 0) { fprintf(stderr, "afs: cannot open %s\n", image); return 1; }
    if (has_rdb) {
        /* The bitmap is per-volume; on a partitioned disk the bare image
         * names no volume, so a partition is required (named, with the
         * available names listed). */
        const afs_rdb_partition_t *p;
        unsigned i;
        if (!part || !*part) {
            fprintf(stderr, "afs: %s has %u partitions; bitmap needs one:",
                    image, rdb.partition_count);
            for (i = 0; i < rdb.partition_count; i++)
                fprintf(stderr, " '%s'", rdb.part[i].name);
            fprintf(stderr, "\n");
            return 1;
        }
        p = find_partition(&rdb, part);
        if (!p) {
            fprintf(stderr, "afs: no partition named '%s'\n", part);
            return 1;
        }
        if (open_partition(image, p, &v) != 0) return 1;
    } else {
        if (open_volume(image, &v) != 0) return 1;
    }
    uint32_t nblocks = afs_volume_block_count(v);
    size_t bytes = (size_t)((nblocks + 7) / 8) + 8;
    unsigned char *map = calloc(1, bytes);
    if (!map) { afs_volume_close(v); return 1; }

    int read = afs_bitmap_read(v, map, bytes);
    if (read < 0) {
        fprintf(stderr, "afs: cannot read bitmap\n");
        free(map); afs_volume_close(v);
        return 1;
    }
    uint32_t used = 0, freec = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        if (afs_bitmap_test(map, b)) freec++;
        else used++;
    }
    printf("bitmap blocks: %d\n", read);
    printf("total blocks:  %u\n", nblocks);
    printf("used:           %u\n", used);
    printf("free:           %u\n", freec);
    free(map);
    afs_volume_close(v);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *cmd = argv[1];
    const char *image = argv[2];

    if (strcmp(cmd, "info") == 0)        return cmd_info(image, argc > 3 ? argv[3] : NULL);
    if (strcmp(cmd, "verify") == 0) {
        int strict = 0;
        for (int i = 3; i < argc; i++)
            if (strcmp(argv[i], "--strict") == 0) strict = 1;
        return cmd_verify(image, strict);
    }
    if (strcmp(cmd, "bitmap") == 0)      return cmd_bitmap(image, argc > 3 ? argv[3] : NULL);
    if (strcmp(cmd, "bitmap-rebuild") == 0) {
        int dry = 0, yes = 0, i;
        for (i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--dry-run") == 0) dry = 1;
            else if (strcmp(argv[i], "--yes") == 0) yes = 1;
            else { fprintf(stderr, "afs: unknown option '%s'\n", argv[i]); return 2; }
        }
        return cmd_bitmap_rebuild(image, NULL, dry, yes);
    }
    if (strcmp(cmd, "ls") == 0) {
        const char *dir = NULL;
        int recursive = 0, nlink = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-R") == 0) recursive = 1;
            else if (strcmp(argv[i], "-l") == 0) nlink = 1;
            else dir = argv[i];
        }
        return cmd_ls(image, dir, recursive, nlink);
    }
    if (strcmp(cmd, "cat") == 0) {
        if (argc < 4) { fprintf(stderr, "afs: cat needs a path\n"); return 2; }
        return cmd_cat(image, argv[3]);
    }
    if (strcmp(cmd, "put") == 0) {
        if (argc < 5) { fprintf(stderr, "afs: put needs a path and a local file\n"); return 2; }
        return cmd_put(image, argv[3], argv[4]);
    }
    if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 4) { fprintf(stderr, "afs: mkdir needs a path\n"); return 2; }
        return cmd_mkdir(image, argv[3]);
    }
    if (strcmp(cmd, "rm") == 0) {
        if (argc < 4) { fprintf(stderr, "afs: rm needs a path\n"); return 2; }
        return cmd_rm(image, argv[3]);
    }
    if (strcmp(cmd, "link") == 0) {
        if (argc < 5) { fprintf(stderr, "afs: link needs a path and a new name\n"); return 2; }
        return cmd_link(image, argv[3], argv[4]);
    }
    if (strcmp(cmd, "symlink") == 0) {
        if (argc < 5) { fprintf(stderr, "afs: symlink needs a path and a target\n"); return 2; }
        return cmd_symlink(image, argv[3], argv[4]);
    }
    if (strcmp(cmd, "mv") == 0) {
        if (argc < 5) { fprintf(stderr, "afs: mv needs source and destination\n"); return 2; }
        return cmd_mv(image, argv[3], argv[4]);
    }
    return usage();
}