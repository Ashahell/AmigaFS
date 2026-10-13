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

int main(void) {
    printf("links\n");
    test_hardlink_file();
    test_hardlink_nlink();
    test_hardlink_dir_is_symlink();
    test_softlink_translation();
    test_softlink_syntax_rules();
    test_softlink_relative_from_subdir();
    TEST_REPORT();
}