#include "amigafs/links.h"
#include "amigafs/alloc.h"
#include "amigafs/be.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
#include "amigafs/dir.h"
#include "amigafs/names.h"
#include "amigafs/volume.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void lk_err(char *buf, size_t len, const char *fmt, ...) {
    va_list ap;
    if (!buf || len == 0) return;
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

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

/* Count the real entry plus every block in its next_link chain. Shared by the
 * two public entry points below so the count exists in exactly ONE place: the
 * FUSE stat path resolves an inode to a header block and never sees the name,
 * while afs_file_nlink has a name and must resolve it first. */
static int nlink_from_real(afs_volume_t *v, uint32_t real, unsigned *out) {
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
        if (next == 0 || next == cur || next >= limit) break;
        count++;
        cur = next;
    }
    *out = count;
    return 0;
}

int afs_file_nlink(afs_volume_t *v, uint32_t dirblock,
                   const char *name, size_t namelen, unsigned *out) {
    if (!v || !name || !out) return -1;
    afs_dirent_t e;
    if (afs_dir_lookup(v, dirblock, name, namelen, &e) != 0) return -1;
    uint32_t real;
    if (chase(v, e.block, &real, 0) != 0) return -1;
    return nlink_from_real(v, real, out);
}

int afs_nlink_for_block(afs_volume_t *v, uint32_t hdrblock, unsigned *out) {
    if (!v || !out) return -1;
    if (hdrblock == 0 || hdrblock >= afs_volume_block_count(v)) return -1;
    uint32_t real;
    if (chase(v, hdrblock, &real, 0) != 0) return -1;
    return nlink_from_real(v, real, out);
}

/* Translate an Amiga softlink path into a POSIX-relative target, given the
 * depth of the directory holding the link (0 = the volume root). This is the
 * pure form, exposed for the FUSE layer which already knows the link's depth
 * (or, more simply, emits a root-relative target). */
int afs_softlink_translate(const char *raw, unsigned depth, char *out, size_t cap) {
    if (!raw || !out || cap == 0) return -1;
    /* strip a leading Amiga volume prefix: "Vol:" or ":" */
    const char *t = raw;
    const char *colon = strchr(raw, ':');
    if (colon) {
        const char *slash = strchr(raw, '/');
        if (!slash || colon < slash) {
            t = colon + 1;
            if (*t == '/') t++;
        }
    }
    const char *body = t;
    int absolute = 0;
    while (*body == '/') { absolute = 1; body++; }

    size_t o = 0;
    out[0] = '\0';
    if (absolute) {
        /* the MOUNT ROOT is the volume root, so an absolute Amiga path is
         * already the correct mount-relative POSIX path */
        snprintf(out, cap, "%s", body);
        return 0;
    }
    if (strchr(body, '/')) {
        for (unsigned i = 0; i < depth && o + 3 < cap; i++) {
            memcpy(out + o, "../", 3);
            o += 3;
        }
        snprintf(out + o, cap - o, "%s", body);
    } else {
        snprintf(out, cap, "%s", body);
    }
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

    return afs_softlink_translate(raw, depth, out, cap);
}
/* Append `nb` to the target's next_link chain, walking to the tail and writing
 * the last block back. links.adf shows the shape for both of its links:
 * FILE.next_link -> LINKFILE -> 0, i.e. the chain hangs off the TARGET.
 *
 * The walk is strict (every pointer in range, readable, no self-loop) for the
 * same reason the unlink path is: a corrupt chain must be reported, not
 * papered over by writing somewhere plausible. */
static int chain_append(afs_volume_t *v, uint32_t target, uint32_t nb,
                        size_t bs, char *errbuf, size_t errlen) {
    uint32_t limit = afs_volume_block_count(v);
    uint8_t *cb = calloc(1, bs);
    uint32_t cur = target;
    uint32_t steps = 0;
    if (!cb) { lk_err(errbuf, errlen, "out of memory"); return -1; }

    for (;;) {
        if (cur >= limit || steps++ > limit) {
            lk_err(errbuf, errlen,
                   "target's next_link chain does not terminate (corrupt volume)");
            free(cb);
            return -1;
        }
        if (afs_volume_read_block(v, cur, cb, bs) != 0) {
            lk_err(errbuf, errlen, "cannot read next_link block %u", cur);
            free(cb);
            return -1;
        }
        uint32_t next = be32(cb + bs - 40);
        if (next == 0) {
            put_be32(cb + bs - 40, nb);
            put_be32(cb + 20, afs_block_checksum(cb, bs, 20));
            if (afs_volume_write_block(v, cur, cb, bs) != 0) {
                lk_err(errbuf, errlen, "cannot update the next_link chain");
                free(cb);
                return -1;
            }
            break;
        }
        if (next == cur) {
            lk_err(errbuf, errlen, "target's next_link chain is circular");
            free(cb);
            return -1;
        }
        cur = next;
    }
    free(cb);
    return 0;
}

