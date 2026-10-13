#include "amigafs/rdb.h"
#include "amigafs/be.h"

#include <string.h>

/* Supported DOS types: the 'DOS' ASCII prefix (0x444F5300) with flag 0..5.
 * 6/7 are AmigaOS 3.2 long-filename types (out of scope). */
#define DOS_PREFIX 0x444F5300u

int afs_rdb_dostype_supported(uint32_t dos_type) {
    if ((dos_type & 0xFFFFFF00u) != DOS_PREFIX) return 0;
    return (dos_type & 0x07u) <= 5u;
}

static uint32_t rd_long(const uint8_t *img, size_t size, size_t bsize,
                        uint32_t blk, uint32_t longidx) {
    uint64_t off = (uint64_t)blk * bsize + 4ull * longidx;
    if (off + 4 > (uint64_t)size) return 0;
    return be32(img + off);
}

int afs_rdb_parse(const uint8_t *img, size_t size, size_t bsize, afs_rdb_t *rdb) {
    if (!img || !rdb || bsize == 0 || size < bsize) return -1;
    if (memcmp(img, "RDSK", 4) != 0) return -1;      /* no RDB */

    uint32_t image_blocks = (uint32_t)(size / bsize);
    memset(rdb, 0, sizeof *rdb);

    uint32_t n = 0;
    /* The PART blocks are found by SCANNING the RDB block region, NOT by
     * following RDSK.next: on a real image (a590_WD93028XA_6parts.hdd) the
     * RDSK.next field is 512 while the PART blocks live at blocks 1..N. Each
     * PART's own `next` field then links the partition chain, but the entry
     * point into it is by position. We scan the first 512 blocks, collecting
     * every PART block in order. */
    uint32_t scan_limit = image_blocks < 512 ? image_blocks : 512;
    for (uint32_t cur = 1; cur < scan_limit &&
                            n < AFS_MAX_RDB_PARTITIONS; cur++) {
        /* bound the read: the block must lie wholly within the image */
        uint64_t boff = (uint64_t)cur * bsize;
        if (boff + bsize > (uint64_t)size) break;
        const uint8_t *pblk = img + boff;
        if (memcmp(pblk, "PART", 4) != 0) continue;

        afs_rdb_partition_t *p = &rdb->part[n];
        p->surfaces        = rd_long(img, size, bsize, cur, 35);
        p->sectors_per_track = rd_long(img, size, bsize, cur, 37);
        p->reserved        = rd_long(img, size, bsize, cur, 38);
        p->low_cyl         = rd_long(img, size, bsize, cur, 41);
        p->high_cyl        = rd_long(img, size, bsize, cur, 42);
        p->dos_type        = rd_long(img, size, bsize, cur, 48);
        bstr(pblk + 36, 32, p->name, sizeof p->name);
        n++;
    }
    rdb->partition_count = n;

    /* Geometry. A partition starts at low_cyl * cyl_blks and spans
     * (high_cyl - low_cyl + 1) * cyl_blks blocks, where cyl_blks =
     * surfaces * sectors_per_track. Verified against the real fixture: with
     * surf=2 and spt=27, cyl_blks=54, and each partition's bootblock lands
     * exactly on its declared DOS type at low_cyl*54. */
    for (uint32_t i = 0; i < n; i++) {
        afs_rdb_partition_t *p = &rdb->part[i];
        uint32_t cyl_blks = p->surfaces * p->sectors_per_track;
        uint32_t cyl = (p->high_cyl >= p->low_cyl) ? (p->high_cyl - p->low_cyl + 1) : 0;
        p->start_block = p->low_cyl * cyl_blks;
        p->size_blocks = cyl * cyl_blks;
    }
    return 0;
}