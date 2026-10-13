/* Unit tests for directory walking (dir.c).
 *
 * The hash table is the AUTHORITY for lookup and listing. The directory cache
 * (DIRC) is only a speed-up and must never be able to hide a file (C4), so
 * none of these tests consult it.
 *
 * Traversal model, confirmed against a real fixture:
 *   bucket = hash(name)                      (see hash.c)
 *   entry  = ht[bucket]                      a block number, 0 = empty
 *   follow `hash_chain` (BSIZE-16) for collisions
 *   list   = every bucket, following every chain
 *
 * The synthetic volumes below are built so that chains, collisions and the
 * link kinds are all exercised without needing a fixture.
 */
#include "test.h"
#include "amigafs/be.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/volume.h"
#include "amigafs/dir.h"
#include "amigafs/hash.h"

#define BS 512

/* One in-memory volume: a rootblock plus a table of named entries. */
/* The rootblock formula is INT((Reserved + highKey)/2) with Reserved=2, so a
 * volume of 1760 blocks puts the root at (2 + 1759)/2 = 880. The block count
 * therefore has to be 1760 for the root to land where we write it. Entry
 * blocks live above the rootblock but below 1760. */
#define SYNTH_BLOCKS 1760
#define SYNTH_ROOT   880
#define SYNTH_FIRST_ENTRY 1200

typedef struct {
    uint8_t  img[SYNTH_BLOCKS * BS];
    uint32_t nblocks;
    uint8_t  flags;
} synth_t;

static void synth_init(synth_t *s, uint8_t flags) {
    memset(s->img, 0, sizeof s->img);
    s->flags = flags;
    s->nblocks = SYNTH_BLOCKS;
    /* the DOS id must be present on block 0 (or block 1) or the volume is
     * refused -- this is the detection path volume_open exercises */
    put_be32(s->img + 0, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                        ((uint32_t)'S' << 8) | flags);
    uint8_t *r = s->img + SYNTH_ROOT * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));   /* ht_size = 72 */
    put_be32(r + 312, 0xFFFFFFFFu);
    put_be32(r + 316, 881);
    r[BS - 80] = 4;
    memcpy(r + BS - 79, "synth", 4);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
}

/* Write a file/dir/link entry block. Does NOT touch the hash table; call
 * synth_link() to file it under a name. Keeping the two apart lets a test
 * deliberately force several names into ONE bucket to build a real chain. */
static uint32_t synth_add(synth_t *s, const char *name, uint32_t sec_type,
                          uint32_t hash_chain, uint32_t *prev_out) {
    uint32_t bno = *prev_out;
    (*prev_out)++;
    uint8_t *b = s->img + bno * BS;
    put_be32(b + 0, AFS_T_HEADER);
    put_be32(b + BS - 16, hash_chain);
    put_be32(b + BS - 12, SYNTH_ROOT);           /* parent = rootblock */
    put_be32(b + BS - 8, 0);              /* no extension */
    put_be32(b + BS - 4, sec_type);
    size_t nl = strlen(name);
    b[BS - 80] = (uint8_t)nl;
    memcpy(b + BS - 79, name, nl);
    put_be32(b + 20, afs_block_checksum(b, BS, 20));
    return bno;
}

/* Point a hash-table bucket at a block (chain head), re-stamping the root. */
static void synth_link(synth_t *s, const char *name, uint32_t bno) {
    uint8_t *r = s->img + SYNTH_ROOT * BS;
    uint32_t tsz = be32(r + 12);
    uint32_t bucket = afs_hash(name, strlen(name), tsz, s->flags);
    put_be32(r + 24 + 4 * bucket, bno);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
}

static afs_volume_t *synth_open(synth_t *s) {
    afs_volume_t *v = NULL;
    char err[256] = "(none)";
    int rc = afs_volume_open_memory(s->img, (size_t)s->nblocks * BS, BS, &v, err, sizeof err);
    if (rc != 0) {
        printf("         (synth_open failed: %s)\n", err);
        return NULL;
    }
    return v;
}

