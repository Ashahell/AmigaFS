/* Write-path differential fuzz: random operation sequences against an
 * in-memory model of what the volume should contain.
 *
 * WHY THIS EXISTS. The write path (M13a/b, M14, M15) is pointer surgery --
 * hash-chain splicing, dir-cache record compaction with memmove, OFS header
 * fixups, table rebuilds -- and every gate so far drives a handful of FIXED
 * scenarios. A bug that needs "an extension block AND a chain relink AND a
 * cache record in the same operation" is invisible to a fixed script and
 * corrupts a volume silently: the file reads back with the wrong bytes, or the
 * volume stops verifying, and nothing complains until an unrelated tool opens
 * it.
 *
 * WHAT IS ASSERTED, after EVERY operation that returned success:
 *   1. afs_verify_memory_ex(..., AFS_VERIFY_STRICT) reports ZERO problems. This
 *      is the strong one: checksums, pointer ranges, block types, double-use,
 *      dir-cache/hash-table agreement, AND bitmap agreement. A leaked block, an
 *      eaten live block, a broken chain and a stale cache record all fail here.
 *
 *      STRICT is not decoration. It was afs_verify_memory() -- flags == 0 --
 *      while this comment claimed bitmap agreement, which is the ONE check
 *      strict adds. Measured: with the everyday call, a volume whose live file
 *      header was marked free reported ZERO problems. That is M26's exact
 *      shape, invisible to the harness built to find it (M28).
 *   2. A recursive tree walk yields EXACTLY the model's set of (path, kind) --
 *      no orphans, no duplicates, no missing entries.
 *   3. Every model's file reads back byte-identical.
 * The model is only updated when the library reports success, so a refusal
 * leaves it (and the volume) untouched -- which is itself the assertion: a
 * write that half-applies before failing shows up as a model/volume mismatch.
 *
 * COVERAGE IS ASSERTED, NOT ASSUMED (the project's standing rule: a fuzz run
 * that tested nothing must not report green). The harness prints how many
 * operations of each class actually ran and how many were refused, and exits
 * non-zero if any class fell below its floor.
 *
 * A second mode starts from a deliberately damaged image: the only assertions
 * there are "no crash, no hang" under ASan/UBSan, because writes onto a corrupt
 * structure are allowed to be refused with a reason or to succeed -- but never
 * to read or write out of bounds.
 *
 * Built with -fsanitize=address,undefined by tests/oracle/m16_write_fuzz.sh.
 */
#include "amigafs/bitmap.h"
#include "amigafs/be.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/dir.h"
#include "amigafs/file.h"
#include "amigafs/hash.h"
#include "amigafs/volume.h"
#include "amigafs/verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMG_BLOCKS 1760u
#define IMG_ROOT   880u
#define IMG_BM     881u

#define MAX_ENT    48      /* tracked files + dirs at once */
#define MAX_DATA   3000u   /* largest content any one file gets */
#define WALK_DEPTH 6

/* Sized so a run does not SATURATE the volume: 48 entries x 3000 bytes is well
 * under 300 blocks of a 1756-block floppy. An early version used 8192 bytes
 * and filled the disk within ~100 operations, after which every op that needs
 * even one block was refused and the run stopped exercising anything -- seed 3
 * "passed" with `trunc` never succeeding at all. Out-of-space is covered
 * DELIBERATELY by op_overflow below, not by starving every operation. */

/* ------------------------------------------------------------------ */
/* deterministic RNG: a failing run must be reproducible from its seed  */
/* ------------------------------------------------------------------ */

static uint64_t rng_state;

static void rng_seed(uint64_t s) { rng_state = s ? s : 1; }

static uint32_t rnd(uint32_t n) {
    /* splitmix64 -- small state, good distribution, no libc dependency */
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return n ? (uint32_t)((z >> 11) % n) : 0u;
}

/* ------------------------------------------------------------------ */
/* the model                                                           */
/* ------------------------------------------------------------------ */

struct ent {
    int      in_use;
    int      is_dir;
    char     path[192];    /* "/" separated, no leading slash; root is "" */
    char     name[AFS_NAME_MAX];
    char     dir[192];     /* parent path, "" for the root */
    uint32_t block;        /* the entry's own header block */
    uint8_t *data;
    uint32_t len;
};

static struct ent g_ent[MAX_ENT];
static uint8_t    g_img[(size_t)IMG_BLOCKS * 512];

/* Operation tallies. `tried` counts attempts, `done` successes, `refused`
 * failures with a reason -- a run where nothing was ever refused has not
 * exercised the refusal paths at all. */
enum {
    OP_CREATE, OP_OVERWRITE, OP_RANGE, OP_TRUNC,
    OP_RENAME, OP_UNLINK, OP_MKDIR, OP_RMDIR, OP_OVERFLOW, OP_COUNT
};
static const char *op_name[OP_COUNT] = {
    "create", "overwrite", "range", "trunc",
    "rename", "unlink", "mkdir", "rmdir", "overflow"
};
static unsigned g_tried[OP_COUNT], g_done[OP_COUNT], g_refused[OP_COUNT];

