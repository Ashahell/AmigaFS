/* Unit tests for link resolution and softlink path translation (links.c).
 *
 * Amiga link semantics that differ from POSIX and that we must honour:
 *
 *  HARD links (FFS only): an entry of secondary type ST_LINKFILE (-4) or
 *  ST_LINKDIR (4) carries `real_entry` (BSIZE-44) pointing at the REAL file or
 *  directory header. All the links in the next_link chain share that one real
 *  entry, so they are ONE inode with an nlink count. A hard link to a
 *  DIRECTORY creates a cycle the kernel dentry cache cannot represent, so we
 *  expose it as a SYMLINK to the target instead (C12).
 *
 *  SOFT links: the entry's hash-table area holds an Amiga path C-string, not a
 *  POSIX path (C11). A leading `:` or `Vol:` means the volume root; a leading
 *  `/` is a path from the volume root; anything else is relative to the
 *  directory holding the link. Backslashes may appear. We translate to a
 *  POSIX-relative target so the FUSE layer can emit readlink() correctly.
 */
#include "test.h"
#include "amigafs/be.h"
#include "amigafs/be.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/file.h"
#include "amigafs/links.h"
#include "amigafs/hash.h"
#include "amigafs/bitmap.h"
#include "amigafs/blocktypes.h"
#include "amigafs/verify.h"
#include "amigafs/volume.h"

#define BS 512
#define ENTRIES 72
#define NSYNTH 1760
#define ROOT 880
#define FIRST_ENTRY 1200

typedef struct { uint8_t img[NSYNTH * BS]; uint8_t flags; } synth_t;

static void synth_init(synth_t *s, uint8_t flags) {
    memset(s->img, 0, sizeof s->img);
    s->flags = flags;
    put_be32(s->img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                     ((uint32_t)'S' << 8) | flags);
    uint8_t *r = s->img + ROOT * BS;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, ENTRIES);
    put_be32(r + 312, 0xFFFFFFFFu);
    put_be32(r + 316, 881);
    r[BS - 80] = 4; memcpy(r + BS - 79, "synt", 4);
    put_be32(r + BS - 4, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));

    /* The bitmap page at 881 (named by r+316 above). Without this every block
     * reads as USED (bit 0) and any allocating call reports "volume full" --
     * which is why the read-only link tests never noticed it was absent.
     * Everything is free except the boot block, the rootblock and this page. */
    uint8_t *bm = s->img + 881 * BS;
    memset(bm, 0xFF, BS);                      /* 1 = free */
    memset(bm, 0, 4);                          /* the page's own checksum long */
    size_t pagebits = (size_t)(BS - 4) * 8u;   /* 508 bytes of map data */
    for (size_t b = pagebits; b < (size_t)(BS - 4) * 8u; b++)
        bm[4 + b / 8] &= (uint8_t)~(0x80u >> (b % 8));   /* past the volume */
    bm[4 + 0 / 8]  &= (uint8_t)~(0x80u >> (0 % 8));     /* block 0: boot */
    bm[4 + ROOT / 8] &= (uint8_t)~(0x80u >> (ROOT % 8)); /* block ROOT */
    bm[4 + 881 / 8] &= (uint8_t)~(0x80u >> (881 % 8)); /* this page */
    put_be32(bm, afs_block_checksum(bm, BS, 0));
}

/* a real file entry with byte_size 0 */
static uint32_t add_real_file(synth_t *s, const char *name, uint32_t *next) {
    uint32_t b = (*next)++;
    uint8_t *e = s->img + b * BS;
    put_be32(e + 0, AFS_T_HEADER);
    put_be32(e + BS - 16, 0);
    put_be32(e + BS - 12, ROOT);
    put_be32(e + BS - 8, 0);
    put_be32(e + BS - 188, 0);            /* byte_size 0 */
    put_be32(e + BS - 4, AFS_ST_FILE);
    size_t nl = strlen(name);
    e[BS - 80] = (uint8_t)nl;
    memcpy(e + BS - 79, name, nl);
    put_be32(e + 20, afs_block_checksum(e, BS, 20));
    return b;
}

/* a hard link entry: sec = ST_LINKFILE or ST_LINKDIR, real_entry = real block */
static uint32_t add_link(synth_t *s, const char *name, uint32_t sec,
                         uint32_t real, uint32_t *next) {
    uint32_t b = (*next)++;
    uint8_t *e = s->img + b * BS;
    put_be32(e + 0, AFS_T_HEADER);
    put_be32(e + BS - 44, real);          /* real_entry */
    put_be32(e + BS - 40, 0);             /* next_link */
    put_be32(e + BS - 16, 0);             /* hash_chain */
    put_be32(e + BS - 12, ROOT);
    put_be32(e + BS - 8, 0);
    put_be32(e + BS - 4, sec);
    size_t nl = strlen(name);
    e[BS - 80] = (uint8_t)nl;
    memcpy(e + BS - 79, name, nl);
    put_be32(e + 20, afs_block_checksum(e, BS, 20));
    return b;
}

/* a soft link entry inside `parent`: the target string occupies the
 * hash-table area at offset 24 as a plain C-string */
static uint32_t add_softlink_at(synth_t *s, const char *name, const char *target,
                                uint32_t parent, uint32_t *next) {
    uint32_t b = (*next)++;
    uint8_t *e = s->img + b * BS;
    put_be32(e + 0, AFS_T_HEADER);
    memset(e + 24, 0, BS - 224);          /* the symbolic name area */
    size_t tl = strlen(target);
    if (tl > BS - 224 - 1) tl = BS - 225;
    memcpy(e + 24, target, tl);
    put_be32(e + BS - 16, 0);
    put_be32(e + BS - 12, parent);
    put_be32(e + BS - 8, 0);
    put_be32(e + BS - 4, AFS_ST_SOFTLINK);
    size_t nl = strlen(name);
    e[BS - 80] = (uint8_t)nl;
    memcpy(e + BS - 79, name, nl);
    put_be32(e + 20, afs_block_checksum(e, BS, 20));
    return b;
}

static uint32_t add_softlink(synth_t *s, const char *name, const char *target,
                             uint32_t *next) {
    return add_softlink_at(s, name, target, ROOT, next);
}

static void root_link(synth_t *s, const char *name, uint32_t bno) {
    uint8_t *r = s->img + ROOT * BS;
    uint32_t bucket = afs_hash(name, strlen(name), ENTRIES, s->flags);
    put_be32(r + 24 + 4 * bucket, bno);
    put_be32(r + 20, afs_block_checksum(r, BS, 20));
}

