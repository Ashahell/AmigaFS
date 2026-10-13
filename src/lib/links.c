#include "amigafs/links.h"
#include "amigafs/be.h"
#include "amigafs/blocktypes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Follow a hard link chain to the real file header. A plain file is its own
 * real entry; a ST_LINKFILE carries real_entry. A link that points at another
 * link is chased (test_link_chains.adf exercises links to links). */
static int chase(afs_volume_t *v, uint32_t block, uint32_t *out, int depth) {
    size_t bsize = afs_volume_block_size(v);
    if (depth > 16) return -1;                 /* link loop guard */
    uint32_t cur = block;
    for (int i = 0; i < 16; i++) {
        if (cur >= afs_volume_block_count(v)) return -1;
        uint8_t *b = malloc(bsize);
        if (!b) return -1;
        if (afs_volume_read_block(v, cur, b, bsize) != 0) { free(b); return -1; }
        afs_block_kind_t k = afs_block_kind(b, bsize);
        if (k == AFS_BLOCK_FILE) { *out = cur; free(b); return 0; }
        if (k == AFS_BLOCK_LINKFILE) {
            uint32_t real = be32(b + bsize - 44);
            free(b);
            if (real == 0 || real == cur) return -1;
            cur = real;
            continue;
        }
        free(b);
        return -1;
    }
    (void)depth;
    return -1;
}

int afs_link_resolve_file(afs_volume_t *v, uint32_t dirblock,
                          const char *name, size_t namelen, uint32_t *out) {
    if (!v || !name || !out) return -1;
    afs_dirent_t e;
    if (afs_dir_lookup(v, dirblock, name, namelen, &e) != 0) return -1;
    return chase(v, e.block, out, 0);
}

int afs_file_nlink(afs_volume_t *v, uint32_t dirblock,
                   const char *name, size_t namelen, unsigned *out) {
    if (!v || !name || !out) return -1;
    afs_dirent_t e;
    if (afs_dir_lookup(v, dirblock, name, namelen, &e) != 0) return -1;
    uint32_t real;
    if (chase(v, e.block, &real, 0) != 0) return -1;

    /* the real entry, then every block in its next_link chain */
    size_t bsize = afs_volume_block_size(v);
    unsigned count = 1;
    uint32_t cur = real;
    uint32_t limit = afs_volume_block_count(v);
    for (uint32_t steps = 0; steps <= limit; steps++) {
        uint8_t *b = malloc(bsize);
        if (!b) return -1;
        if (afs_volume_read_block(v, cur, b, bsize) != 0) { free(b); break; }
        uint32_t next = be32(b + bsize - 40);   /* next_link */
        free(b);
        if (next == 0 || next == cur || next >= afs_volume_block_count(v)) break;
        count++;
        cur = next;
    }
    *out = count;
    return 0;
}

/* Translate an Amiga softlink path into a POSIX-relative target.
 *
 * Amiga rules: `Vol:` and a lone `:` mean the volume root; `/x` is from the
 * volume root; `//x` is also the root (the extra slash is collapsed); anything
 * else is relative to the link's own directory. We emit a path relative to the
 * mount root: a volume-root-relative target gets a `../` prefix per level, and
 * a same-directory target is emitted bare.
 */
int afs_softlink_target(afs_volume_t *v, uint32_t dirblock,
                        const char *name, size_t namelen,
                        char *out, size_t cap) {
    if (!v || !name || !out || cap == 0) return -1;
    afs_dirent_t e;
    if (afs_dir_lookup(v, dirblock, name, namelen, &e) != 0) return -1;

    size_t bsize = afs_volume_block_size(v);
    uint8_t *b = malloc(bsize);
    if (!b) return -1;
    if (afs_volume_read_block(v, e.block, b, bsize) != 0) { free(b); return -1; }
    if (afs_block_kind(b, bsize) != AFS_BLOCK_SOFTLINK) { free(b); return -1; }

    /* The symbolic name lives in the hash-table area at offset 24. It is a plain
     * C-STRING (NUL-terminated), NOT a BCPL length-prefixed string -- so we
     * must NOT call bstr(), which would consume the first character as a
     * length byte. */
    char raw[1024];
    size_t maxlen = bsize - 24;
    if (maxlen > sizeof raw - 1) maxlen = sizeof raw - 1;
    size_t rl = 0;
    while (rl < maxlen && b[24 + rl] != '\0') rl++;
    memcpy(raw, b + 24, rl);
    raw[rl] = '\0';
    free(b);

    /* strip an Amiga volume prefix: "Vol:" or ":" */
    const char *t = raw;
    const char *colon = strchr(raw, ':');
    if (colon) {
        /* a colon only counts as a volume prefix when it precedes any slash,
         * i.e. in the first path component */
        const char *slash = strchr(raw, '/');
        if (!slash || colon < slash) {
            t = colon + 1;
            /* skip an immediately following '/' so "Vol:/x" == "/x" */
            if (*t == '/') t++;
        }
    }

    /* count how deep the link's own directory is, to build the ../ prefix */
    unsigned depth = 0;
    {
        /* dirblock is the link's directory; walk its parents up to the root */
        uint32_t d = dirblock;
        uint32_t limit = afs_volume_block_count(v);
        uint8_t parent[4];
        for (uint32_t i = 0; i < 64 && d && d < limit; i++) {
            uint8_t *db = malloc(bsize);
            if (!db) break;
            if (afs_volume_read_block(v, d, db, bsize) != 0) { free(db); break; }
            memcpy(parent, db + bsize - 12, 4);
            free(db);
            uint32_t p = ((uint32_t)parent[0] << 24) | ((uint32_t)parent[1] << 16) |
                         ((uint32_t)parent[2] << 8) | parent[3];
            if (p == d || p == 0) break;
            d = p;
            depth++;
        }
    }

    /* build the result */
    char rel[1024];
    size_t o = 0;
    rel[0] = '\0';

    const char *body = t;
    int absolute = 0;
    while (*body == '/') { absolute = 1; body++; }   /* '/x' and '//x' both root */

    if (absolute) {
        /* from the mount root: no ../ prefix */
        o = (size_t)snprintf(rel, sizeof rel, "%s", body);
    } else if (strchr(body, '/') != NULL) {
        /* a relative path with slashes: prefix with ../ per level of depth */
        for (unsigned i = 0; i < depth; i++) {
            int n = snprintf(rel + o, sizeof rel - o, "../");
            if (n < 0 || (size_t)n >= sizeof rel - o) break;
            o += (size_t)n;
        }
        snprintf(rel + o, sizeof rel - o, "%s", body);
    } else {
        /* a bare relative name: same directory, emit as-is */
        snprintf(rel, sizeof rel, "%s", body);
    }

    snprintf(out, cap, "%s", rel);
    return 0;
}