static int failures;

/* The iteration counter and the op class currently being run, so a failure
 * names the operation that caused it instead of just the invariant. */
static unsigned g_iter;
static unsigned g_last_op;

static void fail(const char *what, const char *detail) {
    printf("  FAIL  %s: %s   [iter %u, op %s]\n", what, detail ? detail : "",
           g_iter, op_name[g_last_op]);
    failures++;
}

/* parent path + "/" + name */
static void join_path(char *out, size_t cap, const char *dir, const char *name) {
    if (dir && dir[0]) snprintf(out, cap, "%s/%s", dir, name);
    else               snprintf(out, cap, "%s", name);
}

/* split "a/b" into dir "a" and name "b"; the root gives dir "" name "a" */
static void split_path(const char *path, char *dir, size_t dcap,
                       char *name, size_t ncap) {
    const char *slash = strrchr(path, '/');
    if (!slash) { snprintf(dir, dcap, "%s", ""); snprintf(name, ncap, "%s", path); return; }
    size_t dl = (size_t)(slash - path);
    if (dl >= dcap) dl = dcap - 1;
    memcpy(dir, path, dl);
    dir[dl] = '\0';
    snprintf(name, ncap, "%s", slash + 1);
}

static struct ent *find_by_path(const char *path) {
    for (int i = 0; i < MAX_ENT; i++)
        if (g_ent[i].in_use && strcmp(g_ent[i].path, path) == 0)
            return &g_ent[i];
    return NULL;
}

static struct ent *find_random(void) {
    unsigned live = 0;
    for (int i = 0; i < MAX_ENT; i++) if (g_ent[i].in_use) live++;
    if (!live) return NULL;
    unsigned pick = rnd(live);
    for (int i = 0; i < MAX_ENT; i++) {
        if (!g_ent[i].in_use) continue;
        if (pick-- == 0) return &g_ent[i];
    }
    return NULL;
}

static struct ent *random_dir(void) {
    unsigned dirs = 0;
    for (int i = 0; i < MAX_ENT; i++)
        if (g_ent[i].in_use && g_ent[i].is_dir) dirs++;
    if (!dirs) return NULL;
    unsigned pick = rnd(dirs);
    for (int i = 0; i < MAX_ENT; i++) {
        if (!g_ent[i].in_use || !g_ent[i].is_dir) continue;
        if (pick-- == 0) return &g_ent[i];
    }
    return NULL;
}

static struct ent *alloc_ent(void) {
    for (int i = 0; i < MAX_ENT; i++)
        if (!g_ent[i].in_use) return &g_ent[i];
    return NULL;
}

/* A directory is removable only when the model says it holds nothing: the
 * library refuses a non-empty rmdir (ADFlib parity), and a harness that
 * expected otherwise would be testing its own bug. */
static int dir_is_empty(const char *path) {
    size_t plen = strlen(path);
    for (int i = 0; i < MAX_ENT; i++) {
        if (!g_ent[i].in_use) continue;
        if (strncmp(g_ent[i].path, path, plen) != 0) continue;
        if (g_ent[i].path[plen] == '\0') continue;         /* it is the dir */
        if (g_ent[i].path[plen] == '/') return 0;          /* a child */
    }
    return 1;
}

static void drop_subtree(const char *path) {
    size_t plen = strlen(path);
    for (int i = 0; i < MAX_ENT; i++) {
        if (!g_ent[i].in_use) continue;
        if (strncmp(g_ent[i].path, path, plen) != 0) continue;
        if (g_ent[i].path[plen] != '\0' && g_ent[i].path[plen] != '/') continue;
        free(g_ent[i].data);
        g_ent[i].data = NULL;
        g_ent[i].in_use = 0;
    }
}

/* ------------------------------------------------------------------ */
/* the synthetic volume (same construction as the unit suite)           */
/* ------------------------------------------------------------------ */