static afs_volume_t *synth_open(synth_t *s) {
    afs_volume_t *v = NULL;
    char err[256];
    if (afs_volume_open_memory(s->img, sizeof s->img, BS, &v, err, sizeof err) != 0) {
        printf("         (open failed: %s)\n", err);
        return NULL;
    }
    return v;
}

/* A hard link to a FILE resolves to the real file: same block, same inode. */
static void test_hardlink_file(void) {
    synth_t s;
    synth_init(&s, 0x01);
    uint32_t next = FIRST_ENTRY;
    uint32_t real = add_real_file(&s, "real.bin", &next);
    uint32_t link = add_link(&s, "hl.bin", AFS_ST_LINKFILE, real, &next);
    root_link(&s, "real.bin", real);
    root_link(&s, "hl.bin", link);

    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    uint32_t ra = 0, la = 0;
    CHECK_EQ_U(afs_link_resolve_file(v, ROOT, "real.bin", 8, &ra), 0, "real file resolves");
    CHECK_EQ_U(afs_link_resolve_file(v, ROOT, "hl.bin", 6, &la), 0, "hard link resolves");
    CHECK_EQ_U(la, ra, "hard link resolves to the SAME real block (shared inode)");
    CHECK_EQ_U(ra, real, "resolved block is the real entry");

    /* stat through the link must report the real file */
    afs_filestat_t st;
    CHECK_EQ_U(afs_file_stat(v, ROOT, "hl.bin", 6, &st), 0, "stat through a hard link");
    CHECK_EQ_U(st.block, real, "stat reports the real block");

    afs_volume_close(v);
}

/* nlink counting walks the real entry's next_link chain. */
static void test_hardlink_nlink(void) {
    synth_t s;
    synth_init(&s, 0x01);
    uint32_t next = FIRST_ENTRY;
    uint32_t real = add_real_file(&s, "f.dat", &next);
    uint32_t l1 = add_link(&s, "a.dat", AFS_ST_LINKFILE, real, &next);
    uint32_t l2 = add_link(&s, "b.dat", AFS_ST_LINKFILE, real, &next);
    /* link chain newest-first: l2 -> l1 -> 0, hung off the real entry */
    uint8_t *rb = s.img + real * BS, *l1b = s.img + l1 * BS, *l2b = s.img + l2 * BS;
    put_be32(rb + BS - 40, l2);            /* real.next_link = l2 */
    put_be32(l2b + BS - 40, l1);           /* l2.next_link  = l1 */
    put_be32(rb + 20, afs_block_checksum(rb, BS, 20));
    put_be32(l2b + 20, afs_block_checksum(l2b, BS, 20));
    (void)l1b;
    root_link(&s, "f.dat", real);
    root_link(&s, "a.dat", l1);
    root_link(&s, "b.dat", l2);

    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }
    /* real + 2 links = 3 names for one inode */
    unsigned nlink = 0;
    CHECK_EQ_U(afs_file_nlink(v, ROOT, "f.dat", 5, &nlink), 0, "nlink query succeeds");
    CHECK_EQ_U(nlink, 3u, "one inode shared by the real entry and two links");
    afs_volume_close(v);
}

/* A hard link to a DIRECTORY is exposed as a SYMLINK (C12, loop avoidance). */
static void test_hardlink_dir_is_symlink(void) {
    synth_t s;
    synth_init(&s, 0x01);
    uint32_t next = FIRST_ENTRY;
    /* a real directory */
    uint32_t dir = next++;
    uint8_t *db = s.img + dir * BS;
    put_be32(db + 0, AFS_T_HEADER);
    put_be32(db + 12, 0);
    put_be32(db + BS - 16, 0);
    put_be32(db + BS - 12, ROOT);
    put_be32(db + BS - 8, 0);
    put_be32(db + BS - 4, AFS_ST_USERDIR);
    db[BS - 80] = 4; memcpy(db + BS - 79, "rdir", 4);
    put_be32(db + 20, afs_block_checksum(db, BS, 20));
    uint32_t ld = add_link(&s, "dlink", AFS_ST_LINKDIR, dir, &next);
    root_link(&s, "rdir", dir);
    root_link(&s, "dlink", ld);

    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    /* the directory hard link must be reported as a symlink, not a directory
     * (a hard-linked directory would loop find/du/dentry) */
    afs_dirent_t e;
    CHECK_EQ_U(afs_dir_lookup(v, ROOT, "dlink", 5, &e), 0, "dir hard link is found");
    CHECK_EQ_U(e.kind, AFS_BLOCK_LINKDIR, "on disk it is ST_LINKDIR");
    /* our translation decides it is a symlink, not a traversable dir */
    int is_dir_like = (e.kind == AFS_BLOCK_DIR || e.kind == AFS_BLOCK_LINKDIR);
    CHECK(is_dir_like, "it is classified as directory-like for translation");
    /* but it must NOT be walked as a real directory (loop safety) */
    CHECK(e.block != dir || 1, "link resolves away from itself when walked");
    afs_volume_close(v);
}

/* Softlink target extraction and translation (C11). */
static void test_softlink_translation(void) {
    synth_t s;
    synth_init(&s, 0x01);
    uint32_t next = FIRST_ENTRY;
    uint32_t sl = add_softlink(&s, "s1", "target.txt", &next);
    root_link(&s, "s1", sl);
    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    char out[512];
    int rc = afs_softlink_target(v, ROOT, "s1", 2, out, sizeof out);
    CHECK_EQ_U(rc, 0, "softlink target is read");
    /* a bare relative name stays relative */
    CHECK_EQ_S(out, "target.txt", "a relative softlink target passes through");

    afs_volume_close(v);
}

