/* Adversarial robustness harness for the AmigaDOS core.
 *
 * Every block pointer the parser follows comes from untrusted on-disk data.
 * This harness feeds deliberately hostile images to the WHOLE stack -- volume
 * open, structure verify, recursive listing, full file extraction, link
 * resolution -- and asserts that the only acceptable outcomes are "clean
 * data" or "a clean error". A crash, a hang, or an out-of-bounds access is a
 * FAILURE, never an acceptable result.
 *
 * Three classes of hostile input, cheapest first:
 *   1. real fixtures with bytes flipped (bit rot);
 *   2. real fixtures with whole blocks zeroed (a dead directory, a dead data
 *      chain);
 *   3. synthetic volumes whose hash chains and data pointers aim outside the
 *      volume, at themselves, at the bitmap, and in long cycles.
 *
 * Built with ASan+UBSan by tests/fuzz_gate.sh; a run under a sanitizer is the
 * point -- without it, "no crash" is only "did not happen to segfault today".
 *
 *   usage: fuzz_test [fixture_dir] [iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "amigafs/be.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/dir.h"
#include "amigafs/file.h"
#include "amigafs/hash.h"
#include "amigafs/links.h"
#include "amigafs/verify.h"
#include "amigafs/volume.h"

#define BS 512
#define SYNTH_BLOCKS 1760
#define SYNTH_ROOT   880

static unsigned long g_images;      /* images that opened AND were walked */
static unsigned long g_refused;     /* refused with a reason: also fine */
static unsigned long g_entries;     /* directory entries actually visited */
static unsigned long g_files;       /* files actually read end-to-end */
static unsigned long g_problems;    /* structural problems the verifier saw */
static int g_failures;

/* Coverage counters matter: a fuzzer that is refused every image at the DOS
 * id check has tested nothing. Reporting these makes "0 failures" mean
 * "reached the parser", not "never got past open". */

static uint8_t *slurp(const char *path, size_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b); fclose(f); return NULL;
    }
    fclose(f);
    *out = (size_t)sz;
    return b;
}

/* Walk every entry we can reach, read every file in full, resolve every link.
 * `depth` bounds directory nesting; a hostile image can point a directory at
 * an ancestor, and the ONLY thing stopping infinite recursion here is depth --
 * so if this returns, the walk is bounded no matter what the image says. */
static void walk_everything(afs_volume_t *v, uint32_t dirblk, int depth) {
    if (depth > 8) return;
    afs_dirent_t ents[128];
    size_t n = 0;
    if (afs_dir_list(v, dirblk, ents, 128, &n) != 0) return;
    if (n > 128) n = 128;                       /* api contract; be explicit */
    for (size_t i = 0; i < n; i++) {
        const char *nm = ents[i].name;
        g_entries++;
        size_t nl = strlen(nm);
        switch (ents[i].kind) {
        case AFS_BLOCK_DIR:
            walk_everything(v, ents[i].block, depth + 1);
            break;
        case AFS_BLOCK_FILE:
        case AFS_BLOCK_LINKFILE: {
            void *buf = NULL; size_t len = 0;
            if (afs_file_read_all(v, dirblk, nm, nl, &buf, &len) == 0) {
                g_files++;
                free(buf);
            }
            /* also exercise the by-block path FUSE uses */
            uint32_t real = 0;
            if (afs_link_resolve_file(v, dirblk, nm, nl, &real) == 0) {
                unsigned char tmp[256];
                size_t got = 0;
                (void)afs_read_file_by_block(v, real, tmp, sizeof tmp, 0, &got);
                (void)afs_read_file_by_block(v, real, tmp, sizeof tmp,
                                            1u << 30, &got);  /* past EOF */
            }
            unsigned nlink = 0;
            (void)afs_file_nlink(v, dirblk, nm, nl, &nlink);
            break;
        }
        case AFS_BLOCK_SOFTLINK: {
            char tgt[1024];
            (void)afs_softlink_target(v, dirblk, nm, nl, tgt, sizeof tgt);
            break;
        }
        case AFS_BLOCK_LINKDIR:
            /* deliberately NOT descended: C12, a hard-linked dir is a cycle */
            break;
        default:
            break;
        }
    }
}

/* The whole stack over one in-memory image. Every outcome is acceptable
 * except "we never came back" or "open succeeded without a handle". */
static void exercise(const uint8_t *img, size_t size) {
    afs_volume_t *v = NULL;
    char err[256];
    if (afs_volume_open_memory((void *)(uintptr_t)img, size, BS, &v,
                               err, sizeof err) != 0) {
        g_refused++;
        return;                        /* refused with a reason: fine */
    }
    if (!v) { g_failures++; return; }

    afs_verify_report_t rep[64];
    int nprob = afs_verify_memory(img, size, BS, rep, 64);
    g_problems += (unsigned long)nprob;

    walk_everything(v, afs_volume_rootblock(v), 0);

    /* the raw block reader is the last line of defence -- ask it for
     * everything, including absurd block numbers */
    unsigned char blk[BS];
    uint32_t nblk = (uint32_t)(size / BS);
    const uint32_t probes[4] = { 0, 0xFFFFFFFFu, nblk, nblk + 1 };
    for (int probe = 0; probe < 4; probe++)
        (void)afs_volume_read_block(v, probes[probe], blk, BS);
    afs_volume_close(v);
    g_images++;
}

