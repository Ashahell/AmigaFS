/* M10 gate: open every partition of an RDB-partitioned image.
 *
 * Inside a partition every AmigaDOS block pointer is PARTITION-RELATIVE, so
 * mounting one means handing the reader a window of exactly that partition's
 * block range. Getting the window's LENGTH wrong is not a rounding error: a
 * window that runs to the end of the image makes the computed rootblock land
 * in the middle of the NEXT partition, and the open fails. On a six-partition
 * disk that leaves exactly one partition mountable -- the last, whose "to the
 * end of the image" happens to be the right length by accident.
 *
 * For each partition this prints one normalized line so the shell gate can
 * diff it against `rdbtool list`:
 *
 *   part <index> name=<DriveName> dos=<hex> start=<blk> size=<blk> root=<blk>
 *
 * and it FAILS if a partition does not open, or if its rootblock is not the
 * one its size implies: root = (Reserved + highKey) / 2 with Reserved = 2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/be.h"
#include "amigafs/rdb.h"
#include "amigafs/volume.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: m10_partition <rdb-image>\n"); return 2; }
    const char *path = argv[1];

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 2; }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 2; }
    uint8_t *img = malloc((size_t)sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) {
        free(img); fclose(f); return 2;
    }
    fclose(f);

    afs_rdb_t rdb;
    if (afs_rdb_parse(img, (size_t)sz, 512, &rdb) != 0) {
        printf("not an RDB image: %s\n", path);
        free(img);
        return 1;
    }
    printf("rdb partitions=%u\n", rdb.partition_count);

    int bad = 0, opened = 0;
    for (unsigned i = 0; i < rdb.partition_count; i++) {
        const afs_rdb_partition_t *p = &rdb.part[i];
        /* Reserved is 2 on every partition of this fixture; honour whatever
         * the DosEnvVec actually said rather than assuming. */
        uint32_t reserved = p->reserved ? p->reserved : 2;
        uint32_t want_root = (reserved + (p->size_blocks ? p->size_blocks - 1 : 0)) / 2;

        afs_volume_t *v = NULL;
        char err[256] = {0};
        if (afs_volume_open_window(path, 512, p->start_block, p->size_blocks,
                                   &v, err, sizeof err) != 0) {
            printf("part %u name=%s dos=0x%08x start=%u size=%u root=OPEN-FAILED(%s)\n",
                   i, p->name, p->dos_type, p->start_block, p->size_blocks, err);
            bad++;
            continue;
        }
        uint32_t got_root = afs_volume_rootblock(v);
        uint32_t got_size = afs_volume_block_count(v);
        printf("part %u name=%s dos=0x%08x start=%u size=%u root=%u volname=%s\n",
               i, p->name, p->dos_type, p->start_block, got_size, got_root,
               afs_volume_name(v));

        if (got_size != p->size_blocks) {
            printf("  FAIL partition %u windowed %u blocks, RDB says %u\n",
                   i, got_size, p->size_blocks);
            bad++;
        }
        if (got_root != want_root) {
            printf("  FAIL partition %u rootblock %u, size %u implies %u\n",
                   i, got_root, p->size_blocks, want_root);
            bad++;
        }
        /* the DOS type the partition advertises must match what the bootblock
         * inside it actually carries */
        uint32_t ondisk = afs_volume_dostype(v);
        uint32_t want = p->dos_type & 0x07u;
        if (ondisk != want) {
            printf("  FAIL partition %u bootblock DOS type 0x%02x, RDB says 0x%02x\n",
                   i, ondisk, want);
            bad++;
        }
        afs_volume_close(v);
        opened++;
    }

    printf("summary opened=%u of %u\n", opened, rdb.partition_count);
    free(img);
    if (bad) { printf("M10 partition gate: FAILED (%d problem(s))\n", bad); return 1; }
    printf("M10 partition gate: OK\n");
    return 0;
}
