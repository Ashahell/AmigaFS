#ifndef AMIGAFS_RDB_H
#define AMIGAFS_RDB_H

#include <stddef.h>
#include <stdint.h>

/* Rigid Disk Block (RDB) parsing for hard-disk images.
 *
 * A hard-disk image begins with an RDB chain describing the physical geometry
 * and linking to PARTITION blocks. Each partition names a volume and carries a
 * DosEnvVec holding that partition's DOS type, geometry and reserved count.
 * Inside a partition, every AmigaDOS block pointer is PARTITION-RELATIVE, so
 * mounting a partition means handing blockio a window onto it.
 *
 * Field layout (big-endian), verified against amitools'
 * fs/block/rdb/PartitionBlock.py and the real a590_WD93028XA_6parts.hdd:
 *   RDB/PART header: id[4] size hostID next flags   (next at long 4)
 *   PART: drv_name BCPL string at byte 36, DosEnvVec longs 32..51,
 *         dos_type at long 48.
 */

#define AFS_MAX_RDB_PARTITIONS 16

typedef struct {
    char     name[32];          /* DriveName, Latin-1 */
    uint32_t dos_type;          /* 'DOS' | flags, e.g. 0x444F5305 */
    uint32_t start_block;       /* absolute block in the image */
    uint32_t size_blocks;       /* blocks in this partition */
    uint32_t reserved;          /* reserved blocks (from DosEnvVec) */
    uint32_t surfaces;
    uint32_t sectors_per_track;
    uint32_t low_cyl, high_cyl;
} afs_rdb_partition_t;

typedef struct {
    uint32_t partition_count;
    afs_rdb_partition_t part[AFS_MAX_RDB_PARTITIONS];
} afs_rdb_t;

/* Parse the RDB in an image of `size` bytes. Returns 0 and fills `rdb` when an
 * RDB is present; returns non-zero (leaving rdb untouched) when the image has
 * no RDB (a plain floppy ADF), so the caller can fall back to treating the
 * whole image as a single volume. */
int afs_rdb_parse(const uint8_t *img, size_t size, size_t bsize, afs_rdb_t *rdb);

/* True if a stored DOS-type long names a filesystem we support (DOS\0..DOS\5).
 * DOS\6/DOS\7 (AmigaOS 3.2 long filenames) are NOT supported. */
int afs_rdb_dostype_supported(uint32_t dos_type);

#endif /* AMIGAFS_RDB_H */