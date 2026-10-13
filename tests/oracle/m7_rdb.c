/* M7 gate: parse an RDB image and print one line per partition.
 *
 * Output: <name>\t<dos_type_hex>\t<start_block>\t<size_blocks>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/rdb.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: m7_rdb <image>\n"); return 2; }
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { fprintf(stderr, "%s: cannot open\n", argv[a]); continue; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *img = malloc(sz > 0 ? (size_t)sz : 1);
        if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) {
            fprintf(stderr, "%s: read failed\n", argv[a]); free(img); fclose(f); continue;
        }
        fclose(f);

        afs_rdb_t rdb;
        if (afs_rdb_parse(img, (size_t)sz, 512, &rdb) != 0) {
            printf("%s\tno-rdb\n", argv[a]);
            free(img);
            continue;
        }
        for (uint32_t i = 0; i < rdb.partition_count; i++) {
            const afs_rdb_partition_t *p = &rdb.part[i];
            printf("%s\t0x%08x\t%u\t%u\t%s\n", p->name, p->dos_type,
                   p->start_block, p->size_blocks,
                   afs_rdb_dostype_supported(p->dos_type) ? "supported" : "UNSUPPORTED");
        }
        free(img);
    }
    return 0;
}