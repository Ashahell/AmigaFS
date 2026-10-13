/* M5 gate: resolve links on real fixtures.
 *
 * For each entry that is a hard link or soft link, print:
 *   <path>\t<kind>\t<resolved-or-target>
 * where kind is hardfile/harddir/soft. Hard links resolve to their real entry
 * block; soft links print the translated POSIX-relative target.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/dir.h"
#include "amigafs/links.h"

static void walk(afs_volume_t *v, uint32_t dirblk, const char *prefix, int depth) {
    if (depth > 4) return;
    afs_dirent_t ents[256];
    size_t n = 0;
    if (afs_dir_list(v, dirblk, ents, 256, &n) != 0) return;

    for (size_t i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", prefix, ents[i].name);
        switch (ents[i].kind) {
        case AFS_BLOCK_DIR:
        case AFS_BLOCK_LINKDIR:
            /* a directory hard link is NOT traversed (loop avoidance, C12) */
            if (ents[i].kind == AFS_BLOCK_DIR)
                walk(v, ents[i].block, path, depth + 1);
            else
                printf("%s\tharddir\t%s\n", path, (const char *)"");
            break;
        case AFS_BLOCK_LINKFILE: {
            uint32_t real = 0;
            if (afs_link_resolve_file(v, dirblk, ents[i].name, strlen(ents[i].name), &real) == 0)
                printf("%s\thardfile\t%u\n", path, real);
            break;
        }
        case AFS_BLOCK_SOFTLINK: {
            char tgt[1024];
            if (afs_softlink_target(v, dirblk, ents[i].name, strlen(ents[i].name),
                                    tgt, sizeof tgt) == 0)
                printf("%s\tsoft\t%s\n", path, tgt);
            break;
        }
        default:
            break;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: m5_links <image>...\n"); return 2; }
    for (int a = 1; a < argc; a++) {
        afs_volume_t *v = NULL; char err[256];
        if (afs_volume_open_file(argv[a], 512, 0, &v, err, sizeof err) != 0) continue;
        walk(v, afs_volume_rootblock(v), "", 0);
        afs_volume_close(v);
    }
    return 0;
}