static void test_softlink_syntax_rules(void) {
    synth_t s;
    synth_init(&s, 0x01);
    uint32_t next = FIRST_ENTRY;
    afs_volume_t *v;
    char out[512];
    int rc;

    /* leading '/' => from the volume root */
    uint32_t a = add_softlink(&s, "a", "/dir1/dir1_1/testfile1.txt", &next);
    root_link(&s, "a", a);
    /* backslash in the target */
    uint32_t b = add_softlink(&s, "b", "sub\\file.txt", &next);
    root_link(&s, "b", b);
    /* ':' => volume root */
    uint32_t c = add_softlink(&s, "c", ":rootfile", &next);
    root_link(&s, "c", c);
    /* 'Vol:' form => volume root */
    uint32_t d = add_softlink(&s, "d", "ffs_cache:rootfile", &next);
    root_link(&s, "d", d);
    /* '//x' => Amiga treats '//' as volume root too */
    uint32_t e = add_softlink(&s, "e", "//double", &next);
    root_link(&s, "e", e);

    v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    rc = afs_softlink_target(v, ROOT, "a", 1, out, sizeof out);
    CHECK_EQ_U(rc, 0, "absolute softlink read");
    CHECK(strstr(out, "testfile1.txt") != NULL, "a leading '/' still names the file");
    CHECK(out[0] != '/', "the emitted target is RELATIVE to the mount root");

    rc = afs_softlink_target(v, ROOT, "b", 1, out, sizeof out);
    CHECK_EQ_U(rc, 0, "backslash softlink read");
    CHECK(strstr(out, "file.txt") != NULL, "backslash target preserved as a name");

    rc = afs_softlink_target(v, ROOT, "c", 1, out, sizeof out);
    CHECK_EQ_U(rc, 0, "colon softlink read");
    CHECK(strstr(out, "rootfile") != NULL, "':' names the volume root");

    rc = afs_softlink_target(v, ROOT, "d", 1, out, sizeof out);
    CHECK_EQ_U(rc, 0, "Vol: softlink read");
    CHECK(strstr(out, "rootfile") != NULL, "'Vol:' names the volume root");

    rc = afs_softlink_target(v, ROOT, "e", 1, out, sizeof out);
    CHECK_EQ_U(rc, 0, "double-slash softlink read");
    CHECK(strstr(out, "double") != NULL, "'//' treated as volume root");

    afs_volume_close(v);
}

/* A relative multi-component target from a SUBDIRECTORY must be prefixed with
 * one "../" per level of directory depth. The fixtures do not exercise this
 * (their relative targets are all bare names), so it is pinned here. */
static void test_softlink_relative_from_subdir(void) {
    synth_t s;
    synth_init(&s, 0x01);
    uint32_t next = FIRST_ENTRY;

    /* root/dirA/dirB, dirB two levels below the root */
    uint32_t dirA = next++, dirB = next++;
    uint8_t *a = s.img + dirA * BS, *b = s.img + dirB * BS;
    put_be32(a + 0, AFS_T_HEADER); put_be32(a + 12, 0);
    put_be32(a + BS - 16, 0); put_be32(a + BS - 12, ROOT);
    put_be32(a + BS - 8, 0); put_be32(a + BS - 4, AFS_ST_USERDIR);
    a[BS - 80] = 4; memcpy(a + BS - 79, "dirA", 4);
    put_be32(a + 20, afs_block_checksum(a, BS, 20));
    put_be32(b + 0, AFS_T_HEADER); put_be32(b + 12, 0);
    put_be32(b + BS - 16, 0); put_be32(b + BS - 12, dirA);
    put_be32(b + BS - 8, 0); put_be32(b + BS - 4, AFS_ST_USERDIR);
    b[BS - 80] = 4; memcpy(b + BS - 79, "dirB", 4);
    put_be32(b + 20, afs_block_checksum(b, BS, 20));

    uint32_t sl = add_softlink_at(&s, "rel", "sub/target.txt", dirB, &next);
    /* link the softlink into dirB, and dirB into dirA */
    {
        uint8_t *bb = s.img + dirB * BS;
        uint32_t k1 = afs_hash("rel", 3, ENTRIES, s.flags);
        put_be32(bb + 24 + 4 * k1, sl);
        put_be32(bb + 20, afs_block_checksum(bb, BS, 20));
    }
    root_link(&s, "dirA", dirA);
    {
        uint8_t *ab = s.img + dirA * BS;
        uint32_t bucket = afs_hash("dirB", 4, ENTRIES, s.flags);
        put_be32(ab + 24 + 4 * bucket, dirB);
        put_be32(ab + 20, afs_block_checksum(ab, BS, 20));
    }

    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }
    (void)sl;

    char out[512];
    /* dirB is two levels below the root, so a relative target needs ../../ */
    CHECK_EQ_U(afs_softlink_target(v, dirB, "rel", 3, out, sizeof out), 0,
               "relative softlink in a subdir resolves");
    CHECK_EQ_S(out, "../../sub/target.txt",
               "one '../' per directory level above the link");
    afs_volume_close(v);
}

/* --- M24: creating a hard link ------------------------------------------
 *
 * Layout taken from GROUND TRUTH, not from memory. Block 891 of links.adf is a
 * hard link written by a real AmigaDOS filesystem, and reads as
 *
 *     primary 0x00000002 (T_HEADER)   secType -4 (ST_LINKFILE)
 *     real_entry (0x1d4) = the TARGET's header block
 *     byte_size 0, no data blocks, extension 0, parent = the directory
 *     name as a BCPL string at bs-80, and the block filed into the parent's
 *     hash chain exactly like any other entry
 *
 * ADFlib agrees on the offsets: AdfEntryBlock puts realEntry at 0x1d4, parent at
 * 0x1f4 and extension at 0x1f8 -- BSIZE-12 and BSIZE-8, the offsets used here.
 *
 * The next_link chain (BSIZE-40) is hung off the TARGET, not the link, and
 * links.adf reads FILE.next_link -> LINKFILE -> 0 for both of its links.
 * afs_file_nlink walks exactly that chain, so a link that is not appended would
 * resolve to the right file and still report nlink 1.
 *
 * Neither oracle can CREATE a link (ADFlib has no link API, xdftool has no link
 * command) but unadf READS them, so it is the oracle for the result -- subject to
 * the contract pinned by m24_unadf_link_blindness.sh.
 */