/* Remove `linkblock` from its real entry's next_link chain -- the counterpart of
 * chain_append. Without it a removed hard link keeps counting towards nlink and
 * `ls -l` over-reports. A block that is not a hard link, or is not in the chain,
 * is a no-op success: the name is already gone from the count either way, which
 * is what the caller wanted. */
static void chain_unhook(afs_volume_t *v, uint32_t linkblock) {
    size_t bs = afs_volume_block_size(v);
    uint32_t limit = afs_volume_block_count(v);
    uint8_t *lb = calloc(1, bs), *cb = calloc(1, bs);
    uint32_t cur, steps = 0;
    if (!lb || !cb) { free(lb); free(cb); return; }

    if (afs_volume_read_block(v, linkblock, lb, bs) != 0) { free(lb); free(cb); return; }
    if (afs_block_kind(lb, bs) != AFS_BLOCK_LINKFILE) { free(lb); free(cb); return; }

    cur = be32(lb + bs - 44);                 /* real_entry */
    for (;;) {
        if (cur >= limit || steps++ > limit) break;       /* corrupt: stop */
        if (afs_volume_read_block(v, cur, cb, bs) != 0) break;
        uint32_t next = be32(cb + bs - 40);
        if (next == linkblock) {
            /* Splice, do not truncate: the removed link's own successor takes
             * its place. Zeroing here would silently drop every link after it
             * -- with the chain real -> one -> two, unlinking "one" must leave
             * real -> two, not real -> nothing. */
            uint32_t after = be32(lb + bs - 40);
            put_be32(cb + bs - 40, after);
            put_be32(cb + 20, afs_block_checksum(cb, bs, 20));
            (void)afs_volume_write_block(v, cur, cb, bs);
            break;
        }
        if (next == 0 || next == cur) break;              /* absent / circular */
        cur = next;
    }
    free(lb);
    free(cb);
}

int afs_file_link(afs_volume_t *v, uint32_t dirblock,
                  const char *name, size_t namelen, uint32_t target,
                  uint32_t *link_out, char *errbuf, size_t errlen) {
    size_t bs;
    uint32_t nb, nblocks;
    uint8_t *b = NULL;
    afs_dirent_t dup;

    if (!v || !name || !link_out) {
        lk_err(errbuf, errlen, "bad arguments");
        return -1;
    }
    /* Hard links are an FFS-only feature: an OFS volume has no link secondary
     * type, so writing one would produce a file no Amiga can read. */
    if (afs_volume_is_ofs(v)) {
        lk_err(errbuf, errlen,
               "hard links do not exist on an OFS volume (needs FFS)");
        return -1;
    }
    if (!afs_name_valid(name, namelen)) {
        lk_err(errbuf, errlen, "invalid name (1..30 bytes, no '/' or ':')");
        return -1;
    }
    /* Duplicate-check by the same lookup a later ls will use (INTL folds here) */
    if (afs_dir_lookup(v, dirblock, name, namelen, &dup) == 0) {
        lk_err(errbuf, errlen, "'%.*s' already exists", (int)namelen, name);
        return -1;
    }
    bs = afs_volume_block_size(v);
    nblocks = afs_volume_block_count(v);
    if (target == 0 || target >= nblocks) {
        lk_err(errbuf, errlen, "target block %u is outside the volume", target);
        return -1;
    }

    /* The target must be a PLAIN FILE. Linking a directory would create the
     * cycle C12 exists to avoid; linking a link would chain for no reason. */
    b = calloc(1, bs);
    if (!b) { lk_err(errbuf, errlen, "out of memory"); return -1; }
    if (afs_volume_read_block(v, target, b, bs) != 0) {
        free(b);
        lk_err(errbuf, errlen, "cannot read target block %u", target);
        return -1;
    }
    if (afs_block_kind(b, bs) != AFS_BLOCK_FILE) {
        free(b);
        lk_err(errbuf, errlen,
               "link target is not a plain file (a directory link would be a "
               "cycle, and a link to a link chains for nothing)");
        return -1;
    }
    free(b);

    if (afs_alloc_block(v, &nb, errbuf, errlen) != 0) return -1;
    b = calloc(1, bs);
    if (!b) { lk_err(errbuf, errlen, "out of memory"); return -1; }

    /* The block below is block 891 of links.adf, field for field: a hard link
     * written by a real AmigaDOS. It owns NO data -- byte_size 0, no extension
     * block -- and reaches the target's header through real_entry at BSIZE-44. */
    put_be32(b + 0, AFS_T_HEADER);
    put_be32(b + 4, nb);                       /* header_key: self-pointer */
    /* +8 high_seq, +12 ht_size, +16 data_size all 0; hash area stays zeroed */
    put_be16(b + bs - 196, 0);
    put_be16(b + bs - 194, 0);
    put_be32(b + bs - 192, 0);                 /* protect */
    put_be32(b + bs - 188, 0);                 /* byte_size: a link has no data */
    b[bs - 184] = 0;                           /* no comment */
    b[bs - 80] = (uint8_t)namelen;
    memcpy(b + bs - 79, name, namelen);
    put_be32(b + bs - 44, target);             /* real_entry (0x1d4) */
    put_be32(b + bs - 40, 0);                  /* next_link: chain_append fills it */
    put_be32(b + bs - 16, 0);                  /* no hash-chain successor yet */
    put_be32(b + bs - 12, dirblock);           /* parent */
    put_be32(b + bs - 8, 0);                   /* no extension */
    put_be32(b + bs - 4, AFS_ST_LINKFILE);
    put_be32(b + 20, afs_block_checksum(b, bs, 20));

    if (afs_volume_write_block(v, nb, b, bs) != 0) {
        free(b);
        lk_err(errbuf, errlen, "cannot write link block");
        return -1;
    }
    free(b);

    /* Chain first, directory entry second. A chain failure then leaves the
     * block unreachable rather than publishing a name whose nlink is wrong. */
    if (chain_append(v, target, nb, bs, errbuf, errlen) != 0) return -1;
    if (afs_dir_link_entry(v, dirblock, nb, errbuf, errlen) != 0) return -1;

    /* No dir-cache record: a hard link's size is the TARGET's, and the cache
     * stores the size it is given, so caching zero here would teach the cache
     * that this name is an empty file. The hash table is the authority. */
    *link_out = nb;
    return 0;
}

