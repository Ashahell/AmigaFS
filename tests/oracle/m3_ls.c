/* M3 gate: list a directory on real fixtures and print the names, so the shell
 * gate can diff our output against ADFlib's adfls.
 *
 * Recurses a given depth so subdirectories are covered too. Names are emitted
 * as Latin-1 bytes (one per line, relative path) so the diff is byte-exact
 * against the oracle; the UTF-8 conversion belongs at the FUSE boundary.
 *
 * Usage: m3_ls <image> [maxdepth]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/dir.h"

static void list_rec(afs_volume_t *v, uint32_t dirblk, const char *prefix,
                     int depth, int maxdepth) {
    afs_dirent_t ents[256];
    size_t n = 0;
    if (afs_dir_list(v, dirblk, ents, 256, &n) != 0) return;

    for (size_t i = 0; i < n; i++) {
        printf("%s/%s\n", prefix, ents[i].name);
        if (depth < maxdepth && ents[i].kind == AFS_BLOCK_DIR)
            list_rec(v, ents[i].block, ents[i].name, depth + 1, maxdepth);
        else if (depth < maxdepth && ents[i].kind == AFS_BLOCK_LINKDIR)
            list_rec(v, ents[i].block, ents[i].name, depth + 1, maxdepth);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: m3_ls <image> [maxdepth]\n"); return 2; }
    int maxdepth = (argc > 2) ? atoi(argv[2]) : 1;

    afs_volume_t *v = NULL;
    char err[256];
    if (afs_volume_open_file(argv[1], 512, 0, &v, err, sizeof err) != 0) {
        fprintf(stderr, "open failed: %s\n", err);
        return 1;
    }
    list_rec(v, afs_volume_rootblock(v), "", 0, maxdepth);
    afs_volume_close(v);
    return 0;
}