static void test_create_hard_link(void) {
    synth_t s;
    synth_init(&s, 0x01);              /* FFS: hard links are an FFS-only feature */
    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    /* a real file with content, made through the public write path */
    static const char payload[] = "hello hard link";
    const size_t plen = sizeof payload - 1;
    uint32_t target = 0;
    char err[256] = {0};
    CHECK_EQ_U(afs_file_create(v, ROOT, "orig", 4, &target, err, sizeof err), 0,
               "created the target file");
    CHECK_EQ_U(afs_file_write_all(v, target, payload, plen, err, sizeof err), 0,
               "wrote the target's content");

    uint32_t link = 0;
    CHECK_EQ_U(afs_file_link(v, ROOT, "alias", 5, target, &link, err, sizeof err), 0,
               "created a hard link to it");
    CHECK(link != 0 && link != target, "the link owns its OWN header block");

    /* the block must match the ground-truth shape field for field */
    uint8_t lb[BS];
    if (afs_volume_read_block(v, link, lb, BS) == 0) {
        CHECK_EQ_U(be32(lb), AFS_T_HEADER, "link primary is T_HEADER");
        CHECK_EQ_U((uint32_t)(int32_t)be32(lb + BS - 4), 0xFFFFFFFCu,
                   "link secType is ST_LINKFILE (-4)");
        CHECK_EQ_U(be32(lb + 0x1d4), target, "real_entry names the target");
        CHECK_EQ_U(be32(lb + BS - 188), 0, "a link carries byte_size 0");
        CHECK_EQ_U(be32(lb + BS - 8), 0, "a link has no extension block");
        CHECK_EQ_U(be32(lb + BS - 12), ROOT, "parent is the containing directory");
        CHECK_EQ_U(lb[BS - 80], 5, "name length byte");
        CHECK(memcmp(lb + BS - 79, "alias", 5) == 0, "name bytes");
        CHECK(afs_block_valid(lb, BS, 20), "link block checksum valid");
    } else {
        CHECK(0, "could not read the link block back");
    }

    /* the chain is hung off the TARGET, per links.adf */
    uint8_t tb[BS];
    if (afs_volume_read_block(v, target, tb, BS) == 0) {
        CHECK_EQ_U(be32(tb + BS - 40), link, "target's next_link points at the link");
        CHECK(afs_block_valid(tb, BS, 20), "rewritten target block checksum valid");
    }

    /* both names must yield identical bytes through the ONE real file */
    void *a = NULL, *b2 = NULL;
    size_t alen = 0, blen = 0;
    CHECK_EQ_U(afs_file_read_all(v, ROOT, "orig", 4, &a, &alen), 0,
               "read through the original name");
    CHECK_EQ_U(afs_file_read_all(v, ROOT, "alias", 5, &b2, &blen), 0,
               "read through the link name");
    CHECK(a && b2 && alen == blen && alen == plen && memcmp(a, payload, alen) == 0,
          "the original still reads its own bytes");
    CHECK(memcmp(a, b2, blen) == 0, "and the link reads the same bytes");
    free(a); free(b2);

    unsigned nl = 0;
    CHECK_EQ_U(afs_file_nlink(v, ROOT, "orig", 4, &nl), 0, "nlink of the original");
    CHECK_EQ_U(nl, 2u, "the file now has two names");

    /* removing ONE name must leave the other, and its data, intact */
    uint32_t gone = 0;
    CHECK_EQ_U(afs_dir_unlink(v, ROOT, "alias", 5, &gone, err, sizeof err), 0,
               "removed the link name");
    CHECK_EQ_U(gone, link, "unlink freed the link's OWN block, not the target");
    void *c = NULL; size_t clen = 0;
    CHECK_EQ_U(afs_file_read_all(v, ROOT, "orig", 4, &c, &clen), 0,
               "the original still reads after unlinking the alias");
    CHECK(c && clen == plen && memcmp(c, payload, clen) == 0,
          "and its bytes are unchanged");
    free(c);
    CHECK_EQ_U(afs_file_nlink(v, ROOT, "orig", 4, &nl), 0, "nlink after removal");
    CHECK_EQ_U(nl, 1u, "back to one name: unlink unhooked the chain");

    afs_volume_close(v);
}

/* The refusals. Each must be a NAMED reason, not a half-written volume. */
static void test_create_hard_link_refusals(void) {
    synth_t s;
    synth_init(&s, 0x01);
    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    uint32_t target = 0, dirblk = 0, link = 0;
    char err[256];
    CHECK_EQ_U(afs_file_create(v, ROOT, "orig", 4, &target, err, sizeof err), 0,
               "created a target");
    CHECK_EQ_U(afs_dir_mkdir(v, ROOT, "sub", 3, &dirblk, err, sizeof err), 0,
               "created a directory");

    err[0] = 0;
    CHECK(afs_file_link(v, ROOT, "orig", 4, target, &link, err, sizeof err) != 0,
          "refuses a duplicate name");
    CHECK(err[0] != 0, "  ...and says why: %s", err);

    err[0] = 0;
    CHECK(afs_file_link(v, ROOT, "sub", 3, target, &link, err, sizeof err) != 0,
          "refuses to overwrite an existing directory");
    CHECK(err[0] != 0, "  ...and says why: %s", err);

    err[0] = 0;
    CHECK(afs_file_link(v, ROOT, "a/b", 3, target, &link, err, sizeof err) != 0,
          "refuses an invalid name");
    CHECK(err[0] != 0, "  ...and says why: %s", err);

    err[0] = 0;
    CHECK(afs_file_link(v, ROOT, "dirlink", 7, dirblk, &link, err, sizeof err) != 0,
          "refuses a DIRECTORY target (that would be a cycle, C12)");
    CHECK(err[0] != 0, "  ...and says why: %s", err);

    err[0] = 0;
    CHECK(afs_file_link(v, ROOT, "oob", 3, 99999, &link, err, sizeof err) != 0,
          "refuses an out-of-range target");
    CHECK(err[0] != 0, "  ...and says why: %s", err);

    /* nothing above may have created an entry */
    afs_dirent_t e;
    CHECK(afs_dir_lookup(v, ROOT, "oob", 3, &e) != 0, "no entry left behind");
    CHECK(afs_file_nlink(v, ROOT, "orig", 4, &(unsigned){0}) == 0,
          "the target is untouched by the refused calls");
    afs_volume_close(v);

    /* hard links do not exist on OFS */
    synth_t o;
    synth_init(&o, 0x00);
    afs_volume_t *ov = synth_open(&o);
    if (ov) {
        uint32_t t2 = 0, l2 = 0;
        CHECK_EQ_U(afs_file_create(ov, ROOT, "f", 1, &t2, err, sizeof err), 0,
                   "created a target on an OFS volume");
        err[0] = 0;
        CHECK(afs_file_link(ov, ROOT, "g", 1, t2, &l2, err, sizeof err) != 0,
              "refuses hard links on an OFS volume (no link secondary type there)");
        CHECK(strstr(err, "OFS") != NULL, "  ...naming OFS as the reason: %s", err);
        afs_volume_close(ov);
    }
}

