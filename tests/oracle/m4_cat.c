/* M4 gate: extract every file on every fixture and print sha256, so the shell
 * gate can diff our bytes against ADFlib's unadf extraction.
 *
 * Output: <relative-path>\t<sha256>\t<size>   (one line per file)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigafs/dir.h"
#include "amigafs/file.h"

/* A tiny sha256 so the gate needs no external library. */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha_t;

static uint32_t rr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void sha_block(sha_t *s, const uint8_t *p);
static void sha_init(sha_t *s) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, iv, sizeof iv); s->len = 0; s->n = 0;
}
static void sha_update(sha_t *s, const uint8_t *p, size_t n) {
    s->len += n;
    while (n) {
        size_t take = 64 - s->n; if (take > n) take = n;
        memcpy(s->buf + s->n, p, take); s->n += take; p += take; n -= take;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}
static void sha_final(sha_t *s, char *out) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->n != 56) sha_update(s, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, lb, 8);
    for (int i = 0; i < 8; i++) sprintf(out + i * 8, "%08x", s->h[i]);
    out[64] = 0;
}
static void sha_block(sha_t *s, const uint8_t *p) {
    static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rr(w[i-15],7)^rr(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = rr(w[i-2],17)^rr(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],hh=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1=rr(e,6)^rr(e,11)^rr(e,25);
        uint32_t ch=(e&f)^((~e)&g);
        uint32_t t1=hh+S1+ch+K[i]+w[i];
        uint32_t S0=rr(a,2)^rr(a,13)^rr(a,22);
        uint32_t maj=(a&b)^(a&c)^(b&c);
        uint32_t t2=S0+maj;
        hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=hh;
}

static void walk(afs_volume_t *v, uint32_t dirblk, const char *prefix, int depth) {
    if (depth > 4) return;
    afs_dirent_t ents[256];
    size_t n = 0;
    if (afs_dir_list(v, dirblk, ents, 256, &n) != 0) return;
    for (size_t i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", prefix, ents[i].name);
        if (ents[i].kind == AFS_BLOCK_DIR || ents[i].kind == AFS_BLOCK_LINKDIR) {
            walk(v, ents[i].block, path, depth + 1);
        } else if (ents[i].kind == AFS_BLOCK_FILE || ents[i].kind == AFS_BLOCK_LINKFILE) {
            /* ADFlib writes a ".info" sidecar for entries that carry a
             * comment. On some of these images such a file ALSO exists as a
             * real volume file, so unadf's sidecar and the real file collide.
             * The oracle side filters .info away; we do the same so the two
             * lists describe the same set of data files. */
            size_t nl = strlen(ents[i].name);
            if (nl > 5 && strcmp(ents[i].name + nl - 5, ".info") == 0) continue;
            void *buf = NULL; size_t len = 0;
            if (afs_file_read_all(v, dirblk, ents[i].name, strlen(ents[i].name),
                                  &buf, &len) == 0) {
                sha_t s; sha_init(&s); sha_update(&s, (const uint8_t *)buf, len);
                char hex[65]; sha_final(&s, hex);
                printf("%s\t%s\t%zu\n", path, hex, len);
                free(buf);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: m4_cat <image>...\n"); return 2; }
    for (int a = 1; a < argc; a++) {
        afs_volume_t *v = NULL; char err[256];
        if (afs_volume_open_file(argv[a], 512, 0, &v, err, sizeof err) != 0) continue;
        walk(v, afs_volume_rootblock(v), "", 0);
        afs_volume_close(v);
    }
    return 0;
}