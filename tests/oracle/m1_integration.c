/* M1 integration: open real Amiga floppy images through the real core stack
 * (blockio -> be -> checksum) and report what we see. This is deliberately
 * NOT a unit test -- it proves the library agrees with bytes Commodore's
 * formatter actually wrote.
 *
 * Output (one line per image):
 *   file=<name> dostype=<n> rootblock=<n> rootvalid=<0|1> bootvalid=<0|1>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/be.h"
#include "amigafs/blockio.h"
#include "amigafs/checksum.h"

#define BSIZE 512u

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: m1_integration <image>...\n");
        return 2;
    }
    int failures = 0;

    for (int a = 1; a < argc; a++) {
        blockdev_t *d = blockdev_open_file(argv[a], BSIZE);
        if (!d) {
            fprintf(stderr, "%s: open failed\n", argv[a]);
            failures++;
            continue;
        }

        uint8_t boot[1024];
        memset(boot, 0, sizeof boot);
        if (blockdev_read_span(d, 0, boot, sizeof boot) != 0) {
            fprintf(stderr, "%s: bootblock read failed\n", argv[a]);
            blockdev_close(d);
            failures++;
            continue;
        }

        uint32_t doslong = be32(boot);
        /* The DOS id is the big-endian ASCII 'D','O','S' -- compare the RAW
         * bytes on disk, not the host-endian integer's byte order. */
        uint8_t flags = boot[3];
        int is_dos = (boot[0] == 'D' && boot[1] == 'O' && boot[2] == 'S');

        /* rootblock by the FAQ/AROS formula for a standard DD/HD floppy:
         * INT((Reserved + highKey)/2). For floppies Reserved=2 and
         * highKey = last block, giving 880 (DD) / 1760 (HD). */
        uint32_t last = blockdev_block_count(d) - 1;
        uint32_t reserved = 2;
        uint32_t rootblock = (reserved + last) / 2;

        int rootvalid = 0;
        uint8_t root[BSIZE];
        if (blockdev_read(d, rootblock, root, BSIZE) == 0) {
            rootvalid = afs_block_valid(root, BSIZE, 20);
        }

        int bootvalid = afs_bootblock_valid(boot, 1024);

        /* sanity: the be32 long must equal ('D'<<24)|('O'<<16)|('S'<<8)|flags */
        uint32_t want_dos = ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                            ((uint32_t)'S' << 8) | flags;
        if (is_dos && doslong != want_dos) {
            fprintf(stderr, "%s: doslong 0x%08x != expected 0x%08x\n",
                    argv[a], doslong, want_dos);
            failures++;
        }

        printf("file=%s dos=%s dostype=%u rootblock=%u rootvalid=%d bootvalid=%d\n",
               argv[a], is_dos ? "yes" : "no", flags, rootblock, rootvalid,
               bootvalid);

        /* a real DOS floppy must have a rootblock that validates; a non-DOS
         * image legitimately does not. */
        if (is_dos && !rootvalid) failures++;

        blockdev_close(d);
    }
    return failures ? 1 : 0;
}