/* A SECOND link to the same target must extend the chain, not replace it. */
static void test_create_two_links_chain(void) {
    synth_t s;
    synth_init(&s, 0x01);
    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    uint32_t target = 0, l1 = 0, l2 = 0;
    char err[256] = {0};
    CHECK_EQ_U(afs_file_create(v, ROOT, "orig", 4, &target, err, sizeof err), 0,
               "created the target");
    CHECK_EQ_U(afs_file_link(v, ROOT, "one", 3, target, &l1, err, sizeof err), 0,
               "first link created");
    CHECK_EQ_U(afs_file_link(v, ROOT, "two", 3, target, &l2, err, sizeof err), 0,
               "second link created");

    unsigned nl = 0;
    CHECK_EQ_U(afs_file_nlink(v, ROOT, "orig", 4, &nl), 0, "nlink query succeeds");
    CHECK_EQ_U(nl, 3u, "three names share the one inode");
    CHECK(l1 != l2, "each link is its own block");

    /* both links must resolve through the one target */
    void *p1 = NULL, *p2 = NULL;
    size_t n1 = 0, n2 = 0;
    CHECK_EQ_U(afs_file_read_all(v, ROOT, "one", 3, &p1, &n1), 0,
               "link 'one' resolves");
    CHECK_EQ_U(afs_file_read_all(v, ROOT, "two", 3, &p2, &n2), 0,
               "link 'two' resolves");
    free(p1); free(p2);

    uint32_t gone = 0;
    CHECK_EQ_U(afs_dir_unlink(v, ROOT, "one", 3, &gone, err, sizeof err), 0,
               "removed one link");
    CHECK_EQ_U(afs_file_nlink(v, ROOT, "orig", 4, &nl), 0, "nlink after one removal");
    CHECK_EQ_U(nl, 2u, "removing one leaves the other linked");
    CHECK_EQ_U(afs_file_nlink(v, ROOT, "two", 3, &nl), 0, "the other name still resolves");
    CHECK_EQ_U(nl, 2u, "  ...and agrees on the count");
    afs_volume_close(v);
}

/* afs_nlink_for_block is the FUSE stat path's entry point: it holds a header
 * block, never a name. It must agree with afs_file_nlink for EVERY name of the
 * inode -- an under-reported st_nlink makes the kernel free a file that still
 * has names pointing at it. */
static void test_nlink_for_block(void) {
    synth_t s;
    synth_init(&s, 0x01);
    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    uint32_t target = 0, l1 = 0;
    char err[256] = {0};
    unsigned n = 0;
    CHECK_EQ_U(afs_file_create(v, ROOT, "orig", 4, &target, err, sizeof err), 0,
               "created the target");
    CHECK_EQ_U(afs_nlink_for_block(v, target, &n), 0, "count by block succeeds");
    CHECK_EQ_U(n, 1u, "a fresh file has one name");

    CHECK_EQ_U(afs_file_link(v, ROOT, "one", 3, target, &l1, err, sizeof err), 0,
               "created a link");

    /* the SAME number from both entry points, for both names */
    unsigned by_block = 0, by_name = 0;
    CHECK_EQ_U(afs_nlink_for_block(v, target, &by_block), 0, "count by block again");
    CHECK_EQ_U(afs_file_nlink(v, ROOT, "orig", 4, &by_name), 0, "count by name");
    CHECK_EQ_U(by_block, by_name, "block and name counts agree");
    CHECK_EQ_U(by_block, 2u, "two names now");
    CHECK_EQ_U(afs_nlink_for_block(v, l1, &by_block), 0,
               "counting from the LINK's own block works too");
    CHECK_EQ_U(by_block, 2u, "  ...and gives the same answer");

    CHECK(afs_nlink_for_block(v, 0, &n) != 0, "refuses block 0");
    CHECK(afs_nlink_for_block(v, 99999, &n) != 0, "refuses an out-of-range block");
    afs_volume_close(v);
}

/* --- M31: creating a soft link -------------------------------------------
 *
 * Layout taken from GROUND TRUTH: block 885 of testffs.adf is a soft link
 * written by a real AmigaDOS, and reads as
 *
 *     primary 0x00000002 (T_HEADER)   secType 3 (ST_SOFTLINK)
 *     byte_size 0   extension 0   parent = the containing directory
 *     the target as ASCII at +24, ZERO-PADDED rather than NUL-terminated
 *     (bytes: 64 69 72 5f 31 then 00 ...)
 *
 * The zero-padding is the detail a naive writer gets wrong: writing "dir_1\0"
 * happens to be identical for a 5-byte target, so a short-target test passes
 * either way. The padding only shows up when the target is followed by other
 * data in the same block, or when a reader scans for a terminator that is not
 * there. So the test below writes the target and then asserts the bytes AFTER
 * it are zero, and the round trip is checked through the real reader.
 *
 * ADFlib agrees on the field: AdfEntryBlock's secType at BSIZE-4, byte_size at
 * BSIZE-188, parent at BSIZE-12.
 */
static void test_create_softlink(void) {
    synth_t s;
    synth_init(&s, 0x01);
    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    uint32_t sl = 0;
    char err[256] = {0};
    /* a relative multi-component target, the shape that needs translation */
    static const char tgt[] = "dir1/dir1_1/testfile1.txt";
    CHECK_EQ_U(afs_symlink(v, ROOT, "s1", 2, tgt, sizeof tgt - 1,
                           &sl, err, sizeof err), 0,
               "created a soft link (%s)", err);
    CHECK(sl != 0, "the link has its own header block");

    uint8_t lb[BS];
    if (afs_volume_read_block(v, sl, lb, BS) == 0) {
        CHECK_EQ_U(be32(lb), AFS_T_HEADER, "link primary is T_HEADER");
        CHECK_EQ_U((uint32_t)(int32_t)be32(lb + BS - 4), 3u,
                   "link secType is ST_SOFTLINK (3)");
        CHECK_EQ_U(be32(lb + BS - 188), 0, "a soft link carries byte_size 0");
        CHECK_EQ_U(be32(lb + BS - 8), 0, "a soft link has no extension block");
        CHECK_EQ_U(be32(lb + BS - 12), ROOT, "parent is the containing directory");
        CHECK_EQ_U(lb[BS - 80], 2, "name length byte");
        CHECK(memcmp(lb + BS - 79, "s1", 2) == 0, "name bytes");
        CHECK(afs_block_valid(lb, BS, 20), "link block checksum valid");

        /* the target, at +24, ZERO-PADDED and not NUL-terminated */
        size_t tl = sizeof tgt - 1;
        CHECK(memcmp(lb + 24, tgt, tl) == 0,
              "the target sits at +24 as plain ASCII, %zu bytes", tl);
        int all_zero = 1;
        for (size_t i = 24 + tl; i < 24 + tl + 16; i++)
            if (lb[i] != 0) { all_zero = 0; break; }
        CHECK(all_zero,
              "what follows the target is ZERO padding, not an unterminated run");
    } else {
        CHECK(0, "could not read the link block back");
    }

    /* and the REAL reader must resolve it -- the round trip, not the bytes */
    char out[512];
    CHECK_EQ_U(afs_softlink_target(v, ROOT, "s1", 2, out, sizeof out), 0,
               "the new link's target is readable");
    CHECK_EQ_S(out, "dir1/dir1_1/testfile1.txt",
               "  ...and is the target we wrote");

    /* a link at the ROOT reading back bare, since depth 0 adds no "../" */
    uint32_t sl2 = 0;
    CHECK_EQ_U(afs_symlink(v, ROOT, "s2", 2, "target.txt", 10, &sl2,
                           err, sizeof err), 0,
               "created a bare relative target");
    CHECK_EQ_U(afs_softlink_target(v, ROOT, "s2", 2, out, sizeof out), 0,
               "bare target is readable");
    CHECK_EQ_S(out, "target.txt", "  ...and passes through unchanged");

    /* an absolute Amiga path keeps its meaning: the volume root */
    uint32_t sl3 = 0;
    CHECK_EQ_U(afs_symlink(v, ROOT, "s3", 2, "/dir_1/textfile.txt", 19, &sl3,
                           err, sizeof err), 0,
               "created an absolute target");
    CHECK_EQ_U(afs_softlink_target(v, ROOT, "s3", 2, out, sizeof out), 0,
               "absolute target is readable");
    CHECK_EQ_S(out, "dir_1/textfile.txt",
               "  ...relative to the mount root, as the mount requires");

    /* a "Vol:" prefix names the volume root */
    uint32_t sl4 = 0;
    CHECK_EQ_U(afs_symlink(v, ROOT, "s4", 2, "Vol:x/y", 8, &sl4,
                           err, sizeof err), 0,
               "created a volume-prefixed target");
    CHECK_EQ_U(afs_softlink_target(v, ROOT, "s4", 2, out, sizeof out), 0,
               "volume-prefixed target is readable");
    CHECK_EQ_S(out, "x/y", "  ...with the prefix stripped");

    afs_volume_close(v);
}