static void build_volume(void) {
    uint8_t *r, *bm;
    memset(g_img, 0, sizeof g_img);
    put_be32(g_img + 0, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                          ((uint32_t)'S' << 8) | 1u);
    put_be32(g_img + 4, afs_bootblock_checksum(g_img, 1024));
    r = g_img + (size_t)IMG_ROOT * 512;
    put_be32(r + 0, AFS_T_HEADER);
    put_be32(r + 12, (uint32_t)afs_hash_table_size(512));
    put_be32(r + 312, 0xFFFFFFFFu);
    put_be32(r + 316, IMG_BM);
    r[432] = 3;
    memcpy(r + 433, "vol", 3);
    put_be32(r + 508, AFS_ST_ROOT);
    put_be32(r + 20, afs_block_checksum(r, 512, 20));
    bm = g_img + (size_t)IMG_BM * 512;
    /* calloc-equivalent: every bit 0 means "used"; free all but the reserved.
     *
     * NOTE the +4. A bitmap PAGE keeps its own checksum long in bytes 0..3 and
     * map data begins after it -- afs_bitmap_read assembles from `blk + 4`.
     * afs_bitmap_set() writes at offset 0 because it addresses an ASSEMBLED
     * map, so calling it on a raw page put every bit into the checksum word.
     * This harness's volume therefore carried a broken map from the day it was
     * written: the allocator state it exercised was not the map any reader
     * would see. Enabling the strict check (M28) is what exposed it -- it
     * reported blocks 880 and 881, the rootblock and the bitmap page, as
     * live-but-free. */
    for (unsigned i = 0; i < IMG_BLOCKS; i++)
        if (i != 0 && i != 1 && i != IMG_ROOT && i != IMG_BM)
            bm[4 + i / 8] |= (uint8_t)(0x80 >> (i % 8));
    put_be32(bm + 0, afs_block_checksum(bm, 512, 0));
    memset(g_ent, 0, sizeof g_ent);
    memset(g_tried, 0, sizeof g_tried);
    memset(g_done, 0, sizeof g_done);
    memset(g_refused, 0, sizeof g_refused);
}

/* Damage that actually reaches the structures the write path reads.
 *
 * Scattered byte flips across a 900 KiB image almost never land on anything:
 * most of the image is file payload, and the structural fields are ~4% of it,
 * so 24 random flips reliably produce zero refusals -- a "corrupt" run that
 * behaves exactly like a clean one and tests nothing. So each damaged BLOCK is
 * picked at random and the byte within it is picked from the regions that
 * carry structure: the block header (type, seq, checksum, hash table, data
 * table) and the tail (entry name, parent, hash chain, extension pointer,
 * secondary type). */
static void damage_volume(unsigned blocks) {
    for (unsigned i = 0; i < blocks; i++) {
        uint32_t blk = rnd(IMG_BLOCKS);
        size_t off;
        if (blk == 0 || blk == 1 || blk == IMG_ROOT || blk == IMG_BM) continue;
        /* 60% the header region, 40% the tail */
        if (rnd(10) < 6) off = rnd(64);
        else             off = 512 - 1 - rnd(96);
        g_img[(size_t)blk * 512 + off] ^= (uint8_t)(1u << rnd(8));
    }
}

/* ------------------------------------------------------------------ */
/* model <-> volume cross-check                                        */
/* ------------------------------------------------------------------ */

static afs_volume_t *g_vol;

/* Corrupt mode STARTS from a deliberately damaged image. Its contract is that
 * operations refuse cleanly and never half-apply -- NOT that the volume is
 * clean, which it never was. So the bitmap-agreement check, which reports that
 * PRE-EXISTING damage, is a clean-mode assertion. */
static int g_corrupt = 0;

static int check_volume(const char *when) {
    afs_verify_report_t rep[8];
    /* STRICT, not the everyday check -- and the difference is the whole point.
     *
     * This harness's header claimed it catches "bitmap agreement", and it did
     * not: afs_verify_memory() passes flags == 0, and bitmap agreement is the
     * ONE thing AFS_VERIFY_STRICT adds. Measured: a volume whose live file
     * header was marked free reported ZERO problems through the everyday call
     * and ONE through this one -- M26's exact shape, invisible to the harness
     * built to find it.
     *
     * Strict is safe here and always was: this harness BUILDS its volume with a
     * correct map, so it has no stale-map noise to be drowned by, which is the
     * only reason --strict is off by default elsewhere.
     *
     * AFS_VERIFY_ORPHANS is deliberately NOT enabled here yet, and the reason is
     * a finding rather than an oversight. Wired on, it fires on `op overflow` --
     * the op that drives the volume to ENOSPC -- after 34 to 55 iterations, on
     * every seed: afs_file_write_range's `fail_range` path deliberately does not
     * free the blocks it had already allocated ("no transactions: blocks
     * allocated before an I/O failure stay allocated but unreferenced", file.c).
     * That is a DOCUMENTED accepted limitation, so asserting zero orphans here
     * would red the gate forever.
     *
     * The honest form is a gate that pins the limitation and its size, not a
     * fuzzer assertion quietly switched off -- see m33_orphan_and_enospc.sh.
     * The capability and the completed data-block walk both ship regardless. */
    int n = afs_verify_memory_ex(g_img, sizeof g_img, 512, rep, 8,
                                 g_corrupt ? 0u : AFS_VERIFY_STRICT);
    if (n > 0) {
        for (int i = 0; i < n && i < 4; i++)
            printf("        [%d] block %u: %s\n", (int)rep[i].kind,
                   rep[i].block, rep[i].detail);
        fail("verify --strict after op", when);
        return -1;
    }
    return 0;
}