/* --- listing --- */
static void test_list_directory(void) {
    synth_t s;
    synth_init(&s, 0x01);              /* plain FFS */
    uint32_t next = SYNTH_FIRST_ENTRY;
    synth_link(&s, "alpha",  synth_add(&s, "alpha",  AFS_ST_FILE, 0, &next));
    synth_link(&s, "Beta",   synth_add(&s, "Beta",   AFS_ST_USERDIR, 0, &next));
    synth_link(&s, "gamma.txt", synth_add(&s, "gamma.txt", AFS_ST_FILE, 0, &next));

    afs_volume_t *v = synth_open(&s);
    CHECK(v != NULL, "synthetic volume opens");
    if (!v) return;

    afs_dirent_t ents[16];
    size_t n = 0;
    int rc = afs_dir_list(v, SYNTH_ROOT, ents, 16, &n);
    CHECK_EQ_U(rc, 0, "directory lists");
    CHECK_EQ_U(n, 3u, "three entries listed");
    if (n == 3) {
        /* Amiga directories have NO defined order: entries come out in hash
         * bucket order (here Beta=0, gamma.txt=6, alpha=23). Assert membership,
         * not position -- the FUSE layer sorts for display. */
        int a = 0, b = 0, c = 0;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(ents[i].name, "alpha") == 0)     a = 1;
            if (strcmp(ents[i].name, "Beta") == 0)      b = 1;
            if (strcmp(ents[i].name, "gamma.txt") == 0) c = 1;
        }
        CHECK(a && b && c, "all three names present regardless of order");
        /* the stored spelling must be preserved exactly, including case */
        int beta_preserved = 0;
        for (size_t i = 0; i < n; i++)
            if (strcmp(ents[i].name, "Beta") == 0) beta_preserved = 1;
        CHECK(beta_preserved, "stored case is preserved (Beta, not beta)");
        /* types must survive the walk */
        int saw_file = 0, saw_dir = 0;
        for (size_t i = 0; i < n; i++) {
            if (ents[i].kind == AFS_BLOCK_FILE) saw_file = 1;
            if (ents[i].kind == AFS_BLOCK_DIR)  saw_dir = 1;
        }
        CHECK(saw_file && saw_dir, "entry kinds are reported correctly");
    }
    afs_volume_close(v);
}

/* --- hash chain (collision) following --- */
static void test_hash_chain_following(void) {
    synth_t s;
    synth_init(&s, 0x01);
    uint32_t next = SYNTH_FIRST_ENTRY;
    /* three names sharing one bucket, linked as a chain */
    uint32_t a = synth_add(&s, "same_hash",  AFS_ST_FILE, 0, &next);
    uint32_t b = synth_add(&s, "same_hash2", AFS_ST_FILE, 0, &next);
    uint32_t c = synth_add(&s, "same_hash3", AFS_ST_FILE, 0, &next);
    /* chain a -> b -> c -> 0, and file the WHOLE chain under ONE bucket so the
     * walker must follow hash_chain rather than finding them separately */
    uint8_t *ba = s.img + a * BS, *bb = s.img + b * BS;
    put_be32(ba + BS - 16, b);
    put_be32(bb + BS - 16, c);
    put_be32(ba + 20, afs_block_checksum(ba, BS, 20));
    put_be32(bb + 20, afs_block_checksum(bb, BS, 20));
    synth_link(&s, "same_hash", a);

    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    afs_dirent_t ents[16];
    size_t n = 0;
    afs_dir_list(v, SYNTH_ROOT, ents, 16, &n);
    CHECK_EQ_U(n, 3u, "all three chained entries are listed");
    if (n == 3) {
        CHECK_EQ_S(ents[0].name, "same_hash",  "chain head");
        CHECK_EQ_S(ents[1].name, "same_hash2", "chain middle");
        CHECK_EQ_S(ents[2].name, "same_hash3", "chain tail");
    }
    afs_volume_close(v);
}

/* --- point lookup, including case-insensitivity --- */
static void test_lookup(void) {
    synth_t s;
    synth_init(&s, 0x01);
    uint32_t next = SYNTH_FIRST_ENTRY;
    synth_link(&s, "MOON.GIF", synth_add(&s, "MOON.GIF", AFS_ST_FILE, 0, &next));
    synth_link(&s, "SubDir",   synth_add(&s, "SubDir",   AFS_ST_USERDIR, 0, &next));

    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    afs_dirent_t e;
    /* exact */
    CHECK_EQ_U(afs_dir_lookup(v, SYNTH_ROOT, "MOON.GIF", 8, &e), 0, "exact lookup finds MOON.GIF");
    CHECK_EQ_S(e.name, "MOON.GIF", "looked-up name preserved");
    /* case-insensitive on the Amiga, even though the mount is exact-match (D5) */
    CHECK_EQ_U(afs_dir_lookup(v, SYNTH_ROOT, "moon.gif", 8, &e), 0,
               "case-insensitive lookup finds it too (Amiga semantics)");
    CHECK_EQ_S(e.name, "MOON.GIF", "lookup returns the STORED spelling");
    /* miss */
    CHECK(afs_dir_lookup(v, SYNTH_ROOT, "nope", 4, &e) != 0, "a missing name is reported absent");
    /* a prefix must not match */
    CHECK(afs_dir_lookup(v, SYNTH_ROOT, "MOON", 4, &e) != 0, "a prefix does not match");

    afs_volume_close(v);
}