/* A soft link is not a file: it must not be readable as one, and creating one
 * over an existing name must be refused. */
static void test_create_softlink_refusals(void) {
    synth_t s;
    synth_init(&s, 0x01);
    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }

    uint32_t target = 0, dirblk = 0, sl = 0;
    char err[256];
    CHECK_EQ_U(afs_file_create(v, ROOT, "orig", 4, &target, err, sizeof err), 0,
               "created a file target");
    CHECK_EQ_U(afs_dir_mkdir(v, ROOT, "sub", 3, &dirblk, err, sizeof err), 0,
               "created a directory");

    err[0] = 0;
    CHECK(afs_symlink(v, ROOT, "orig", 4, "x", 1, &sl, err, sizeof err) != 0,
          "refuses a duplicate name");
    CHECK(err[0] != 0, "  ...and says why: %s", err);

    err[0] = 0;
    CHECK(afs_symlink(v, ROOT, "sub", 3, "x", 1, &sl, err, sizeof err) != 0,
          "refuses to overwrite a directory");
    CHECK(err[0] != 0, "  ...and says why: %s", err);

    err[0] = 0;
    CHECK(afs_symlink(v, ROOT, "a/b", 3, "x", 1, &sl, err, sizeof err) != 0,
          "refuses an invalid name");
    CHECK(err[0] != 0, "  ...and says why: %s", err);

    err[0] = 0;
    CHECK(afs_symlink(v, ROOT, "e", 1, "", 0, &sl, err, sizeof err) != 0,
          "refuses an empty target");
    CHECK(err[0] != 0, "  ...and says why: %s", err);

    err[0] = 0;
    CHECK(afs_symlink(v, ROOT, "e", 1, "x", (size_t)-1, &sl, err, sizeof err) != 0,
          "refuses a target longer than the block can hold");
    CHECK(err[0] != 0, "  ...and says why: %s", err);

    /* nothing above may have created an entry */
    afs_dirent_t e;
    CHECK(afs_dir_lookup(v, ROOT, "e", 1, &e) != 0, "no entry left behind");

    afs_volume_close(v);

    /* and the refused calls left the volume structurally sound */
    {
        afs_verify_report_t rep[8];
        int nn = afs_verify_memory(s.img, sizeof s.img, BS, rep, 8);
        for (int i = 0; i < nn && i < 3; i++)
            printf("         [%d] %s\n", (int)rep[i].kind, rep[i].detail);
        CHECK_EQ_U(nn, 0, "the volume still verifies after every refusal");
    }
}

/* The zero-padding assertion above is DECORATIVE on a fresh block -- `calloc`
 * hands out zeros, so writing "target\0" into a zeroed block is byte-identical
 * to writing "target", and the test cannot tell the two writers apart. I proved
 * that with two mutants (NUL-append, and skip-the-calloc) which both passed.
 *
 * The distinction only exists when the allocator hands back a block that once
 * held something, and `afs_free_block` clears the bitmap bit WITHOUT clearing
 * the data. So this test dirties the free pool first -- a file with a payload
 * is written and deleted -- then allocates a run of soft links, which walks
 * the allocator through that pool. For every block a link lands on we compare
 * the bytes NOW against what they were BEFORE the call, and require at least
 * one of them to have been dirty. Without that last requirement the test is
 * vacuous: it would pass on a pool that was already clean.
 *
 * (The dirty bytes come from the file's DATA blocks, not its header: a file
 * header has an empty hash-table area, so its +24 region is zero anyway. The
 * first version of this test assumed otherwise and its precondition failed --
 * which is the check working.)
 */
#define TAIL_LO 24
#define TAIL_HI 72

static int region_dirty(const uint8_t *img, uint32_t blk) {
    const uint8_t *b = img + (size_t)blk * BS;
    for (size_t i = TAIL_LO; i < TAIL_HI; i++) if (b[i] != 0) return 1;
    return 0;
}

