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
 *   afs verify <image>              structure check; exit 1 if problems
 *   afs bitmap <image>              free/used block bitmap summary
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        "  afs verify <image>\n"
        "  afs bitmap <image> <partition>\n"
        "  afs put    <image> <partition/path> <local-file>\n"
        "  afs mkdir  <image> <partition/dir>\n"
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
                o = 0;
                while (*p == '/') p++;
                if (!*p) break;
            }
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

static int cmd_verify(const char *image) {
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
            n = afs_verify_memory(img + off, len, 512, rep, 256);
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

    n = afs_verify_memory(img, sz, 512, rep, 256);
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
    if (strcmp(cmd, "verify") == 0)      return cmd_verify(image);
    if (strcmp(cmd, "bitmap") == 0)      return cmd_bitmap(image, argc > 3 ? argv[3] : NULL);
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
    return usage();
}