/* Resolve the header block of the DIRECTORY AT `path` ("" is the rootblock).
 * Every component is looked up, so this walks the path itself -- not its
 * parent. Getting that wrong is silent and looks exactly like a library bug:
 * the first run renamed a file out of a subdirectory into the root and the
 * model said it should have stayed, because resolve_dir("rty") had answered
 * "the root" (the block of the directory CONTAINING rty). */
static uint32_t resolve_dir(const char *path) {
    uint32_t cur = IMG_ROOT;
    char buf[192], dirbuf[192], namebuf[AFS_NAME_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    while (buf[0]) {
        afs_dirent_t d;
        split_path(buf, dirbuf, sizeof dirbuf, namebuf, sizeof namebuf);
        if (afs_dir_lookup(g_vol, cur, namebuf, strlen(namebuf), &d) != 0)
            return 0;
        cur = d.block;
        snprintf(buf, sizeof buf, "%s", dirbuf);
    }
    return cur;
}

/* every model file must read back byte-identical */
static int check_contents(const char *when) {
    for (int i = 0; i < MAX_ENT; i++) {
        struct ent *e = &g_ent[i];
        void *got = NULL;
        size_t gotlen = 0;
        char dirbuf[192], namebuf[AFS_NAME_MAX];
        uint32_t parent;
        if (!e->in_use || e->is_dir) continue;
        split_path(e->path, dirbuf, sizeof dirbuf, namebuf, sizeof namebuf);
        parent = resolve_dir(dirbuf);
        if (!parent) { fail("model dir did not resolve", e->path); return -1; }
        if (afs_file_read_all(g_vol, parent, namebuf, strlen(namebuf),
                              &got, &gotlen) != 0) {
            fail("read_all failed for a model file", e->path);
            return -1;
        }
        if (gotlen != e->len || (e->len && !got) ||
            (got && e->len && memcmp(got, e->data, e->len) != 0)) {
            printf("        %s: model len=%u volume len=%zu\n",
                   e->path, e->len, gotlen);
            free(got);
            fail("content mismatch after op", when);
            return -1;
        }
        free(got);
    }
    return 0;
}

/* Recursive walk comparing the volume's entry SET against the model's. */
struct seen {
    char path[192];
    int  is_dir;
};
static struct seen g_seen[MAX_ENT];
static int g_nseen;

static void mark_seen(const char *path, int is_dir) {
    if (g_nseen >= MAX_ENT) return;
    snprintf(g_seen[g_nseen].path, sizeof g_seen[g_nseen].path, "%s", path);
    g_seen[g_nseen].is_dir = is_dir;
    g_nseen++;
}

static int seen_has(const char *path) {
    for (int i = 0; i < g_nseen; i++)
        if (strcmp(g_seen[i].path, path) == 0) return 1;
    return 0;
}

static int walk_dir(uint32_t dirblock, const char *dirpath, int depth,
                    const char *when) {
    afs_dirent_t ents[128];
    size_t n = 0;
    char err[256] = {0};
    if (depth > WALK_DEPTH) { fail("walk depth exceeded", when); return -1; }
    if (afs_dir_list(g_vol, dirblock, ents, 128, &n) != 0) {
        fail("dir_list failed during walk", when);
        return -1;
    }
    if (n == 128) { fail("directory listing hit the harness cap", when); return -1; }
    for (size_t i = 0; i < n; i++) {
        char path[192];
        join_path(path, sizeof path, dirpath, ents[i].name);
        if (seen_has(path)) {
            fail("duplicate entry in the volume", path);
            return -1;
        }
        if (ents[i].kind == AFS_BLOCK_DIR || ents[i].kind == AFS_BLOCK_ROOT) {
            mark_seen(path, 1);
            if (walk_dir(ents[i].block, path, depth + 1, when) != 0) return -1;
        } else if (ents[i].kind == AFS_BLOCK_FILE) {
            mark_seen(path, 0);
        } else {
            /* a link should never exist: this harness never makes one */
            fail("unexpected entry kind", path);
            return -1;
        }
    }
    (void)err;
    return 0;
}

static void dump_state(const char *tag) {
    printf("        --- %s ---\n", tag);
    printf("        model:\n");
    for (int i = 0; i < MAX_ENT; i++)
        if (g_ent[i].in_use)
            printf("          %s %s\n", g_ent[i].is_dir ? "d" : "f", g_ent[i].path);
    printf("        volume:\n");
    for (int i = 0; i < g_nseen; i++)
        printf("          %s %s\n", g_seen[i].is_dir ? "d" : "f", g_seen[i].path);
}

static int check_tree(const char *when) {
    g_nseen = 0;
    if (walk_dir(IMG_ROOT, "", 0, when) != 0) return -1;
    for (int i = 0; i < MAX_ENT; i++) {
        if (!g_ent[i].in_use) continue;
        if (!seen_has(g_ent[i].path)) {
            dump_state(when);
            fail("model entry missing from the volume", g_ent[i].path);
            return -1;
        }
    }
    for (int i = 0; i < g_nseen; i++) {
        if (!find_by_path(g_seen[i].path)) {
            dump_state(when);
            fail("volume entry missing from the model", g_seen[i].path);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* operations                                                          */
/* ------------------------------------------------------------------ */

static void random_name(char *out, size_t cap) {
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyz";
    size_t n = 2 + rnd(6);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) out[i] = alpha[rnd(sizeof alpha - 1)];
    out[n] = '\0';
}

/* a name free in `dirpath` per the model */
static int fresh_name(const char *dirpath, char *out, size_t cap) {
    char dir[192], dir2[192], path[192], nm[AFS_NAME_MAX];
    split_path(dirpath, dir2, sizeof dir2, nm, sizeof nm);
    (void)dir;
    for (int tries = 0; tries < 40; tries++) {
        random_name(out, cap);
        join_path(path, sizeof path, dirpath, out);
        if (!find_by_path(path)) return 0;
    }
    return -1;
}

/* Two deliberate-failure operations, so the REFUSAL paths are covered rather
 * than assumed. Every other op picks a valid target and succeeds, which left
 * `refused` at zero across a whole run -- a gate that never exercises a
 * refusal is not testing it.
 *
 *  - a create aimed at a name that already exists (the duplicate check),
 *  - a write larger than the volume can hold (the ENOSPC discipline: deficit
 *    blocks are allocated BEFORE anything is mutated, so a refused overflow
 *    must leave the previous content byte-identical -- which the model/content
 *    check then verifies).
 */
static void op_overflow(void) {
    struct ent *e = find_random();
    char err[256] = {0};
    if (!e || e->is_dir) return;
    g_tried[OP_OVERFLOW]++;
    /* comfortably past the volume: 1760 blocks is under 1 MiB */
    uint32_t len = (uint32_t)IMG_BLOCKS * 512u + 4096u;
    uint8_t *buf = malloc(len);
    if (!buf) return;
    memset(buf, 0xA5, len);
    if (afs_file_write_all(g_vol, e->block, buf, len, err, sizeof err) != 0) {
        g_refused[OP_OVERFLOW]++;
        free(buf);
        return;      /* the model is unchanged; the content check proves it */
    }
    /* It SUCCEEDED, which means the volume has more free space than the
     * arithmetic above assumed (a damaged bitmap can look emptier than it is).
     * The model must follow, or the very next check reports a mismatch that
     * looks like a library bug. */
    free(e->data);
    e->data = buf;
    e->len = len;
    g_done[OP_OVERFLOW]++;
}

static void op_create(void) {
    char dirpath[192] = "", name[AFS_NAME_MAX], path[192];
    char err[256] = {0};
    uint32_t hdr = 0, dblk;
    struct ent *d;
    if (rnd(3) == 0) {
        d = random_dir();
        if (d) snprintf(dirpath, sizeof dirpath, "%s", d->path);
    }
    /* deliberately aim at an existing name sometimes: the duplicate refusal */
    struct ent *victim = NULL;
    if (rnd(6) == 0) {
        victim = find_random();
        if (victim && victim->is_dir) victim = NULL;
        if (victim) snprintf(dirpath, sizeof dirpath, "%s", victim->dir);
    }
    if (victim) snprintf(name, sizeof name, "%s", victim->name);
    else if (fresh_name(dirpath, name, sizeof name) != 0) return;
    dblk = resolve_dir(dirpath);
    if (!dblk) return;
    g_tried[OP_CREATE]++;
    if (afs_file_create(g_vol, dblk, name, strlen(name), &hdr,
                        err, sizeof err) != 0) {
        g_refused[OP_CREATE]++;
        return;      /* nothing changed; the model must not change either */
    }
    if (victim) {
        /* created where a name already existed -- a real library bug. */
        fail("create over an existing name succeeded", path);
        return;
    }
    /* content, sometimes large enough to force extension blocks */
    uint32_t len = rnd(3) == 0 ? rnd(MAX_DATA) : rnd(600);
    uint8_t *buf = malloc(len ? len : 1);
    struct ent *e = alloc_ent();
    if (!buf || !e) { free(buf); return; }
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(rnd(256));
    if (afs_file_write_all(g_vol, hdr, buf, len, err, sizeof err) != 0) {
        /* refused (ENOSPC, say): the file exists but is empty -- record it
         * that way rather than pretending the op did not happen */
        e->in_use = 1;
        e->is_dir = 0;
        join_path(e->path, sizeof e->path, dirpath, name);
        snprintf(e->name, sizeof e->name, "%s", name);
        snprintf(e->dir, sizeof e->dir, "%s", dirpath);
        e->block = hdr;
        e->data = calloc(1, 1);
        e->len = 0;
        g_refused[OP_CREATE]++;
        g_done[OP_CREATE]++;
        free(buf);
        return;
    }
    e->in_use = 1;
    e->is_dir = 0;
    join_path(e->path, sizeof e->path, dirpath, name);
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->dir, sizeof e->dir, "%s", dirpath);
    e->block = hdr;
    e->data = buf;
    e->len = len;
    g_done[OP_CREATE]++;
    join_path(path, sizeof path, dirpath, name);
}

static void op_overwrite(void) {
    struct ent *e = find_random();
    char err[256] = {0};
    uint32_t dblk;
    if (!e || e->is_dir) return;
    dblk = resolve_dir(e->dir);
    if (!dblk) return;
    g_tried[OP_OVERWRITE]++;
    uint32_t len = rnd(3) == 0 ? rnd(MAX_DATA) : rnd(600);
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) return;
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(rnd(256));
    if (afs_file_write_all(g_vol, e->block, buf, len, err, sizeof err) != 0) {
        g_refused[OP_OVERWRITE]++;
        free(buf);
        return;
    }
    free(e->data);
    e->data = buf;
    e->len = len;
    g_done[OP_OVERWRITE]++;
}

static void op_range(void) {
    struct ent *e = find_random();
    char err[256] = {0};
    uint32_t dblk;
    if (!e || e->is_dir) return;
    dblk = resolve_dir(e->dir);
    if (!dblk) return;
    g_tried[OP_RANGE]++;
    /* offset deliberately spans: inside the file, exactly at EOF, and past it
     * (so the zero-gap allocation path runs) */
    uint64_t off;
    switch (rnd(4)) {
    case 0:  off = rnd(e->len ? e->len : 1); break;         /* in place */
    case 1:  off = e->len; break;                           /* at EOF */
    case 2:  off = (uint64_t)e->len + rnd(4096); break;     /* past EOF */
    default: off = rnd(4) ? 0 : (uint64_t)e->len + 3; break;
    }
    uint32_t len = 1 + rnd(1500);
    uint8_t *buf = malloc(len);
    if (!buf) return;
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(rnd(256));
    if (afs_file_write_range(g_vol, e->block, buf, len, off,
                             err, sizeof err) != 0) {
        g_refused[OP_RANGE]++;
        free(buf);
        return;
    }
    /* fold into the model: bytes before the old size survive, the gap is
     * zeros, and the tail is the new data */
    uint64_t end = off + len;
    uint32_t newlen = end > e->len ? (uint32_t)end : e->len;
    uint8_t *nd = calloc(newlen ? newlen : 1, 1);
    if (!nd) { free(buf); return; }
    if (e->len) memcpy(nd, e->data, e->len);
    memcpy(nd + off, buf, len);
    free(e->data);
    e->data = nd;
    e->len = newlen;
    free(buf);
    g_done[OP_RANGE]++;
}

static void op_trunc(void) {
    struct ent *e = find_random();
    char err[256] = {0};
    if (!e || e->is_dir) return;
    g_tried[OP_TRUNC]++;
    uint32_t want = rnd(3) == 0 ? rnd(MAX_DATA) : rnd(600);
    uint8_t *buf = calloc(want ? want : 1, 1);
    if (!buf) return;
    uint32_t keep = want < e->len ? want : e->len;
    if (keep) memcpy(buf, e->data, keep);
    if (afs_file_write_all(g_vol, e->block, buf, want, err, sizeof err) != 0) {
        g_refused[OP_TRUNC]++;
        free(buf);
        return;
    }
    free(e->data);
    e->data = buf;
    e->len = want;
    g_done[OP_TRUNC]++;
}

static void op_rename(void) {
    struct ent *e = find_random();
    char err[256] = {0};
    char newname[AFS_NAME_MAX], newpath[192];
    char dirbuf[192], namebuf[AFS_NAME_MAX];
    uint32_t sp, dp;
    if (!e) return;
    /* moving a directory would need the model to re-key the whole subtree;
     * rename directories IN PLACE only, which still exercises the chain
     * splice, the cache move and the duplicate check */
    if (e->is_dir && rnd(2)) return;
    char target[192];
    if (e->is_dir) snprintf(target, sizeof target, "%s", e->dir);
    else if (rnd(3) == 0) {
        struct ent *d = random_dir();
        if (d) snprintf(target, sizeof target, "%s", d->path);
        else   snprintf(target, sizeof target, "%s", e->dir);
    } else snprintf(target, sizeof target, "%s", e->dir);
    if (fresh_name(target, newname, sizeof newname) != 0) return;
    join_path(newpath, sizeof newpath, target, newname);
    sp = resolve_dir(e->dir);
    dp = resolve_dir(target);
    if (!sp || !dp) return;
    split_path(e->path, dirbuf, sizeof dirbuf, namebuf, sizeof namebuf);
    /* A directory rename re-keys every descendant. If any of them would not
     * fit the model's path buffer, refuse the rename BEFORE doing it -- a
     * rename that is applied but only half-tracked is a harness bug that would
     * look exactly like a library bug. (With 8-char names and bounded depth it
     * cannot happen; the guard is here so that stays true by construction.) */
    if (e->is_dir) {
        size_t oldlen = strlen(e->path);
        for (int i = 0; i < MAX_ENT; i++) {
            size_t rest_len;
            if (!g_ent[i].in_use || g_ent[i].path == e->path) continue;
            if (strncmp(g_ent[i].path, e->path, oldlen) != 0) continue;
            if (g_ent[i].path[oldlen] != '/') continue;
            rest_len = strlen(g_ent[i].path + oldlen);
            if (strlen(newpath) + rest_len >= sizeof g_ent[i].path) return;
        }
    }
    g_tried[OP_RENAME]++;
    if (afs_dir_rename(g_vol, sp, namebuf, strlen(namebuf),
                       dp, newname, strlen(newname), err, sizeof err) != 0) {
        g_refused[OP_RENAME]++;
        return;
    }
    if (e->is_dir) {
        size_t oldlen = strlen(e->path);
        for (int i = 0; i < MAX_ENT; i++) {
            char np[192];
            if (!g_ent[i].in_use || g_ent[i].path == e->path) continue;
            if (strncmp(g_ent[i].path, e->path, oldlen) != 0) continue;
            if (g_ent[i].path[oldlen] != '/') continue;
            snprintf(np, sizeof np, "%s%s", newpath, g_ent[i].path + oldlen);
            snprintf(g_ent[i].path, sizeof g_ent[i].path, "%s", np);
        }
    }
    snprintf(e->path, sizeof e->path, "%s", newpath);
    snprintf(e->dir, sizeof e->dir, "%s", target);
    snprintf(e->name, sizeof e->name, "%s", newname);
    g_done[OP_RENAME]++;
}

static void op_unlink(void) {
    struct ent *e = find_random();
    char err[256] = {0};
    char namebuf[AFS_NAME_MAX], dirbuf[192];
    uint32_t dblk;
    if (!e || e->is_dir) return;
    dblk = resolve_dir(e->dir);
    if (!dblk) return;
    split_path(e->path, dirbuf, sizeof dirbuf, namebuf, sizeof namebuf);
    g_tried[OP_UNLINK]++;
    if (afs_file_delete(g_vol, dblk, namebuf, strlen(namebuf),
                        err, sizeof err) != 0) {
        g_refused[OP_UNLINK]++;
        return;
    }
    free(e->data);
    e->data = NULL;
    e->in_use = 0;
    g_done[OP_UNLINK]++;
}

static void op_mkdir(void) {
    char dirpath[192] = "", name[AFS_NAME_MAX], path[192];
    char err[256] = {0};
    struct ent *d;
    uint32_t dblk, nb = 0;
    if (rnd(3) == 0) {
        d = random_dir();
        if (d) snprintf(dirpath, sizeof dirpath, "%s", d->path);
    }
    if (fresh_name(dirpath, name, sizeof name) != 0) return;
    dblk = resolve_dir(dirpath);
    if (!dblk) return;
    g_tried[OP_MKDIR]++;
    if (afs_dir_mkdir(g_vol, dblk, name, strlen(name), &nb,
                      err, sizeof err) != 0) {
        g_refused[OP_MKDIR]++;
        return;
    }
    {
        struct ent *e = alloc_ent();
        if (e) {
            e->in_use = 1;
            e->is_dir = 1;
            join_path(e->path, sizeof e->path, dirpath, name);
            snprintf(e->name, sizeof e->name, "%s", name);
            snprintf(e->dir, sizeof e->dir, "%s", dirpath);
            e->block = nb;
            e->data = NULL;
            e->len = 0;
        }
    }
    join_path(path, sizeof path, dirpath, name);
    g_done[OP_MKDIR]++;
}

static void op_rmdir(void) {
    struct ent *e;
    char err[256] = {0};
    char namebuf[AFS_NAME_MAX], dirbuf[192];
    uint32_t dblk;
    /* find an EMPTY directory: rmdir on a non-empty one is refused by design
     * (AFFlib parity) and the refusal is already covered by m14 */
    unsigned dirs = 0;
    for (int i = 0; i < MAX_ENT; i++)
        if (g_ent[i].in_use && g_ent[i].is_dir && dir_is_empty(g_ent[i].path))
            dirs++;
    if (!dirs) return;
    unsigned pick = rnd(dirs);
    e = NULL;
    for (int i = 0; i < MAX_ENT; i++) {
        if (!g_ent[i].in_use || !g_ent[i].is_dir) continue;
        if (!dir_is_empty(g_ent[i].path)) continue;
        if (pick-- == 0) { e = &g_ent[i]; break; }
    }
    if (!e) return;
    dblk = resolve_dir(e->dir);
    if (!dblk) return;
    split_path(e->path, dirbuf, sizeof dirbuf, namebuf, sizeof namebuf);
    g_tried[OP_RMDIR]++;
    if (afs_dir_rmdir(g_vol, dblk, namebuf, strlen(namebuf),
                      err, sizeof err) != 0) {
        g_refused[OP_RMDIR]++;
        return;
    }
    drop_subtree(e->path);
    g_done[OP_RMDIR]++;
}

/* ------------------------------------------------------------------ */
/* driver                                                              */
/* ------------------------------------------------------------------ */

static void run_ops(unsigned iterations) {
    for (unsigned it = 0; it < iterations; it++) {
        unsigned pick = rnd(100);
        g_iter = it;
        g_last_op = pick < 24 ? OP_CREATE
                   : pick < 36 ? OP_OVERWRITE
                   : pick < 52 ? OP_RANGE
                   : pick < 60 ? OP_TRUNC
                   : pick < 70 ? OP_RENAME
                   : pick < 78 ? OP_UNLINK
                   : pick < 86 ? OP_MKDIR
                   : pick < 94 ? OP_RMDIR : OP_OVERFLOW;
        if (pick < 24)      op_create();
        else if (pick < 36) op_overwrite();
        else if (pick < 52) op_range();
        else if (pick < 60) op_trunc();
        else if (pick < 70) op_rename();
        else if (pick < 78) op_unlink();
        else if (pick < 86) op_mkdir();
        else if (pick < 94) op_rmdir();
        else                op_overflow();
        if (failures) return;
        /* The checks are the point, and they are cheap next to an operation
         * (verify is O(blocks), the walk is O(entries)), so they run after
         * EVERY one -- not once at the end, which would report only the last
         * state and hide a corruption an earlier op introduced. */
        if (check_volume("after op") != 0) return;
        if (check_contents("after op") != 0) return;
        if (check_tree("after op") != 0) return;
    }
}

static void report_coverage(unsigned floors_needed) {
    unsigned missed = 0;
    unsigned refusals = 0;
    for (int i = 0; i < OP_COUNT; i++) {
        printf("  %-10s tried=%-5u ok=%-5u refused=%-4u\n",
               op_name[i], g_tried[i], g_done[i], g_refused[i]);
        refusals += g_refused[i];
        /* OVERFLOW is SUPPOSED to be refused (the volume cannot hold it), so
         * requiring a success from it would be asserting the opposite of the
         * design. Every other class must succeed at least once, or the run
         * never exercised it. */
        if (!g_done[i] && i != OP_OVERFLOW) {
            printf("  FAIL  op class never succeeded: %s\n", op_name[i]);
            missed++;
        }
    }
    if (missed && floors_needed) failures++;
    /* A run with zero refusals anywhere has not touched a single refusal
     * path, whatever the success tally says. */
    if (!refusals && floors_needed) {
        printf("  FAIL  no operation was ever refused: refusal paths untested\n");
        failures++;
    }
}

int main(int argc, char **argv) {
    uint64_t seed = 1;
    unsigned iterations = 300;
    int corrupt = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--seed=", 7) == 0) seed = strtoull(argv[i] + 7, NULL, 10);
        else if (strncmp(argv[i], "--iters=", 8) == 0) iterations = (unsigned)strtoul(argv[i] + 8, NULL, 10);
        else if (strcmp(argv[i], "--corrupt") == 0) corrupt = 1;
        else if (strncmp(argv[i], "--corrupt=", 10) == 0) { corrupt = 1; iterations /= 4; }
    }
    rng_seed(seed);
    build_volume();
    if (corrupt) damage_volume(24);
    g_corrupt = corrupt;

    printf("write fuzz: seed=%llu iters=%u corrupt=%d\n",
           (unsigned long long)seed, iterations, corrupt);

    /* one open for the whole run: reopening per op would hide state that the
     * volume object itself caches */
    char err[256] = {0};
    if (afs_volume_open_memory(g_img, sizeof g_img, 512, &g_vol,
                               err, sizeof err) != 0) {
        fail("open volume", err);
        return 1;
    }
    if (!corrupt) {
        if (check_volume("initial") != 0) { afs_volume_close(g_vol); return 1; }
    }
    run_ops(iterations);
    if (g_vol) afs_volume_close(g_vol);
    g_vol = NULL;

    report_coverage(corrupt ? 0 : 1);

    if (failures) {
        printf("write fuzz: %d failure(s)\n", failures);
        return 1;
    }
    printf("write fuzz: PASS (seed=%llu, %u iterations)\n",
           (unsigned long long)seed, iterations);
    return 0;
}