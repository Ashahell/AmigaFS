#include "amigafs/volume.h"
#include "amigafs/be.h"
#include "amigafs/blockio.h"
#include "amigafs/checksum.h"
#include "amigafs/hash.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct afs_volume {
    blockdev_t      *dev;
    uint8_t          flags;        /* DOS type flag byte */
    unsigned         dostype;      /* 0..5, or 6/7 which we refuse */
    uint32_t         rootblock;
    uint32_t         block_count;
    size_t           bsize;
    afs_rootblock_t  root;
    char             name[31];
};

/* DOS id as the big-endian long 'D','O','S' (no flag byte). */
#define DOS_ID_BASE (((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) | ((uint32_t)'S' << 8))

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...) {
    if (!errbuf || errlen == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

/* Read the DOS id from block 0, or block 1 if block 0 does not carry it
 * (some media place it there). Returns the flag byte, or 0xFF if absent. */
static uint8_t read_dos_id(afs_volume_t *v, uint8_t *out_long) {
    uint8_t blk[512];
    for (int which = 0; which < 2; which++) {
        if (blockdev_read(v->dev, (uint32_t)which, blk, sizeof blk) != 0)
            return 0xFF;
        uint32_t longval = be32(blk);
        if ((longval & 0xFFFFFF00u) == DOS_ID_BASE) {
            if (out_long) *out_long = blk[3];
            return blk[3];
        }
    }
    return 0xFF;
}

/* The single rootblock formula: INT((Reserved + highKey)/2). For floppies
 * Reserved=2 and highKey = last block, giving 880 (DD) and 1760 (HD). */
static uint32_t rootblock_formula(uint32_t reserved, uint32_t highkey) {
    return (reserved + highkey) / 2;
}

static int finish_open(afs_volume_t *v, char *errbuf, size_t errlen) {
    /* refuse anything outside DOS\0..DOS\5 by NAME, not silently */
    if (v->dostype > 5) {
        set_err(errbuf, errlen,
                "DOS\\%u is not a supported AmigaDOS volume "
                "(only DOS\\0-DOS\\5; DOS\\6/DOS\\7 are AmigaOS 3.2 "
                "long-filename types)", v->dostype);
        return -1;
    }

    v->rootblock = rootblock_formula(2, v->block_count - 1);

    uint8_t *rootbuf = malloc(v->bsize);
    if (!rootbuf) { set_err(errbuf, errlen, "out of memory"); return -1; }
    if (blockdev_read(v->dev, v->rootblock, rootbuf, v->bsize) != 0) {
        set_err(errbuf, errlen, "cannot read rootblock at %u", v->rootblock);
        free(rootbuf);
        return -1;
    }
    /* Validate BEFORE trusting: the block must be T_HEADER/ST_ROOT. */
    if (afs_block_kind(rootbuf, v->bsize) != AFS_BLOCK_ROOT) {
        set_err(errbuf, errlen,
                "block %u is not a T_HEADER/ST_ROOT rootblock", v->rootblock);
        free(rootbuf);
        return -1;
    }
    if (!afs_block_valid(rootbuf, v->bsize, 20)) {
        set_err(errbuf, errlen,
                "rootblock checksum invalid at block %u", v->rootblock);
        free(rootbuf);
        return -1;
    }
    if (afs_parse_rootblock(rootbuf, v->bsize, &v->root) != 0) {
        set_err(errbuf, errlen, "rootblock parse failed");
        free(rootbuf);
        return -1;
    }
    memcpy(v->name, v->root.name, sizeof v->name);
    v->name[sizeof v->name - 1] = '\0';
    free(rootbuf);
    return 0;
}

int afs_volume_open_memory(void *data, size_t size, size_t bsize,
                           afs_volume_t **out, char *errbuf, size_t errlen) {
    if (out) *out = NULL;
    afs_volume_t *v = calloc(1, sizeof *v);
    if (!v) { set_err(errbuf, errlen, "out of memory"); return -1; }

    v->bsize = bsize;
    v->dev = blockdev_open_memory(data, size, bsize);
    if (!v->dev) {
        set_err(errbuf, errlen, "cannot open block device (%zu bytes, bsize %zu)",
                size, bsize);
        free(v);
        return -1;
    }
    v->block_count = blockdev_block_count(v->dev);

    uint8_t idbuf[8];
    uint8_t flags = read_dos_id(v, idbuf);
    if (flags == 0xFF) {
        set_err(errbuf, errlen, "no DOS filesystem id on block 0 or block 1");
        blockdev_close(v->dev);
        free(v);
        return -1;
    }
    /* Only the low three bits are defined; DOS\6/\7 are the long-name types. */
    v->dostype = flags & 0x07u;
    v->flags = flags;

    if (finish_open(v, errbuf, errlen) != 0) {
        blockdev_close(v->dev);
        free(v);
        return -1;
    }
    if (out) *out = v;
    return 0;
}

int afs_volume_open_file(const char *path, size_t bsize, uint32_t start_block,
                         afs_volume_t **out, char *errbuf, size_t errlen) {
    if (out) *out = NULL;
    afs_volume_t *v = calloc(1, sizeof *v);
    if (!v) { set_err(errbuf, errlen, "out of memory"); return -1; }

    v->bsize = bsize;
    blockdev_t *base = blockdev_open_file(path, bsize);
    if (!base) {
        set_err(errbuf, errlen, "cannot open image '%s'", path);
        free(v);
        return -1;
    }
    if (start_block) {
        uint32_t n = blockdev_block_count(base) - start_block;
        v->dev = blockdev_window(base, start_block, n);
        if (!v->dev) {
            set_err(errbuf, errlen, "invalid partition window in '%s'", path);
            blockdev_close(base);
            free(v);
            return -1;
        }
    } else {
        v->dev = base;
    }
    v->block_count = blockdev_block_count(v->dev);

    uint8_t flags = read_dos_id(v, NULL);
    if (flags == 0xFF) {
        set_err(errbuf, errlen, "no DOS filesystem id on block 0 or block 1");
        blockdev_close(base);
        free(v);
        return -1;
    }
    v->dostype = flags & 0x07u;
    v->flags = flags;

    if (finish_open(v, errbuf, errlen) != 0) {
        blockdev_close(base);
        free(v);
        return -1;
    }
    if (out) *out = v;
    return 0;
}

void afs_volume_close(afs_volume_t *v) {
    if (!v) return;
    blockdev_close(v->dev);
    free(v);
}

uint8_t     afs_volume_flags(const afs_volume_t *v)       { return v->flags; }
unsigned    afs_volume_dostype(const afs_volume_t *v)    { return v->dostype; }
const char *afs_volume_name(const afs_volume_t *v)        { return v->name; }
uint32_t    afs_volume_rootblock(const afs_volume_t *v)   { return v->rootblock; }
uint32_t    afs_volume_block_count(const afs_volume_t *v) { return v->block_count; }
size_t      afs_volume_block_size(const afs_volume_t *v)  { return v->bsize; }
const afs_rootblock_t *afs_volume_root(const afs_volume_t *v) { return &v->root; }

/* OFS is the ABSENCE of the FFS bit -- DOS\2 and DOS\4 are OFS too. A test of
 * `flags == 0` would misclassify them (the AROS bug, C5). */
int afs_volume_is_ofs(const afs_volume_t *v)   { return afs_is_ofs(v->flags); }
int afs_volume_is_ffs(const afs_volume_t *v)   { return afs_is_ffs(v->flags); }
int afs_volume_has_intl(const afs_volume_t *v) { return afs_has_intl(v->flags); }
int afs_volume_has_dircache(const afs_volume_t *v) { return afs_has_dircache(v->flags); }

int afs_volume_read_block(afs_volume_t *v, uint32_t blockno, void *buf, size_t len) {
    if (!v) return -1;
    return blockdev_read(v->dev, blockno, buf, len);
}