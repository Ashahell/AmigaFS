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
#include "amigafs/verify.h"
#include "amigafs/volume.h"

static int usage(void) {
    fprintf(stderr,
        "usage: afs <command> <image> [args]\n"
        "  afs info   <image>\n"
        "  afs ls     <image> [dir] [-R]\n"
        "  afs cat    <image> <path>\n"
        "  afs verify <image>\n"
        "  afs bitmap <image>\n");
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

static const char *dostype_name(uint8_t flags) {
    static const char *names[8] = {
        "DOS\\0 OFS", "DOS\\1 FFS", "DOS\\2 OFS/INTL", "DOS\\3 FFS/INTL",
        "DOS\\4 OFS/DIRC&INTL", "DOS\\5 FFS/DIRC&INTL",
        "DOS\\6 (unsupported long-name)", "DOS\\7 (unsupported long-name)"
    };
    return names[flags & 7u];
}

static int cmd_info(const char *image) {
    afs_volume_t *v;
    if (open_volume(image, &v) != 0) return 1;
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
    if (open_volume(image, &v) != 0) return 1;
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
    if (open_volume(image, &v) != 0) return 1;
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

static int cmd_verify(const char *image) {
    FILE *f = fopen(image, "rb");
    if (!f) { fprintf(stderr, "afs: cannot open %s\n", image); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *img = malloc(sz > 0 ? (size_t)sz : 1);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "afs: cannot read %s\n", image);
        free(img); fclose(f); return 1;
    }
    fclose(f);

    afs_verify_report_t rep[256];
    int n = afs_verify_memory(img, (size_t)sz, 512, rep, 256);
    if (n == 0) {
        printf("%s: OK, no structural problems found\n", image);
    } else {
        printf("%s: %d problem(s)\n", image, n);
        for (int i = 0; i < n; i++)
            printf("  block %-6u %s\n", rep[i].block, rep[i].detail);
    }
    free(img);
    return n == 0 ? 0 : 1;
}

static int cmd_bitmap(const char *image) {
    afs_volume_t *v;
    if (open_volume(image, &v) != 0) return 1;
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

    if (strcmp(cmd, "info") == 0)        return cmd_info(image);
    if (strcmp(cmd, "verify") == 0)      return cmd_verify(image);
    if (strcmp(cmd, "bitmap") == 0)      return cmd_bitmap(image);
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
    return usage();
}