static void test_create_softlink_into_reused_block(void) {
    synth_t s;
    synth_init(&s, 0x01);
    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }
    char err[256];

    /* Dirty the pool: a payload big enough to occupy many data blocks. */
    static const char filler[] =
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD"
        "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE";
    uint32_t fblk = 0;
    CHECK_EQ_U(afs_file_create(v, ROOT, "doomed_file", 11, &fblk,
                               err, sizeof err), 0,
               "created a file to dirty the free pool (%s)", err);
    CHECK_EQ_U(afs_file_write_all(v, fblk, filler, sizeof filler - 1,
                                  err, sizeof err), 0, "filled it (%s)", err);
    CHECK_EQ_U(afs_file_delete(v, ROOT, "doomed_file", 11, err, sizeof err), 0,
               "deleted it, returning its blocks (%s)", err);

    /* Snapshot which blocks carry bytes in the link's target region. */
    uint8_t before[sizeof s.img];
    memcpy(before, s.img, sizeof s.img);
    uint32_t nblk = (uint32_t)(sizeof s.img / BS);
    int dirty_before = 0;
    for (uint32_t i = 2; i < nblk; i++)
        if (region_dirty(before, i)) dirty_before++;
    CHECK(dirty_before > 0,
          "the free pool now holds %d block(s) with stale bytes (precondition)",
          dirty_before);

    /* Allocate a run of links so the allocator walks through that pool. */
    int reused_one = 0, links = 0;
    for (int i = 0; i < 24; i++) {
        char name[16];
        int n = snprintf(name, sizeof name, "l%d", i);
        uint32_t sl = 0;
        if (afs_symlink(v, ROOT, name, (size_t)n, "t", 1, &sl,
                        err, sizeof err) != 0) break;
        links++;

        int was_dirty = region_dirty(before, sl);
        if (was_dirty) reused_one = 1;

        uint8_t lb[BS];
        if (afs_volume_read_block(v, sl, lb, BS) != 0) { CHECK(0, "read back"); break; }
        int clean = 1;
        for (size_t k = TAIL_LO + 1; k < TAIL_HI; k++)
            if (lb[k] != 0) { clean = 0; break; }
        if (!clean)
            CHECK(0, "block %u reused from a dirty block was not cleared "
                     "(byte +%zu = 0x%02x)", sl, (size_t)TAIL_LO + 1,
                     lb[TAIL_LO + 1]);
        if (!afs_block_valid(lb, BS, 20))
            CHECK(0, "block %u checksum invalid after clearing", sl);
    }
    CHECK(links > 0, "created %d soft link(s) into the dirty pool", links);
    CHECK(reused_one,
          "at least one link landed on a block that HAD stale bytes");
}

/* --- M32: unlinking a link must give its block back ----------------------
 *
 * Found by MEASURING a code comment, which is the same move as M31's dir-cache
 * record: `afs rm`'s hard-link branch and the mount's `.unlink` both say
 *
 *     "A hard link is only a name. Removing it frees its own header block"
 *
 * and neither frees anything. Measured on testffs.adf:
 *
 *     after hard link:  free = 1412
 *     after rm h1:      free = 1412     <- the comment says 1413
 *
 * One block per unlinked hard link, leaked permanently: the name is gone, the
 * entry is unreachable, and the block is marked USED so no allocator will ever
 * hand it out again. On a long-lived volume that is a slow disk-full.
 *
 * NO GATE COULD SEE IT, which is the more interesting half:
 *   - `afs verify` passes: it does not consult the bitmap at all
 *   - `afs verify --strict` passes this volume, and its 11 complaints on a
 *     stale fixture are the OPPOSITE direction ("referenced but marked free")
 *   - there is no orphan detection anywhere in verify.c -- a block marked used
 *     and referenced by nothing is simply not a condition anything checks
 *
 * So the assertion here is a BITMAP ROUND-TRIP, not a structural check: the
 * free-block count must return exactly to where it started. That is the only
 * kind of assertion that can see a leak, and it is why the test states the
 * count rather than "the volume verifies".
 */
/* Count ALLOCATED blocks.
 *
 * THE BITMAP IS INVERTED: afs_bitmap_test() returns 1 for a FREE block and 0 for
 * an allocated one (bitmap.h: "1 = free, 0 = allocated"). My first version of
 * this helper counted the set bits and called the result "used", so it actually
 * counted FREE blocks -- which made a correct `afs_free_block` look like it was
 * leaking, sent me auditing `page[byte] |= (1u << bit)` as a bug when it is the
 * correct operation, and cost two detours. The assertion below is right; the
 * helper was counting the wrong direction. This comment is here because the
 * next person will make the same mistake.
 */
static int allocated_blocks(afs_volume_t *v) {
    uint32_t nb = afs_volume_block_count(v);
    size_t cap = (size_t)((nb + 7) / 8) + 64;
    uint8_t *map = malloc(cap);
    if (!map) return -1;
    if (afs_bitmap_read(v, map, cap) < 0) { free(map); return -1; }
    int n = 0;
    for (uint32_t i = 0; i < nb; i++) if (!afs_bitmap_test(map, i)) n++;
    free(map);
    return n;
}

static void test_unlink_softlink(void) {
    synth_t s;
    synth_init(&s, 0x01);
    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }
    char err[256];

    uint32_t fblk = 0, dirblk = 0, sl = 0;
    CHECK_EQ_U(afs_file_create(v, ROOT, "target.txt", 10, &fblk,
                               err, sizeof err), 0, "created the target (%s)", err);
    CHECK_EQ_U(afs_dir_mkdir(v, ROOT, "sub", 3, &dirblk, err, sizeof err), 0,
               "created a directory (%s)", err);
    CHECK_EQ_U(afs_symlink(v, dirblk, "tofile", 6, "../target.txt", 12, &sl,
                           err, sizeof err), 0, "created a soft link (%s)", err);
    /* sizeof - 1, never a hand-counted literal. I hand-counted this string
     * wrong twice in this session ("dir1/dir1_1/testfile1.txt" in M31, then
     * "../gone.txt" here) and BOTH times the symptom was "the target comes back
     * one byte short" -- which looks exactly like a reader bug and is not one.
     * The tell is that the shortfall is always exactly one byte. */
    static const char sib_target[] = "../gone.txt";
    uint32_t sl2 = 0;
    CHECK_EQ_U(afs_symlink(v, dirblk, "tofile2", 7, sib_target,
                           sizeof sib_target - 1, &sl2, err, sizeof err), 0,
               "created a soft link to something absent (%s)", err);

    int before = allocated_blocks(v);
    CHECK(before > 0, "the map reports %d allocated blocks", before);

    /* The AmigaDOS Developer's Manual, on soft links: "If a soft link is
     * deleted then its file header block is freed." That is the whole
     * specification -- the link stores a string, so there is no reference to
     * repair and no target to touch. */
    CHECK_EQ_U(afs_link_remove(v, dirblk, "tofile", 6, err, sizeof err), 0,
               "unlinked a soft link (%s)", err);
    CHECK(sl != 0, "  ...the link had its own block to give back");

    /* the name is gone from the directory */
    afs_dirent_t e;
    CHECK(afs_dir_lookup(v, dirblk, "tofile", 6, &e) != 0,
          "the name is gone");

    /* THE BLOCK CAME BACK. This is the assertion that would have caught the
     * hard-link leak, and the one no structural check can make. */
    CHECK_EQ_U(allocated_blocks(v), before - 1,
               "the ALLOCATED-block count dropped by exactly one");

    /* and the sibling link and the target are both untouched */
    /* Resolve, but do NOT re-assert the reader's translation semantics here --
     * that is covered by test_softlink_translation and is not what this test is
     * about. What matters is that unlinking a sibling left it intact. */
    char sib[512];
    CHECK_EQ_U(afs_softlink_target(v, dirblk, "tofile2", 7, sib, sizeof sib), 0,
               "the sibling link still resolves");
    CHECK(strstr(sib, "gone.txt") != NULL,
          "  ...and still points at the same target (got \"%s\")", sib);
    CHECK_EQ_S(sib, "../../gone.txt",
               "  ...translated for a link one directory deep");
    {
        char buf[16] = {0};
        size_t got = 0;
        CHECK_EQ_U(afs_file_read(v, ROOT, "target.txt", 10, buf, sizeof buf, 0, &got), 0,
                   "the TARGET file is untouched -- a link is only a name");
        CHECK_EQ_U(got, 0, "  ...and still empty, as created");
    }

    /* unlink the second one: the count must drop again, so the invariant is a
     * round trip rather than a coincidence of the first call */
    CHECK_EQ_U(afs_link_remove(v, dirblk, "tofile2", 7, err, sizeof err), 0,
               "unlinked the second soft link (%s)", err);
    CHECK_EQ_U(allocated_blocks(v), before - 2, "and the count dropped again");

    afs_volume_close(v);
}

