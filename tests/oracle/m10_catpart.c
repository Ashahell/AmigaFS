/* m10_catpart -- read a file out of a NAMED partition of an RDB image.
 *
 * The mount gate compares bytes read THROUGH THE MOUNT against bytes read
 * through the core library, because "the mount lists six directories" proves
 * very little: a window off by one block yields a plausible mount full of
 * garbage. This tool is the independent path -- it opens the partition by its
 * RDB DriveName, exactly as the mount does, and resolves a path with the
 * library's own directory walk rather than through FUSE.
 *
 *   usage: m10_catpart <rdb-image> <DriveName> <path-inside-partition>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/dir.h"
#include "amigafs/file.h"
#include "amigafs/rdb.h"
#include "amigafs/volume.h"

static uint8_t *slurp(const char *path, size_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long s = ftell(f);
    if (s <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)s);
    if (!b || fread(b, 1, (size_t)s, f) != (size_t)s) { free(b); fclose(f); return NULL; }
    fclose(f);
    *out = (size_t)s;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: m10_catpart <rdb-image> <DriveName> <path>\n");
        return 2;
    }
    size_t sz = 0;
    uint8_t *img = slurp(argv[1], &sz);
    if (!img) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

    afs_rdb_t rdb;
    if (afs_rdb_parse(img, sz, 512, &rdb) != 0) {
        fprintf(stderr, "not an RDB image\n");
        free(img);
        return 2;
    }
    const afs_rdb_partition_t *want = NULL;
    for (unsigned i = 0; i < rdb.partition_count; i++)
        if (strcmp(rdb.part[i].name, argv[2]) == 0) { want = &rdb.part[i]; break; }
    if (!want) { fprintf(stderr, "no partition named '%s'\n", argv[2]); free(img); return 2; }

    afs_volume_t *v = NULL;
    char err[256] = {0};
    if (afs_volume_open_window(argv[1], 512, want->start_block, want->size_blocks,
                               &v, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        free(img);
        return 2;
    }

    /* split the path into a directory chain and a basename */
    char dir[512];
    const char *base = argv[3];
    const char *slash = strrchr(argv[3], '/');
    if (slash) {
        size_t dl = (size_t)(slash - argv[3]);
        if (dl >= sizeof dir) dl = sizeof dir - 1;
        memcpy(dir, argv[3], dl);
        dir[dl] = '\0';
        base = slash + 1;
    } else {
        dir[0] = '\0';
    }

    uint32_t dirblk = afs_volume_rootblock(v);
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
            fprintf(stderr, "%s: not found\n", comp);
            afs_volume_close(v);
            free(img);
            return 1;
        }
        dirblk = e.block;
    }

    void *buf = NULL;
    size_t len = 0;
    if (afs_file_read_all(v, dirblk, base, strlen(base), &buf, &len) != 0) {
        fprintf(stderr, "cannot read %s\n", base);
        afs_volume_close(v);
        free(img);
        return 1;
    }
    if (len) fwrite(buf, 1, len, stdout);
    free(buf);
    afs_volume_close(v);
    free(img);
    return 0;
}
