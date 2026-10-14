/* m11_comment -- list every entry comment our library can read.
 *
 * The oracle for this is amitools: tests/oracle/comment_oracle.py drives
 * amitools' own EntryBlock and DirCacheRecord parsers over the raw fixture
 * bytes and prints one line per comment it finds. This tool prints the same
 * shape from OUR reader, and m11_gate.sh diffs the two.
 *
 * Both sides walk the hash table -- the authority for lookup, per decision C4 --
 * so a disagreement means one of us has the comment field in the wrong place,
 * not that one of us is consulting a cache the other is not.
 *
 * Output, one line per entry that HAS a comment:
 *     <image-basename> <block> <kind> <comment>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/be.h"
#include "amigafs/dir.h"
#include "amigafs/file.h"
#include "amigafs/links.h"
#include "amigafs/volume.h"

static const char *basename_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Walk the hash table, collecting distinct entry blocks.
 *
 * Two sets, deliberately: `dirs` is the set of directories ALREADY EXPANDED
 * (so a hostile image cannot make this loop forever), and `blocks` is every
 * entry block to report. Keeping one array for both silently breaks the walk:
 * a subdirectory is added to `blocks` as an entry and then recursed into, and
 * the recursion's own "already expanded?" test matches it and returns before
 * listing anything. That produced a tool which reported nothing at all on a
 * volume that has comments one level down. */
static void collect(afs_volume_t *v, uint32_t dirblk, int depth,
                    uint32_t *dirs, int *ndirs,
                    uint32_t *blocks, int *nblocks, int max) {
    if (depth > 4 || *ndirs >= max) return;
    for (int i = 0; i < *ndirs; i++) if (dirs[i] == dirblk) return;
    dirs[(*ndirs)++] = dirblk;

    afs_dirent_t ents[256];
    size_t n = 0;
    if (afs_dir_list(v, dirblk, ents, 256, &n) != 0) return;
    for (size_t i = 0; i < n && i < 256; i++) {
        int already = 0;
        for (int k = 0; k < *nblocks; k++)
            if (blocks[k] == ents[i].block) { already = 1; break; }
        if (!already && *nblocks < max) blocks[(*nblocks)++] = ents[i].block;
        if (ents[i].kind == AFS_BLOCK_DIR)
            collect(v, ents[i].block, depth + 1, dirs, ndirs,
                    blocks, nblocks, max);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: m11_comment <image>...\n"); return 2; }

    for (int a = 1; a < argc; a++) {
        afs_volume_t *v = NULL;
        char err[256] = {0};
        if (afs_volume_open_file(argv[a], 512, 0, &v, err, sizeof err) != 0)
            continue;                    /* refused by name: no comments to list */

        enum { MAX = 4096 };
        static uint32_t blocks[MAX];
        static uint32_t dirs[MAX];
        int nblocks = 0, ndirs = 0;
        collect(v, afs_volume_rootblock(v), 0, dirs, &ndirs, blocks, &nblocks, MAX);

        size_t bsize = afs_volume_block_size(v);
        uint8_t *blk = malloc(bsize);
        if (!blk) { afs_volume_close(v); continue; }

        /* sorted, so both sides emit the same order regardless of hash order */
        for (int i = 1; i < nblocks; i++) {     /* insertion sort: n is small */
            uint32_t key = blocks[i];
            int j = i - 1;
            while (j >= 0 && blocks[j] > key) { blocks[j + 1] = blocks[j]; j--; }
            blocks[j + 1] = key;
        }

        for (int i = 0; i < nblocks; i++) {
            if (afs_volume_read_block(v, blocks[i], blk, bsize) != 0) continue;
            char cmt[256];
            if (afs_entry_comment(blk, bsize, cmt, sizeof cmt) == 0) continue;
            afs_block_kind_t k = afs_block_kind(blk, bsize);
            printf("%s %u %s %s\n", basename_of(argv[a]), blocks[i],
                   (k == AFS_BLOCK_DIR) ? "dir" : "file", cmt);
        }
        free(blk);
        afs_volume_close(v);
    }
    return 0;
}