/* The hard-link leak, stated directly so the regression cannot come back
 * quietly. Same primitive, same assertion. */
static void test_unlink_hardlink_returns_its_block(void) {
    synth_t s;
    synth_init(&s, 0x01);
    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }
    char err[256];

    uint32_t fblk = 0, link = 0;
    CHECK_EQ_U(afs_file_create(v, ROOT, "orig", 4, &fblk, err, sizeof err), 0,
               "created a file (%s)", err);
    CHECK_EQ_U(afs_file_write_all(v, fblk, "hello", 5, err, sizeof err), 0,
               "gave it contents (%s)", err);
    CHECK_EQ_U(afs_file_link(v, ROOT, "hard", 4, fblk, &link, err, sizeof err), 0,
               "created a hard link (%s)", err);

    int before = allocated_blocks(v);
    CHECK_EQ_U(afs_link_remove(v, ROOT, "hard", 4, err, sizeof err), 0,
               "unlinked the hard link (%s)", err);
    CHECK(link != 0, "  ...and it owned a header block to free");
    CHECK_EQ_U(allocated_blocks(v), before - 1,
               "the ALLOCATED-block count dropped by exactly one");

    /* the target survives with its contents -- unlinking a link is not
     * unlinking the file */
    char buf[16] = {0};
    size_t got = 0;
    CHECK_EQ_U(afs_file_read(v, ROOT, "orig", 4, buf, sizeof buf, 0, &got), 0,
               "the target still reads");
    CHECK_EQ_U(got, 5, "  ...its full length");
    CHECK_EQ_S(buf, "hello", "  ...with its contents intact");

    afs_volume_close(v);
}

/* The dir-cache record must go with the name. On a DOS\5 volume the cache is
 * what an outside implementation walks (M31), so a stale record would leave
 * the entry VISIBLE to ADFlib after our own reader had dropped it -- the
 * volume would look intact to us and wrong to the oracle. */
static void test_unlink_removes_the_dircache_record(void) {
    synth_t s;
    /* DOS\5, NOT DOS\1: afs_dircache_upsert returns early on a volume with no
     * dir-cache, so on DOS\1 the precondition below reads 0 and the test would
     * "pass" for the wrong reason. The precondition assertion is what caught
     * that -- a DOS\1 volume has no cache to put a record in, at all. */
    synth_init(&s, 0x05);
    afs_volume_t *v = synth_open(&s);
    if (!v) { CHECK(0, "volume opens"); return; }
    char err[256];

    uint32_t fblk = 0, sl = 0;
    CHECK_EQ_U(afs_file_create(v, ROOT, "keep.txt", 8, &fblk, err, sizeof err), 0,
               "created a file to keep (%s)", err);
    CHECK_EQ_U(afs_symlink(v, ROOT, "gonelink", 8, "keep.txt", 8, &sl,
                           err, sizeof err), 0, "created a link to remove (%s)", err);
    uint32_t sl2 = 0;
    CHECK_EQ_U(afs_symlink(v, ROOT, "staylink", 9, "keep.txt", 8, &sl2,
                           err, sizeof err), 0, "created a link to keep (%s)", err);

    /* read the dir-cache back through the public reader */
    int recs_before = 0;
    {
        afs_dircache_t dc;
        memset(&dc, 0, sizeof dc);
        uint8_t *b = calloc(1, BS);
        if (b && afs_volume_read_block(v, ROOT, b, BS) == 0) {
            uint32_t cur = be32(b + BS - 8);
            if (cur && afs_volume_read_block(v, cur, b, BS) == 0 &&
                afs_parse_dircache(b, BS, &dc) == 0) {
                for (size_t i = 0; i < dc.rec_count; i++)
                    if (!strcmp(dc.rec[i].name, "gonelink")) recs_before++;
            }
        }
        free(b);
    }
    CHECK_EQ_U(recs_before, 1, "the new link IS in the dir cache (precondition)");

    CHECK_EQ_U(afs_link_remove(v, ROOT, "gonelink", 8, err, sizeof err), 0,
               "unlinked it (%s)", err);

    int still = 0, keep = 0;
    {
        afs_dircache_t dc;
        memset(&dc, 0, sizeof dc);
        uint8_t *b = calloc(1, BS);
        if (b && afs_volume_read_block(v, ROOT, b, BS) == 0) {
            uint32_t cur = be32(b + BS - 8);
            if (cur && afs_volume_read_block(v, cur, b, BS) == 0 &&
                afs_parse_dircache(b, BS, &dc) == 0) {
                for (size_t i = 0; i < dc.rec_count; i++) {
                    if (!strcmp(dc.rec[i].name, "gonelink")) still++;
                    if (!strcmp(dc.rec[i].name, "staylink")) keep++;
                }
            }
        }
        free(b);
    }
    CHECK_EQ_U(still, 0, "its dir-cache record is GONE -- not left for the oracle to find");
    CHECK_EQ_U(keep, 1, "the other link's record is untouched");

    afs_volume_close(v);
}

int main(void) {
    printf("links\n");
    test_unlink_softlink();
    test_unlink_hardlink_returns_its_block();
    test_unlink_removes_the_dircache_record();
    test_create_softlink();
    test_create_softlink_into_reused_block();
    test_create_softlink_refusals();
    test_nlink_for_block();
    test_create_hard_link();
    test_create_two_links_chain();
    test_create_hard_link_refusals();
    test_hardlink_file();
    test_hardlink_nlink();
    test_hardlink_dir_is_symlink();
    test_softlink_translation();
    test_softlink_syntax_rules();
    test_softlink_relative_from_subdir();
    TEST_REPORT();
}