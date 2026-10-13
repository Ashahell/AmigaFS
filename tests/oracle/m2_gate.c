/* M2 gate: open every real fixture through the real volume layer and report
 * its DOS type and rootblock. This is the first point where our variant
 * detection is compared against the actual bytes Commodore wrote.
 *
 * Expected types are the MEASURED values recorded in fixtures.lock:
 *   arccsh/g1a30c/blank/ffdisk0049 = DOS\0
 *   links/test_link_chains         = DOS\1
 *   testofs                        = DOS\2   (OFS/INTL -- ADFlib calls it "???")
 *   testffs/testhd/win32-names     = DOS\5   (FFS/DIRC&INTL)
 *   cache_crash                    = DOS\6   (MUST be refused)
 *
 * Output: dostype=N rootblock=N ofs=0|1 dirc=0|1 name=...  (one line per image)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/volume.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: m2_gate <image>...\n"); return 2; }
    int refused = 0, opened = 0;

    for (int a = 1; a < argc; a++) {
        afs_volume_t *v = NULL;
        char err[256] = {0};
        /* a floppy has 1760 (DD) or 3520 (HD) 512-byte blocks; let volume open
         * figure out geometry from the file size */
        int rc = afs_volume_open_file(argv[a], 512, 0, &v, err, sizeof err);
        if (rc != 0) {
            /* The expectation for cache_crash.adf (DOS\6) is refusal. Print the
             * reason so the gate can match it. */
            printf("file=%s refused=1 reason=%s\n", argv[a], err);
            refused++;
            continue;
        }
        printf("file=%s dostype=%u rootblock=%u ofs=%d dirc=%d name=%s\n",
               argv[a], afs_volume_dostype(v), afs_volume_rootblock(v),
               afs_volume_is_ofs(v), afs_volume_has_dircache(v),
               afs_volume_name(v));
        opened++;
        afs_volume_close(v);
    }
    fprintf(stderr, "opened=%d refused=%d\n", opened, refused);
    return 0;
}