/* Called by afs_dir_unlink once a name has been spliced out, so a removed hard
 * link stops counting towards nlink. */
void afs_link_after_unlink(afs_volume_t *v, uint32_t entryblock) {
    if (v) chain_unhook(v, entryblock);
}

int afs_symlink(afs_volume_t *v, uint32_t dirblock,
                const char *name, size_t namelen,
                const char *target, size_t targetlen,
                uint32_t *link_out, char *errbuf, size_t errlen) {
    size_t bs, capacity;
    uint32_t nb;
    uint8_t *b;
    afs_dirent_t dup;

    if (!v || !name || !target || !link_out) {
        lk_err(errbuf, errlen, "bad arguments");
        return -1;
    }
    if (!afs_name_valid(name, namelen)) {
        lk_err(errbuf, errlen, "invalid name (1..30 bytes, no '/' or ':')");
        return -1;
    }
    /* Duplicate-check by the same lookup a later ls will use (INTL folds here) */
    if (afs_dir_lookup(v, dirblock, name, namelen, &dup) == 0) {
        lk_err(errbuf, errlen, "'%.*s' already exists", (int)namelen, name);
        return -1;
    }
    if (targetlen == 0) {
        lk_err(errbuf, errlen, "a soft link needs a non-empty target");
        return -1;
    }

    bs = afs_volume_block_size(v);
    /* The target lives in the hash-table area, which starts at 24 and runs to
     * BSIZE-192 (where the tail begins). Naming that boundary is what stops a
     * long target from silently overwriting the entry's own fields. */
    capacity = bs - 192 - 24;
    if (targetlen > capacity) {
        lk_err(errbuf, errlen,
               "target is %zu bytes; only %zu fit before the entry's own fields",
               targetlen, capacity);
        return -1;
    }

    if (afs_alloc_block(v, &nb, errbuf, errlen) != 0) return -1;
    b = calloc(1, bs);
    if (!b) { lk_err(errbuf, errlen, "out of memory"); return -1; }

    /* The block below is block 885 of testffs.adf, field for field. Note the
     * target is COPIED INTO A ZEROED BLOCK and never NUL-appended: AmigaDOS
     * pads with zeros, and for a short target "x\0" and "x" are indistinguishable
     * here -- which is exactly why a short-target test cannot prove this. */
    put_be32(b + 0, AFS_T_HEADER);
    put_be32(b + 4, nb);                       /* header_key: self-pointer */
    /* +8 high_seq, +12 ht_size, +16 data_size all 0 */
    memcpy(b + 24, target, targetlen);         /* the path, zero-padded after it */
    put_be16(b + bs - 196, 0);
    put_be16(b + bs - 194, 0);
    put_be32(b + bs - 192, 0);                 /* protect */
    put_be32(b + bs - 188, 0);                 /* byte_size: a link has no data */
    b[bs - 184] = 0;                           /* no comment */
    b[bs - 80] = (uint8_t)namelen;
    memcpy(b + bs - 79, name, namelen);
    put_be32(b + bs - 44, 0);                  /* real_entry: not a hard link */
    put_be32(b + bs - 40, 0);                  /* next_link */
    put_be32(b + bs - 16, 0);                  /* no hash-chain successor yet */
    put_be32(b + bs - 12, dirblock);           /* parent */
    put_be32(b + bs - 8, 0);                   /* no extension */
    put_be32(b + bs - 4, AFS_ST_SOFTLINK);
    put_be32(b + 20, afs_block_checksum(b, bs, 20));

    if (afs_volume_write_block(v, nb, b, bs) != 0) {
        free(b);
        lk_err(errbuf, errlen, "cannot write soft-link block");
        return -1;
    }
    free(b);
    if (afs_dir_link_entry(v, dirblock, nb, errbuf, errlen) != 0) return -1;

    /* The dir-cache record. My first version of this function wrote NONE, with
     * a comment arguing that a size of zero "would teach the cache this name is
     * an empty file". That comment was REASONING, and the oracle contradicted
     * it: `adfls` listed every other entry on the volume and none of the soft
     * links this function created. Ground truth from the fixture settles it --
     * the root's dir-cache block 881 carries, for the volume's OWN soft link
     * `slink_dir1`:
     *
     *     header=885  size=0  sec_type=3  name=slink_dir1
     *
     * size 0 is exactly what a real AmigaDOS writes for a soft link, and the
     * record is what makes the entry visible to a reader that consults the
     * cache. The hash table alone is NOT sufficient -- on a DOS\5 volume it is
     * the cache that an outside implementation walks.
     *
     * (This is rule 257 wearing a hat: do not write a constraint from
     * reasoning about a format. Ask, or read what a real implementation wrote.) */
    afs_dircache_rec_t rec;
    memset(&rec, 0, sizeof rec);
    rec.header = nb;
    rec.size = 0;
    rec.sec_type = (uint8_t)AFS_ST_SOFTLINK;
    memcpy(rec.name, name, namelen < 30 ? namelen : 30);
    if (afs_dircache_upsert(v, dirblock, &rec, NULL, 0, errbuf, errlen) != 0)
        return -1;

    *link_out = nb;
    return 0;
}

