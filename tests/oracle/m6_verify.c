/* M6 gate: run the structure verifier over a real image and print findings.
 *
 * Output (one line per problem, or "clean"):
 *   <kind>\t<block>\t<detail>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/verify.h"

static const char *kind_name(afs_problem_kind_t k) {
    switch (k) {
    case AFS_PROBLEM_CHECKSUM:        return "checksum";
    case AFS_PROBLEM_RANGE:           return "range";
    case AFS_PROBLEM_TYPE:            return "type";
    case AFS_PROBLEM_DOUBLE_USE:      return "double-use";
    case AFS_PROBLEM_BITMAP_MISMATCH: return "bitmap-mismatch";
    case AFS_PROBLEM_DIRCACHE:        return "dircache";
    case AFS_PROBLEM_CHAIN:           return "chain";
    case AFS_PROBLEM_ROOTBLOCK:       return "rootblock";
    default:                          return "?";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: m6_verify <image>...\n"); return 2; }
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { printf("%s\t0\tcannot-open\n", argv[a]); continue; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *img = malloc(sz > 0 ? (size_t)sz : 1);
        if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) {
            printf("%s\t0\tread-failed\n", argv[a]);
            free(img); fclose(f); continue;
        }
        fclose(f);

        afs_verify_report_t rep[256];
        int n = afs_verify_memory(img, (size_t)sz, 512, rep, 256);
        printf("%s\t%d\tproblems\n", argv[a], n);
        for (int i = 0; i < n; i++)
            printf("  %s\t%u\t%s\n", kind_name(rep[i].kind), rep[i].block, rep[i].detail);
        free(img);
    }
    return 0;
}