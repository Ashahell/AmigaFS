#ifndef AMIGAFS_BLOCKTYPES_H
#define AMIGAFS_BLOCKTYPES_H

#include <stddef.h>
#include <stdint.h>

/* On-disk block types and parsed block structures.
 *
 * A block's identity is its PRIMARY type plus (for T_HEADER blocks) its
 * SECONDARY type: T_HEADER covers rootblock, file header, directory block and
 * link blocks alike. Dir-cache blocks instead use the distinct primary type
 * 33 (0x21).
 *
 * All field offsets are relative to the block size and were verified against
 * real fixture bytes; see docs/format-notes.md. Nothing here is hardcoded to
 * 512 bytes -- every table size derives from the block size.
 */

/* Primary types. */
#define AFS_T_HEADER   2u
#define AFS_T_DATA     8u
#define AFS_T_LIST    16u
#define AFS_T_DIRCACHE 33u   /* 0x21, DIRC only */

/* Secondary types. ST_FILE and ST_LINKFILE are negative on disk. */
#define AFS_ST_ROOT      1u
#define AFS_ST_USERDIR   2u
#define AFS_ST_SOFTLINK  3u
#define AFS_ST_LINKDIR   4u
#define AFS_ST_FILE      0xFFFFFFFDu  /* -3 */
#define AFS_ST_LINKFILE  0xFFFFFFFCu  /* -4 */

/* What a block IS, after resolving both types. */
typedef enum {
    AFS_BLOCK_UNKNOWN = 0,
    AFS_BLOCK_ROOT,       /* rootblock:  T_HEADER / ST_ROOT       */
    AFS_BLOCK_DIR,        /* directory:  T_HEADER / ST_USERDIR    */
    AFS_BLOCK_FILE,       /* file:       T_HEADER / ST_FILE       */
    AFS_BLOCK_FILEEXT,    /* extension:  T_LIST   / ST_FILE        */
    AFS_BLOCK_SOFTLINK,   /* symlink:    T_HEADER / ST_SOFTLINK   */
    AFS_BLOCK_LINKFILE,   /* hard link:  T_HEADER / ST_LINKFILE    */
    AFS_BLOCK_LINKDIR,    /* dir  link:  T_HEADER / ST_LINKDIR    */
    AFS_BLOCK_DIRCACHE,   /* dircache:   T_DIRCACHE                */
    AFS_BLOCK_DATABLOCK,  /* OFS data:   T_DATA                    */
    AFS_BLOCK_BITMAP,     /* bitmap block (identified by position) */
    AFS_BLOCK_BITMAPEXT   /* bitmap extension block                 */
} afs_block_kind_t;

/* Number of hash-table entries in a block of `bsize` bytes: BSIZE/4 - 56.
 * Returns 0 for block sizes too small to hold a valid table (never
 * underflows). */
size_t afs_hash_table_size(size_t bsize);

/* Resolve a block's kind from its primary and secondary types. */
afs_block_kind_t afs_block_kind(const uint8_t *blk, size_t bsize);

/* --- parsed block structures ---
 * Every parse returns 0 on success and non-zero if the block is not the kind
 * it claims to be (checked via afs_block_kind) or a length field is out of
 * range. */
typedef struct {
    uint32_t type;          /* primary type (T_HEADER)  */
    uint32_t ht_size;       /* hash table entries       */
    uint32_t chksum;
    uint32_t bm_flag;       /* -1 when the bitmap is valid */
    uint32_t bm_pages[25];  /* bitmap block pointers    */
    uint32_t bm_ext;        /* first bitmap extension block, 0 if none */
    uint32_t dircache_block;/* extension field: first dir-cache block */
    char     name[31];      /* volume name, Latin-1     */
} afs_rootblock_t;

typedef struct {
    uint32_t type;
    uint32_t high_seq;      /* number of data block pointers in this block */
    uint32_t first_data;    /* OFS only */
    uint16_t uid;           /* 16-bit (C2) */
    uint16_t gid;           /* 16-bit (C2) */
    uint32_t protect;
    uint32_t byte_size;
    uint32_t days, mins, ticks;      /* Amiga timestamp fields (BSIZE-92/-88/-84) */
    uint32_t hash_chain;
    uint32_t parent;
    uint32_t extension;     /* file-extension chain, or dir-cache chain */
    uint32_t link_real;     /* real_entry, for hard links */
    uint32_t next_link;     /* next_link, for hard links */
    char     name[31];
} afs_fileheader_t;

/* Dir-cache records. A block holds up to (bsize-24)/26 of them; cap the array
 * so a corrupt records_nb cannot overflow us. */
#define AFS_DIRCACHE_MAX_RECORDS 64
typedef struct {
    uint32_t header;        /* entry block pointer (link block for links) */
    uint32_t size;          /* 0 for directories and links */
    uint32_t protect;
    uint16_t uid;
    uint16_t gid;
    int16_t  days, mins, ticks;   /* 16-bit SIGNED on disk */
    uint8_t  sec_type;
    char     name[31];
} afs_dircache_rec_t;

typedef struct {
    uint32_t type;          /* T_DIRCACHE */
    uint32_t header_key;
    uint32_t parent;
    uint32_t records_nb;
    uint32_t next_dirc;
    uint32_t chksum;
    size_t   rec_count;     /* records actually decoded (clamped) */
    afs_dircache_rec_t rec[AFS_DIRCACHE_MAX_RECORDS];
} afs_dircache_t;

typedef struct {
    uint32_t type;          /* T_DATA */
    uint32_t header_key;    /* -> file header */
    uint32_t seq_num;
    uint32_t data_size;     /* <= BSIZE-24 */
    uint32_t next_data;
    const uint8_t *payload; /* points into the caller's buffer, at byte 24 */
} afs_ofsdatablock_t;

int afs_parse_rootblock(const uint8_t *blk, size_t bsize, afs_rootblock_t *out);
int afs_parse_fileheader(const uint8_t *blk, size_t bsize, afs_fileheader_t *out);
int afs_parse_dircache(const uint8_t *blk, size_t bsize, afs_dircache_t *out);
int afs_parse_ofs_datablock(const uint8_t *blk, size_t bsize, afs_ofsdatablock_t *out);

#endif /* AMIGAFS_BLOCKTYPES_H */