/* --- class 1 & 2: corrupt real fixtures --------------------------------- */
static void fuzz_fixtures(const char *dir, const char *const *names,
                          size_t nnames, unsigned long iters) {
    for (unsigned long it = 0; it < iters; it++) {
        const char *nm = names[it % nnames];
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, nm);
        size_t sz = 0;
        uint8_t *img = slurp(path, &sz);
        if (!img || sz < BS * 4) { free(img); continue; }

        srandom((unsigned)(it * 2654435761u));
        if (it % 4 == 3) {
            size_t blk = (size_t)(random() % (int)(sz / BS)) * BS;
            memset(img + blk, 0, BS);                 /* a whole dead block */
        } else {
            int n = 1 + (int)(random() % 8);
            for (int k = 0; k < n; k++) {
                size_t off = (size_t)random() % sz;
                img[off] ^= (uint8_t)(1u << (random() % 8));
            }
        }
        exercise(img, sz);
        free(img);
    }
}

/* --- class 3: synthetic volumes aimed at the edges ---------------------- */
/* Start from a volume the core accepts, then point its hash buckets and data
 * tables at places a real filesystem would never point them. */
static void fuzz_synthetic(unsigned long iters) {
    size_t sz = (size_t)SYNTH_BLOCKS * BS;
    for (unsigned long it = 0; it < iters; it++) {
        uint8_t *img = calloc(1, sz);
        if (!img) return;
        srandom((unsigned)(it * 40503u + 7u));

        put_be32(img, ((uint32_t)'D' << 24) | ((uint32_t)'O' << 16) |
                     ((uint32_t)'S' << 8) | 1u);          /* DOS\1 */
        uint8_t *r = img + SYNTH_ROOT * BS;
        put_be32(r + 0, AFS_T_HEADER);
        put_be32(r + 12, (uint32_t)afs_hash_table_size(BS));
        put_be32(r + 312, 0xFFFFFFFFu);                   /* bitmap valid */
        put_be32(r + 316, SYNTH_ROOT + 1);                /* bitmap block */
        r[BS - 80] = 4;
        memcpy(r + BS - 79, "synth", 4);
        put_be32(r + BS - 4, AFS_ST_ROOT);
        put_be32(r + 20, afs_block_checksum(r, BS, 20));

        uint32_t nfill = (uint32_t)(random() % 24);
        for (uint32_t k = 0; k < nfill; k++) {
            uint32_t bucket = (uint32_t)(random() % 72);
            uint32_t stored;
            switch (random() % 5) {
            case 0: stored = 0xFFFFFFFFu; break;                     /* absurd  */
            case 1: stored = SYNTH_BLOCKS + (uint32_t)(random() % 4096); break;
            case 2: stored = SYNTH_ROOT + 1; break;                 /* bitmap  */
            case 3: stored = 0; break;                              /* empty   */
            default: stored = 1200 + (uint32_t)(random() % 400); break;
            }
            /* The hostile pointer goes IN the hash bucket, where the library
             * will meet it. The block we actually BUILD to make that pointer
             * look real is always in bounds -- a generator that writes through
             * its own hostile value tests the generator, not the library. */
            uint32_t bld = stored % SYNTH_BLOCKS;
            if (bld == SYNTH_ROOT) bld++;   /* keep the volume openable, so
                                             * the walk is actually reached */
            uint8_t *e = img + bld * BS;
            put_be32(e + 0, AFS_T_HEADER);
            put_be32(e + 36, (uint32_t)(random() % 100000)); /* byte_size   */
            put_be32(e + BS - 16, (random() % 2) ? bld : 0xFFFFFFFFu); /* self */
            put_be32(e + BS - 12, SYNTH_ROOT);
            put_be32(e + BS - 8, 0);
            put_be32(e + BS - 4, (random() % 2) ? AFS_ST_FILE : AFS_ST_USERDIR);
            e[BS - 80] = (uint8_t)(1 + random() % 8);
            memcpy(e + BS - 79, "f", 1);
            for (int t = 0; t < 6; t++) {   /* the data_blocks[] table */
                uint32_t tp;
                switch (random() % 4) {
                case 0: tp = 0xFFFFFFFFu; break;
                case 1: tp = SYNTH_BLOCKS + (uint32_t)random() % 1000; break;
                default: tp = 1300 + (uint32_t)(random() % 300); break;
                }
                put_be32(e + 64 + 4 * t, tp);
            }
            put_be32(e + 20, afs_block_checksum(e, BS, 20));

            put_be32(r + 24 + 4 * bucket, stored);
        }
        put_be32(r + 20, afs_block_checksum(r, BS, 20));

        exercise(img, sz);
        free(img);
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/fixtures";
    unsigned long iters = argc > 2 ? strtoul(argv[2], NULL, 10) : 150;

    static const char *const names[] = {
        "testffs.adf", "testofs.adf", "arccsh.adf", "links.adf",
        "dirhash.adf", "testhd.adf"
    };
    /* a hard 4-minute ceiling: a non-terminating walk must FAIL the gate,
     * not wedge the build */
    alarm(240);

    printf("fuzz: %lu fixture iterations + %lu synthetic\n", iters, iters);
    fuzz_fixtures(dir, names, sizeof names / sizeof names[0], iters);
    fuzz_synthetic(iters);
    printf("fuzz: %lu images opened+walked, %lu refused cleanly\n",
           g_images, g_refused);
    printf("fuzz: reached %lu directory entries, %lu files read in full, "
           "%lu verifier problems seen\n", g_entries, g_files, g_problems);

    /* A green run that reached nothing is a green run that proves nothing. */
    if (g_entries < 100 || g_files < 20) {
        printf("fuzz: FAIL coverage too low -- the harness is not reaching "
               "the parser\n");
        return 1;
    }
    printf("fuzz: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
