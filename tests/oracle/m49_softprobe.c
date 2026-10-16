/* probe for m49_softlink_structure.sh
 *
 * Prints the structural fields of a named entry, as stable key=value lines, so
 * the gate can compare our written soft link against the volume's OWN soft link
 * field for field.
 *
 * That comparison is the point. m47 established that no pinned oracle can see a
 * soft link (amitools refuses the volume; ADFlib's unadf skips link names), and
 * m31_symlink_write verifies only behaviour -- refusals, an entry count, and what
 * `readlink` returns. So both were, in the end, evidence about our own reader.
 * This probe instead compares against a block written by a real AmigaDOS: every
 * fixture carries `slink_dir1`, and if our writer reproduces IT field for field,
 * nothing in the comparison depends on our reader being right.
 *
 * Offsets are ADFlib's own (src/adf_blk.h) and are the same ones m48 uses.
 * Note there is no hardlink-count field anywhere; a soft link is a header block
 * with secType 3, realEntry 0, byteSize 0, extension 0, and the target stored as
 * NUL-free zero-padded ASCII at +24.
 */
#include <stdio.h>
#include <string.h>
#include "amigafs/volume.h"
#include "amigafs/dir.h"

#define BS 512

static int32_t r32(const uint8_t *p)
{
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) |  (uint32_t)p[3]);
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("FAIL usage: probe <img> <name>\n"); return 1; }
    char err[256] = {0};
    afs_volume_t *v = NULL;
    if (afs_volume_open_file(argv[1], BS, 0, &v, err, sizeof err) != 0 || !v) {
        printf("FAIL open %s\n", err[0] ? err : "(no detail)");
        return 1;
    }
    afs_dirent_t e[256];
    size_t n = 0;
    if (afs_dir_list(v, afs_volume_rootblock(v), e, 256, &n) != 0) {
        printf("FAIL list\n"); afs_volume_close(v); return 1;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(e[i].name, argv[2]) != 0) continue;
        uint8_t b[BS];
        if (afs_volume_read_block(v, e[i].block, b, BS) != 0) {
            printf("FAIL read\n"); afs_volume_close(v); return 1;
        }
        printf("block=%u\n", e[i].block);
        printf("type=%u\n",        (unsigned)r32(b + 0));
        printf("header_key=%d\n",  r32(b + 4));
        printf("high_seq=%d\n",    r32(b + 8));
        printf("ht_size=%d\n",     r32(b + 12));
        printf("data_size=%d\n",   r32(b + 16));
        /* the target: ASCII from +24, stopping at the first NUL or 64 bytes */
        char tgt[65];
        int t = 0;
        while (t < 64 && b[24 + t]) { tgt[t] = (char)b[24 + t]; t++; }
        tgt[t] = 0;
        printf("target=%s\n", tgt);
        printf("byte_size=%d\n",   r32(b + BS - 188));
        printf("comm_len=%d\n",    (int)b[BS - 184]);
        printf("name_len=%u\n",    (unsigned)b[BS - 80]);
        printf("name=%s\n",        (char *)(b + BS - 79));
        printf("real_entry=%d\n",  r32(b + BS - 44));
        printf("next_link=%d\n",   r32(b + BS - 40));
        printf("next_hash=%d\n",   r32(b + BS - 16));
        printf("parent=%d\n",      r32(b + BS - 12));
        printf("extension=%d\n",   r32(b + BS - 8));
        printf("sec_type=%d\n",    r32(b + BS - 4));
        afs_volume_close(v);
        return 0;
    }
    printf("MISSING\n");
    afs_volume_close(v);
    return 0;
}