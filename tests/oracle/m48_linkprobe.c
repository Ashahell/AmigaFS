/* probe for m48_link_structure.sh
 *
 * Prints the STRUCTURAL facts about the hard link in an image, and nothing else.
 * Every offset is taken from ADFlib's own block definitions (src/adf_blk.h,
 * struct AdfLinkBlock / struct AdfFileHeaderBlock), which mirror the AmigaDOS
 * FileHeader / FileExtensionBlock layout:
 *
 *     0x004  headerKey    self-pointer: the block's own block number
 *     0x1b0  nameLen
 *     0x1b1  name[...]
 *     0x1d4  realEntry    for a link block, the real file header it points at
 *     0x1d8  nextLink     chain of link blocks sharing that real header
 *     0x1fc  secType      -3 file header, -4 link to file, 4 link to dir, 3 soft
 *
 * NOTE, because this is exactly the kind of thing that gets typed from memory:
 * an FFS file header has NO hardlink-count field. The number of names for a file
 * is not stored on the header at all -- it is implied by the chain of link
 * blocks hanging off nextLink. Asserting "hardlinks == 2 at some offset" would be
 * asserting an invented field, so this probe asserts structure instead.
 *
 * The value of asserting STRUCTURE rather than "does it read back right": these
 * are invariants of the on-disk format, checkable by reading raw blocks, and so
 * they do not depend on this implementation's own reader agreeing with itself.
 * That matters here because m47 established that no pinned oracle can see hard
 * links -- unadf skips them silently and amitools refuses the whole volume.
 *
 * Output, one record per line:
 *   HDR <blk>                     <blk> is a file header (secType -3)
 *   HDRKEYBAD <blk> <key>         headerKey is not the block's own number
 *   LINK <blk> real=<r> next=<n> key=<k> <name>
 *   LINKKEYBAD <blk> <key>
 *   chainlen <n>                  link blocks on the shared real header
 *   NAMEHDR <name> <blk>          <name> resolves to a file header
 *   NAMELINK <name> <blk> secType=<t> real=<r> next=<n> key=<k> hdr=<blk>
 *   MISSING <name>                <name> is not present
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amigafs/volume.h"
#include "amigafs/dir.h"

#define BS 512

static int32_t rd32(const uint8_t *p)
{
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8)  |  (uint32_t)p[3]);
}

static void blkname(const uint8_t *b, char *out, size_t n)
{
    unsigned len = b[0x1b0];
    if (len > 100 || len + 1 > n) { out[0] = 0; return; }
    memcpy(out, b + 0x1b1, len);
    out[len] = 0;
}

/* Locate `want` in the volume root. Returns its entry block, or ~0u. */
static uint32_t find_root(afs_volume_t *v, const char *want, uint8_t *kindslot)
{
    afs_dirent_t ents[256];
    size_t n = 0;
    if (afs_dir_list(v, afs_volume_rootblock(v), ents, 256, &n) != 0) return ~0u;
    for (size_t i = 0; i < n; i++)
        if (strcmp(ents[i].name, want) == 0) {
            if (kindslot) *kindslot = ents[i].kind;
            return ents[i].block;
        }
    return ~0u;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("FAIL usage: probe <img> <realname> [linkname]\n"); return 1; }
    const char *img = argv[1];
    const char *nm_a = argv[2];
    const char *nm_link = (argc > 3) ? argv[3] : NULL;

    char err[256] = {0};
    afs_volume_t *v = NULL;
    if (afs_volume_open_file(img, BS, 0, &v, err, sizeof err) != 0 || !v) {
        printf("FAIL open %s\n", err[0] ? err : "(no detail)");
        return 1;
    }

    uint8_t b[BS];
    uint32_t ba = find_root(v, nm_a, NULL);
    if (ba == ~0u) { printf("MISSING %s\n", nm_a); afs_volume_close(v); return 1; }
    if (afs_volume_read_block(v, ba, b, BS) != 0) { printf("FAIL read hdr\n"); afs_volume_close(v); return 1; }
    if (rd32(b + 0x1fc) != -3) {
        printf("FAIL %s secType=%d (expected -3)\n", nm_a, rd32(b + 0x1fc));
        afs_volume_close(v); return 1;
    }
    printf("HDR %u\n", ba);
    printf("NAMEHDR %s %u\n", nm_a, ba);
    if (rd32(b + 0x004) != (int32_t)ba)
        printf("HDRKEYBAD %u %d\n", ba, rd32(b + 0x004));

    /* walk the link chain off the real header */
    int32_t next = rd32(b + 0x1d8);
    int chainlen = 0;
    for (; next && chainlen < 64; chainlen++) {
        uint8_t lb[BS];
        if (afs_volume_read_block(v, (uint32_t)next, lb, BS) != 0) break;
        int32_t st = rd32(lb + 0x1fc);
        if (st != -4 && st != 4 && st != 3) break;      /* not a link block: chain ends */
        int32_t key = rd32(lb + 0x004);
        if (key != next) { printf("LINKKEYBAD %d %d\n", next, key); break; }
        char nm[128]; blkname(lb, nm, sizeof nm);
        printf("LINK %d real=%d next=%d key=%d %s\n",
               next, rd32(lb + 0x1d4), rd32(lb + 0x1d8), key, nm);
        next = rd32(lb + 0x1d8);
    }
    printf("chainlen %d\n", chainlen);

    if (nm_link) {
        uint32_t bl = find_root(v, nm_link, NULL);
        if (bl == ~0u) {
            printf("MISSING %s\n", nm_link);
        } else if (afs_volume_read_block(v, bl, b, BS) != 0) {
            printf("FAIL read %s\n", nm_link);
        } else {
            char nm[128]; blkname(b, nm, sizeof nm);
            printf("NAMELINK %s %u secType=%d real=%d next=%d key=%d hdr=%d name=%s\n",
                   nm_link, bl, rd32(b + 0x1fc), rd32(b + 0x1d4),
                   rd32(b + 0x1d8), rd32(b + 0x004), ba, nm);
        }
    }

    afs_volume_close(v);
    return 0;
}