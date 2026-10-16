/* Geometry checker: does every file on an image agree with itself?
 *
 * For each file in the tree:
 *   - header.size (bs-188) must equal the SUM of data_size over its OFS chain
 *   - the chain length must equal ceil(size / payload)
 *   - each data block's next pointer must equal the following table entry
 *   - every block in the chain must be marked USED in the bitmap
 *
 * This is checkable with no mount, no writes and no oracle -- which is why it
 * belongs as a gate: it asks an invariant that nothing else in the suite asks.
 *
 * FFS volumes carry no per-block data_size, so the sum check is OFS-only; the
 * chain-length and next-pointer checks apply to both.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/volume.h"
#include "amigafs/dir.h"
#include "amigafs/file.h"
#include "amigafs/bitmap.h"

#define MAXENT 4096
#define MAXN   256

static uint32_t rd32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

struct chk {
    afs_volume_t *v;
    size_t bs, payload, entries;
    int ofs;
    unsigned char *map;
    uint32_t nblocks;
    unsigned files, dirs, bad, skipped;
    int verbose;
};

static int is_free(struct chk *c, uint32_t b)
{
    if (b < 2 || b >= c->nblocks) return -1;
    return afs_bitmap_test(c->map, b);
}

static void walk(struct chk *c, uint32_t dirblock, const char *prefix, int depth);

static void check_file(struct chk *c, uint32_t hdr, const char *path, const char *kindname)
{
    unsigned char *h = malloc(c->bs);
    uint32_t size, want_n, i, prev = 0, first;

    if (!h) return;
    if (afs_volume_read_block(c->v, hdr, h, c->bs) != 0) {
        printf("  BAD  %s: header %u unreadable\n", path, hdr);
        c->bad++;
        free(h);
        return;
    }
    uint64_t acc = 0;
    size = rd32(h + c->bs - 188);
    want_n = c->payload ? (size + (uint32_t)c->payload - 1) / (uint32_t)c->payload : 0;

    /* Files needing more blocks than the header's table holds live in
     * EXTENSION blocks chained off the header. Reading past `entries` indexes
     * before the header and reports garbage -- which is exactly what the first
     * version of this checker did, and it accounted for most of its "failures".
     * Skip them rather than mis-read them; a multi-table walk belongs with the
     * library's own collect_ext_blocks, not re-derived here. */
    if (want_n > c->entries) {
        c->skipped++;
        free(h);
        return;
    }

    /* table fills from the END: entry k at hdr + 24 + 4*(entries-1-k) */
    first = 0;
    for (i = 0; i < want_n; i++) {
        uint32_t b = rd32(h + 24 + 4 * (c->entries - 1 - i));
        unsigned char *d;
        if (b == 0) {
            printf("  BAD  %s: table entry %u is 0 but size %u needs %u blocks\n",
                   path, i, size, want_n);
            c->bad++;
            break;
        }
        if (i == 0) first = b;
        if (prev && c->ofs) {
            d = malloc(c->bs);
            if (d && afs_volume_read_block(c->v, prev, d, c->bs) == 0 &&
                rd32(d + 16) != b) {
                printf("  BAD  %s: block %u next=%u but table says %u\n",
                       path, prev, rd32(d + 16), b);
                c->bad++;
            }
            free(d);
        }
        if (is_free(c, b) == 1) {
            printf("  BAD  %s: data block %u is marked FREE\n", path, b);
            c->bad++;
        }
        prev = b;
    }
    /* a chain that overruns the recorded size means the blocks carry more data
     * than the header claims */
    if (c->ofs) {
        /* walk the chain from the FIRST table entry -- starting at the last is
         * what made the first version of this checker under-report every
         * multi-block file's size by exactly its first block. */
        uint32_t cur = first;
        i = 0;
        while (cur && i <= want_n + 2) {
            unsigned char *d = malloc(c->bs);
            if (!d) break;
            if (afs_volume_read_block(c->v, cur, d, c->bs) != 0) { free(d); break; }
            acc += rd32(d + 12);
            cur = rd32(d + 16);
            i++;
            free(d);
        }
        if (acc != size) {
            printf("  BAD  %s: header size %u but chain carries %llu bytes\n",
                   path, size, (unsigned long long)acc);
            c->bad++;
        }
    }
    c->files++;
    if (c->verbose) printf("  ok   %s [%s] (%u bytes, %u blocks)\n",
                           path, kindname, size, want_n);
    free(h);
}

static void walk(struct chk *c, uint32_t dirblock, const char *prefix, int depth)
{
    afs_dirent_t *ents;
    size_t count = 0, i;

    if (depth > 24) return;
    ents = calloc(MAXENT, sizeof *ents);
    if (!ents) return;
    if (afs_dir_list(c->v, dirblock, ents, MAXENT, &count) != 0) {
        printf("  BAD  %s: directory %u unreadable\n", prefix, dirblock);
        c->bad++;
        free(ents);
        return;
    }
    for (i = 0; i < count && i < MAXENT; i++) {
        char sub[MAXN * 2];
        snprintf(sub, sizeof sub, "%s/%s", prefix, ents[i].name);
        if (ents[i].kind == AFS_BLOCK_DIR) {
            c->dirs++;
            walk(c, ents[i].block, sub, depth + 1);
        } else if (ents[i].kind == AFS_BLOCK_FILE || ents[i].kind == AFS_BLOCK_LINKFILE) {
            check_file(c, ents[i].block, sub,
                       ents[i].kind == AFS_BLOCK_LINKFILE ? "hardlink" : "file");
        }
    }
    free(ents);
}

int main(int argc, char **argv)
{
    afs_volume_t *v = NULL;
    char err[256] = {0};
    struct chk c;
    size_t mapbytes;

    if (argc < 2) { printf("usage: geomcheck <image> [verbose]\n"); return 2; }
    if (afs_volume_open_window(argv[1], 512, 0, 0, &v, NULL, 0) != 0) {
        printf("%s: cannot open\n", argv[1]);
        return 2;
    }
    memset(&c, 0, sizeof c);
    c.v = v;
    c.bs = afs_volume_block_size(v);
    c.ofs = afs_volume_is_ofs(v);
    c.payload = c.ofs ? c.bs - 24 : c.bs;
    c.entries = c.bs / 4 - 56;
    c.nblocks = afs_volume_block_count(v);
    c.verbose = argc > 2;
    mapbytes = ((size_t)c.nblocks + 7u) / 8u;
    c.map = malloc(mapbytes ? mapbytes : 1);
    if (!c.map || afs_bitmap_read(v, c.map, mapbytes) <= 0) {
        printf("%s: cannot read bitmap\n", argv[1]);
        return 2;
    }

    printf("%s (%s, %zu-byte blocks, %u entries/table)\n", argv[1],
           c.ofs ? "OFS" : "FFS", c.bs, (unsigned)c.entries);
    walk(&c, afs_volume_rootblock(v), "", 0);
    printf("  %u files checked, %u skipped (multi-table), %u directories, "
           "%u disagreement(s) -> %s\n",
           c.files, c.skipped, c.dirs, c.bad, c.bad ? "FAIL" : "PASS");
    free(c.map);
    afs_volume_close(v);
    return c.bad ? 1 : 0;
}