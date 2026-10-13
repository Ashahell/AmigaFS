#include "amigafs/blocktypes.h"
#include "amigafs/be.h"

#include <string.h>

/* Every offset is expressed as (bsize - N) for the trailing fields, matching
 * the on-disk layout, and computed from the actual block size so a non-512
 * block works identically. */

size_t afs_hash_table_size(size_t bsize) {
    if (bsize < 4 * 57) return 0;          /* too small for a 56-long header */
    return bsize / 4 - 56;
}

afs_block_kind_t afs_block_kind(const uint8_t *blk, size_t bsize) {
    uint32_t primary = be32(blk);
    if (primary == AFS_T_DIRCACHE) return AFS_BLOCK_DIRCACHE;
    if (primary == AFS_T_DATA)     return AFS_BLOCK_DATABLOCK;

    uint32_t sec = be32(blk + bsize - 4);
    if (primary == AFS_T_LIST) {
        /* a T_LIST with ST_FILE is a file-extension block */
        return (sec == AFS_ST_FILE) ? AFS_BLOCK_FILEEXT : AFS_BLOCK_UNKNOWN;
    }
    if (primary == AFS_T_HEADER) {
        switch (sec) {
        case AFS_ST_ROOT:     return AFS_BLOCK_ROOT;
        case AFS_ST_USERDIR:  return AFS_BLOCK_DIR;
        case AFS_ST_FILE:     return AFS_BLOCK_FILE;
        case AFS_ST_SOFTLINK: return AFS_BLOCK_SOFTLINK;
        case AFS_ST_LINKFILE: return AFS_BLOCK_LINKFILE;
        case AFS_ST_LINKDIR:  return AFS_BLOCK_LINKDIR;
        default:              return AFS_BLOCK_UNKNOWN;
        }
    }
    return AFS_BLOCK_UNKNOWN;
}

int afs_parse_rootblock(const uint8_t *blk, size_t bsize, afs_rootblock_t *out) {
    if (!blk || !out || bsize < 4 * 57) return -1;
    if (afs_block_kind(blk, bsize) != AFS_BLOCK_ROOT) return -1;

    out->type     = be32(blk);
    out->ht_size  = be32(blk + 12);
    out->chksum   = be32(blk + 20);
    out->bm_flag  = be32(blk + bsize - 200);
    for (int i = 0; i < 25; i++) out->bm_pages[i] = be32(blk + bsize - 196 + 4 * i);
    out->bm_ext   = be32(blk + bsize - 96);
    out->dircache_block = be32(blk + bsize - 8);   /* extension field */
    bstr(blk + bsize - 80, 31, out->name, sizeof out->name);
    return 0;
}

int afs_parse_fileheader(const uint8_t *blk, size_t bsize, afs_fileheader_t *out) {
    if (!blk || !out || bsize < 4 * 57) return -1;
    afs_block_kind_t k = afs_block_kind(blk, bsize);
    if (k != AFS_BLOCK_FILE && k != AFS_BLOCK_DIR &&
        k != AFS_BLOCK_SOFTLINK && k != AFS_BLOCK_LINKFILE &&
        k != AFS_BLOCK_LINKDIR) return -1;

    out->type       = be32(blk);
    out->high_seq   = be32(blk + 8);
    out->first_data = be32(blk + 16);
    /* UID/GID are 16-bit (C2) */
    out->uid        = be16(blk + bsize - 196);
    out->gid        = be16(blk + bsize - 194);
    out->protect    = be32(blk + bsize - 192);
    out->byte_size  = be32(blk + bsize - 188);
    out->days       = be32(blk + bsize - 92);
    out->mins       = be32(blk + bsize - 88);
    out->ticks      = be32(blk + bsize - 84);
    out->hash_chain = be32(blk + bsize - 16);
    out->parent     = be32(blk + bsize - 12);
    out->extension  = be32(blk + bsize - 8);
    out->link_real  = be32(blk + bsize - 44);
    out->next_link  = be32(blk + bsize - 40);
    bstr(blk + bsize - 80, 31, out->name, sizeof out->name);
    return 0;
}

int afs_parse_dircache(const uint8_t *blk, size_t bsize, afs_dircache_t *out) {
    if (!blk || !out || bsize < 4 * 57) return -1;
    if (afs_block_kind(blk, bsize) != AFS_BLOCK_DIRCACHE) return -1;

    out->type       = be32(blk);
    out->header_key = be32(blk + 4);
    out->parent     = be32(blk + 8);
    out->records_nb = be32(blk + 12);
    out->next_dirc  = be32(blk + 16);
    out->chksum     = be32(blk + 20);

    /* The on-disk record size is variable (26..77 bytes), so we must walk it
     * rather than index it. records_nb is untrusted: clamp it to what the
     * block can physically hold. */
    size_t avail = bsize - 24;
    size_t maxrec = avail / 26;                 /* each record is >= 26 bytes */
    if (maxrec > AFS_DIRCACHE_MAX_RECORDS) maxrec = AFS_DIRCACHE_MAX_RECORDS;
    size_t want = out->records_nb;
    if (want > maxrec) want = maxrec;

    size_t off = 24;
    size_t n = 0;
    for (; n < want; n++) {
        if (off + 26 > bsize) break;            /* cannot even fit a minimal record */
        afs_dircache_rec_t *r = &out->rec[n];
        r->header  = be32(blk + off);
        r->size    = be32(blk + off + 4);
        r->protect = be32(blk + off + 8);
        r->uid     = be16(blk + off + 12);
        r->gid     = be16(blk + off + 14);
        /* days/mins/ticks are 16-bit SIGNED */
        r->days    = (int16_t)be16(blk + off + 16);
        r->mins    = (int16_t)be16(blk + off + 18);
        r->ticks   = (int16_t)be16(blk + off + 20);
        r->sec_type = blk[off + 22];

        size_t name_off = off + 24;
        size_t nl = blk[off + 23];
        if (nl > 30) nl = 30;
        if (name_off + nl > bsize) nl = (nl > name_off) ? 0 : (bsize - name_off);
        memcpy(r->name, blk + name_off, nl);
        r->name[nl] = '\0';

        /* advance: 24 + name_len + comm_len + optional pad to word alignment */
        size_t cl_off = name_off + nl;
        size_t cl = (cl_off < bsize) ? blk[cl_off] : 0;
        if (cl > 22) cl = 22;
        size_t next = cl_off + cl;
        if (next < bsize) next++;               /* the padding byte */
        /* records are word (2-byte) aligned within the block */
        if (next & 1) next++;
        if (next <= off) break;                 /* no forward progress: stop */
        off = next;
    }
    out->rec_count = n;
    return 0;
}

int afs_parse_ofs_datablock(const uint8_t *blk, size_t bsize, afs_ofsdatablock_t *out) {
    if (!blk || !out || bsize < 24) return -1;
    if (be32(blk) != AFS_T_DATA) return -1;
    out->type       = be32(blk);
    out->header_key = be32(blk + 4);
    out->seq_num    = be32(blk + 8);
    out->data_size  = be32(blk + 12);
    out->next_data  = be32(blk + 16);
    out->payload    = blk + 24;
    /* OFS data_size must fit the BSIZE-24 payload */
    if (out->data_size > bsize - 24) return -1;
    return 0;
}