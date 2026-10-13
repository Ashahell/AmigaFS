/* M6 damage probe: prove the structure verifier can actually SEE damage.
 *
 * m6_verify prints what the verifier found; it returns 0 whatever it found, so
 * on its own it cannot tell "found the two known defects" from "found nothing
 * at all". This tool supplies the missing half -- a NEGATIVE CONTROL.
 *
 * For each entry block the hash table can reach, it damages that block and
 * requires the verifier to report a problem naming that exact block. A
 * verifier that never descends past the rootblock passes every "is the clean
 * disk clean" test while being unable to detect any damage below the root at
 * all; that failure mode is invisible unless something damages a block on
 * purpose and insists it be reported.
 *
 *   usage: m6_damage_probe <image> [image...]
 *   exit 0 = every injected damage was detected; 1 = at least one slipped by
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/be.h"
#include "amigafs/dir.h"
#include "amigafs/verify.h"
#include "amigafs/volume.h"

#define MAXB 8192

static uint32_t blocks[MAXB];
static size_t nblocks;

static void collect(afs_volume_t *v, uint32_t d, int depth) {
    if (depth > 8 || nblocks >= MAXB) return;
    afs_dirent_t e[256];
    size_t n = 0;
    if (afs_dir_list(v, d, e, 256, &n) != 0) return;
    for (size_t i = 0; i < n && i < 256; i++) {
        blocks[nblocks++] = e[i].block;
        if (e[i].kind == AFS_BLOCK_DIR) collect(v, e[i].block, depth + 1);
    }
}

/* Does the report name this block? */
static int names(const afs_verify_report_t *rep, int n, uint32_t blk) {
    for (int i = 0; i < n; i++)
        if (rep[i].block == blk) return 1;
    return 0;
}

static int check(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  %-28s cannot open\n", path); return 1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long s = ftell(f);
    if (s <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 1; }
    size_t sz = (size_t)s;
    unsigned char *img = malloc(sz);
    if (!img || fread(img, 1, sz, f) != sz) { free(img); fclose(f); return 1; }
    fclose(f);

    afs_volume_t *v = NULL;
    char err[256];
    if (afs_volume_open_memory(img, sz, 512, &v, err, sizeof err) != 0) {
        printf("  %-28s not openable: %s\n", path, err);
        free(img);
        return 1;
    }
    nblocks = 0;
    collect(v, afs_volume_rootblock(v), 0);
    afs_volume_close(v);

    afs_verify_report_t rep[256];
    int base = afs_verify_memory(img, sz, 512, rep, 256);

    /* inject damage one reachable block at a time, in three flavours */
    int missed = 0, tested = 0;
    static const struct { size_t off; const char *what; } flavours[3] = {
        { 20, "checksum word" }, { 300, "payload byte" }, { 400, "payload byte 2" }
    };
    for (size_t i = 0; i < nblocks; i++) {
        uint32_t b = blocks[i];
        if ((size_t)b * 512 + 512 > sz) continue;
        for (int fl = 0; fl < 3; fl++) {
            size_t off = (size_t)b * 512 + flavours[fl].off;
            uint8_t save = img[off];
            img[off] ^= 0xFF;
            int n = afs_verify_memory(img, sz, 512, rep, 256);
            tested++;
            if (!names(rep, n, b)) {
                missed++;
                printf("  %-28s MISSED %s in block %u\n",
                       path, flavours[fl].what, b);
            }
            img[off] = save;
        }
    }

    printf("  %-28s %zu reachable blocks, %d baseline problem(s), "
           "%d/%d injected damages detected\n",
           path, nblocks, base, tested - missed, tested);
    free(img);
    return missed ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: m6_damage_probe <image>...\n");
        return 2;
    }
    int bad = 0;
    for (int a = 1; a < argc; a++) bad |= check(argv[a]);
    if (bad)
        printf("M6 damage probe: FAILED -- the verifier is blind below the "
               "rootblock\n");
    else
        printf("M6 damage probe: OK -- every injected damage was detected\n");
    return bad;
}