int afs_link_remove(afs_volume_t *v, uint32_t dirblock,
                    const char *name, size_t namelen,
                    char *errbuf, size_t errlen) {
    uint32_t gone = 0;
    afs_dirent_t e;

    if (!v || !name) { lk_err(errbuf, errlen, "bad arguments"); return -1; }
    if (afs_dir_lookup(v, dirblock, name, namelen, &e) != 0) {
        lk_err(errbuf, errlen, "'%.*s' not found", (int)namelen, name);
        return -1;
    }
    /* Only a LINK entry. A file goes through afs_file_delete (which frees a
     * data-block chain) and a directory through afs_dir_rmdir (which frees its
     * dir-cache chain). This frees ONE block -- the link's own header -- and
     * the generic sequence is right for both link kinds. */
    if (e.kind != AFS_BLOCK_SOFTLINK && e.kind != AFS_BLOCK_LINKFILE) {
        lk_err(errbuf, errlen,
               "'%.*s' is not a link; use the file or directory remover",
               (int)namelen, name);
        return -1;
    }

    /* 1. the name. After this the entry is unreachable whatever follows. */
    if (afs_dir_unlink(v, dirblock, name, namelen, &gone, errbuf, errlen) != 0)
        return -1;
    if (gone != e.block) {
        lk_err(errbuf, errlen, "unlink raced lookup (internal)");
        return -1;
    }
    /* 2. the dir-cache record. On a DOS\5 volume this is what an outside
     *    implementation reads, so leaving it would keep the entry VISIBLE to
     *    ADFlib after our own reader had dropped the name -- a volume that
     *    looks intact here and wrong to the oracle (M31). */
    if (afs_dircache_del(v, dirblock, gone, errbuf, errlen) != 0) return -1;
    /* 3. the block itself. This is the step both shells used to omit. */
    if (afs_free_block(v, gone, errbuf, errlen) != 0) return -1;
    return 0;
}