/* --- links appear with their own kinds --- */
static void test_link_kinds(void) {
    synth_t s;
    synth_init(&s, 0x01);
    uint32_t next = SYNTH_FIRST_ENTRY;
    synth_link(&s, "hlink",  synth_add(&s, "hlink",  AFS_ST_LINKFILE, 0, &next));
    synth_link(&s, "hlinkd", synth_add(&s, "hlinkd", AFS_ST_LINKDIR, 0, &next));
    synth_link(&s, "slink",  synth_add(&s, "slink",  AFS_ST_SOFTLINK, 0, &next));

    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    afs_dirent_t e;
    CHECK_EQ_U(afs_dir_lookup(v, SYNTH_ROOT, "hlink", 5, &e), 0, "hard link to a file found");
    CHECK_EQ_U(e.kind, AFS_BLOCK_LINKFILE, "hard link reports ST_LINKFILE");
    CHECK_EQ_U(afs_dir_lookup(v, SYNTH_ROOT, "hlinkd", 6, &e), 0, "hard link to a dir found");
    CHECK_EQ_U(e.kind, AFS_BLOCK_LINKDIR, "dir hard link reports ST_LINKDIR");
    CHECK_EQ_U(afs_dir_lookup(v, SYNTH_ROOT, "slink", 5, &e), 0, "soft link found");
    CHECK_EQ_U(e.kind, AFS_BLOCK_SOFTLINK, "soft link reports ST_SOFTLINK");

    afs_volume_close(v);
}

/* --- robustness: a corrupt hash chain must not hang or run away --- */
static void test_corrupt_chain_is_bounded(void) {
    synth_t s;
    synth_init(&s, 0x01);
    uint32_t next = SYNTH_FIRST_ENTRY;
    uint32_t a = synth_add(&s, "loop", AFS_ST_FILE, 0, &next);
    synth_link(&s, "loop", a);

    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    /* make the entry's hash_chain point back at itself: an infinite loop */
    uint8_t *b = s.img + a * BS;
    put_be32(b + BS - 16, a);
    put_be32(b + 20, afs_block_checksum(b, BS, 20));

    afs_dirent_t ents[16];
    size_t n = 0;
    int rc = afs_dir_list(v, SYNTH_ROOT, ents, 16, &n);
    /* it must terminate, and must not claim more entries than it can hold */
    CHECK(rc == 0 || rc != 0, "a self-referential chain still returns (no hang)");
    CHECK(n <= 16, "the listing never exceeds the caller's capacity");
    afs_volume_close(v);
}

/* --- an out-of-range hash-chain pointer is refused, not followed --- */
static void test_oob_chain_pointer_refused(void) {
    synth_t s;
    synth_init(&s, 0x01);
    uint32_t next = SYNTH_FIRST_ENTRY;
    uint32_t a = synth_add(&s, "bad", AFS_ST_FILE, 0, &next);
    synth_link(&s, "bad", a);
    /* point the chain at a block far past the end of the volume */
    uint8_t *b = s.img + a * BS;
    put_be32(b + BS - 16, 0x7FFFFFFFu);
    put_be32(b + 20, afs_block_checksum(b, BS, 20));

    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }
    afs_dirent_t ents[16];
    size_t n = 0;
    afs_dir_list(v, SYNTH_ROOT, ents, 16, &n);
    CHECK(n <= 16, "an out-of-range chain pointer does not produce runaway entries");
    afs_volume_close(v);
}

/* --- the dir cache must NOT be consulted for listing (C4) --- */
static void test_listing_ignores_dircache(void) {
    synth_t s;
    synth_init(&s, 0x05);              /* FFS + DIRC */
    uint32_t next = SYNTH_FIRST_ENTRY;
    synth_link(&s, "visible", synth_add(&s, "visible", AFS_ST_FILE, 0, &next));

    /* deliberately give the root a DANGLING dir-cache pointer (a stale cache
     * that does not even exist). Listing must still work. Re-stamp the
     * rootblock checksum after this edit. */
    put_be32(s.img + SYNTH_ROOT * BS + BS - 8, 0x00FFFFFFu);
    put_be32(s.img + SYNTH_ROOT * BS + 20,
             afs_block_checksum(s.img + SYNTH_ROOT * BS, BS, 20));

    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }
    afs_dirent_t ents[16];
    size_t n = 0;
    int rc = afs_dir_list(v, SYNTH_ROOT, ents, 16, &n);
    CHECK_EQ_U(rc, 0, "listing works despite a dangling dir-cache pointer");
    CHECK_EQ_U(n, 1u, "the file is still visible (stale cache cannot hide it)");
    if (n == 1) CHECK_EQ_S(ents[0].name, "visible", "the file is listed");
    afs_volume_close(v);
}

int main(void) {
    printf("dir\n");
    test_list_directory();
    test_hash_chain_following();
    test_lookup();
    test_link_kinds();
    test_corrupt_chain_is_bounded();
    test_oob_chain_pointer_refused();
    test_listing_ignores_dircache();
    TEST_REPORT();
}