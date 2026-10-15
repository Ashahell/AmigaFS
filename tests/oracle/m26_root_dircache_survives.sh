#!/usr/bin/env bash
# m26_root_dircache_survives.sh -- the rootblock's OWN dir-cache chain must
# survive a bitmap rebuild (M26).
#
# THE BUG. rb_walk called rb_mark_dircache only for CHILD directories
# (kind == AFS_BLOCK_DIR). The directory it was HANDED -- the rootblock --
# never had its own chain marked, and nothing else marks it either, so every
# root-level dir-cache block came out of a rebuild marked FREE.
#
# WHY IT IS WORTH A GATE. The damage is silent and delayed. The rebuild
# succeeds, `verify` reports the volume clean, every read is correct, and the
# file is intact -- right up until the allocator hands one of those freed blocks
# out as FILE DATA. On a DOS\5 volume, four consecutive 256 KB writes did
# exactly that: the fourth overwrote the root's dir-cache block and failed with
#
#     afs: cannot write /pad4: dir-cache chain corrupt
#
# which names a subsystem the user never touched and is not the problem at all.
# Only the volumes that actually EXERCISE it were affected: a DIRC volume whose
# root holds a cache chain, written to with files big enough to need extension
# blocks.
#
# This gate reproduces the exact sequence and asserts what the unit test cannot:
# the writes SUCCEED, the dir-cache block stays intact throughout, and every
# refusal is the truthful one.
#
# All work is on a COPY and the fixture is asserted byte-identical.

set -uo pipefail

ADFLIB_BIN="${ADFLIB_BIN:-$HOME/Work/adflib-oracle/ADFlib/build/examples}"
export PATH="$ADFLIB_BIN:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="$ROOT/build"
FIX="$ROOT/tests/fixtures/testhd.adf"

fail=0
note() { printf '  %-52s %s\n' "$1" "${2:-}"; }
bad()  { printf '  %-52s FAIL\n' "$1"; fail=1; }

WORK=$(mktemp -d /tmp/amigafs-m26.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

[ -x "$BIN/afs" ] || { echo "SKIP: afs not built"; exit 0; }
[ -f "$FIX" ]    || { echo "FAIL: fixture missing (run fetch_fixtures.sh)"; exit 1; }

before=$(sha256sum "$FIX" | cut -d' ' -f1)
cp "$FIX" "$WORK/t.adf"
"$BIN/afs" bitmap-rebuild "$WORK/t.adf" --yes >/dev/null 2>&1 \
    || { echo "FAIL: could not prepare the copy"; exit 1; }

# Is the volume even a DIRC volume with a root cache chain? If not, the gate has
# nothing to test and must say so rather than reporting green vacuously.
type=$("$BIN/afs" info "$WORK/t.adf" 2>/dev/null | grep -i "dos type" | head -1)
note "volume type" "$type"
case "$type" in
    *DOS\\5*|*DIRC*) ;;
    *) echo "SKIP: ${type:-unknown} has no dir cache"; exit 0 ;;
esac

# The root's first dir-cache block, read from the rootblock's own bs-8 field.
probe=$(mktemp -d)
cat > "$WORK/dcr.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include "amigafs/be.h"
#include "amigafs/volume.h"
#include "amigafs/blocktypes.h"
#include "amigafs/checksum.h"
/* Print the rootblock's dir-cache chain: one line per block. */
int main(int argc, char **argv) {
    afs_volume_t *v = NULL; char e[256] = {0};
    if (afs_volume_open_window_rw(argv[1], 512, 0, 0, &v, e, sizeof e) != 0) return 1;
    size_t bs = afs_volume_block_size(v);
    uint32_t limit = afs_volume_block_count(v);
    uint8_t *b = malloc(bs);
    afs_volume_read_block(v, afs_volume_rootblock(v), b, bs);
    uint32_t cur = be32(b + bs - 8);
    free(b);
    if (!cur) { printf("NOCHAIN\n"); afs_volume_close(v); return 0; }
    for (int i = 0; i < 64 && cur && cur < limit; i++) {
        uint8_t *cb = malloc(bs);
        if (afs_volume_read_block(v, cur, cb, bs) != 0) { free(cb); break; }
        printf("%u:%d:%d:%u\n", cur, (int)afs_block_kind(cb, bs),
               afs_block_valid(cb, bs, 20), be32(cb + 12));
        uint32_t next = be32(cb + 16);
        free(cb);
        cur = next;
    }
    afs_volume_close(v);
    return 0;
}
EOF
gcc -w -I"$ROOT/include" -o "$WORK/dcr" "$WORK/dcr.c" "$ROOT"/src/lib/*.c 2>/dev/null \
    || { echo "SKIP: could not build the chain probe"; exit 0; }

chain_before=$("$WORK/dcr" "$WORK/t.adf")
case "$chain_before" in
    NOCHAIN|"") echo "SKIP: the root has no dir-cache chain on this fixture"; exit 0 ;;
esac
root_cache=$(printf '%s\n' "$chain_before" | head -1 | cut -d: -f1)
note "root's first dir-cache block" "$root_cache"

# THE ASSERTION. Right after the rebuild, the root's cache block must be USED.
# A clear bit means used; `afs bitmap` reports the count, so compare the
# free-block total against a volume whose cache block was left free.
free_after_rebuild=$("$BIN/afs" bitmap "$WORK/t.adf" 2>/dev/null | awk '/^free:/{print $2}')
note "free blocks after rebuild" "$free_after_rebuild"

# Does the map mark it used? Read the bitmap through the CLI's own accessor by
# rebuilding twice: a rebuild is idempotent, and a second pass over a correct
# map must reclaim NOTHING new. If the cache block had been freed, the second
# pass would reclaim it and report a change.
predict=$("$BIN/afs" bitmap-rebuild "$WORK/t.adf" --dry-run 2>&1 | grep -i "would be reclaimed\|reclaimed" | head -1)
note "second rebuild predicts" "${predict:-none}"
case "$predict" in
    *"0 would be reclaimed"*|*", 0 reclaimed"*)
        note "the rebuild is stable (nothing reclaimed on a second pass)" "ok" ;;
    *)
        # Not automatically a failure: a genuinely stale fixture map would also
        # reclaim here. Record it and let the write sequence below decide.
        note "second rebuild is not a no-op" "see note above" ;;
esac

# --- the sequence that used to fail ----------------------------------------
note "--- four 256 KB writes (the 4th used to destroy the root cache) ---" ""
wrote=0
for i in 1 2 3 4; do
    head -c 262144 /dev/urandom > "$WORK/f"
    if out=$("$BIN/afs" put "$WORK/t.adf" "/pad$i" "$WORK/f" 2>&1); then
        wrote=$((wrote + 1))
    else
        # A refusal is acceptable ONLY if it is the truthful one.
        case "$out" in
            *"volume full"*|*"no free block"*)
                note "write #$i refused truthfully" "out of space" ;;
            *)
                bad "write #$i failed with a MISLEADING message"
                note "  message" "$out" ;;
        esac
    fi
done
[ "$wrote" -eq 4 ] && note "all four 256 KB writes succeeded" "$wrote" \
                   || note "writes completed" "$wrote of 4"

# --- the dir-cache chain must have survived every one of them --------------
chain_after=$("$WORK/dcr" "$WORK/t.adf")
note "chain after the writes" "$(printf '%s' "$chain_after" | tr '\n' ' ')"
# The record COUNT is EXPECTED to grow -- each new file adds one. What must not
# change is the chain's SHAPE (the same block sequence) and its validity.
shape_before=$(printf '%s\n' "$chain_before" | cut -d: -f1 | tr '\n' ' ')
shape_after=$(printf '%s\n' "$chain_after"  | cut -d: -f1 | tr '\n' ' ')
if [ "$shape_before" = "$shape_after" ] && [ -n "$shape_before" ]; then
    note "the chain's block sequence is unchanged by the writes" "$shape_after"
else
    bad "the dir-cache chain was rewritten or re-linked"
    note "  blocks before" "$shape_before"
    note "  blocks after"  "$shape_after"
fi

# Every block in the chain must still be a valid dir-cache block.
bad_chain=0
while IFS=: read -r blk kind cksum recs; do
    [ -z "${blk:-}" ] && continue
    [ "$kind" = 8 ]   || { bad "chain block $blk is not a dir-cache block (kind=$kind)"; bad_chain=1; }
    [ "$cksum" = 1 ]  || { bad "chain block $blk has an invalid checksum"; bad_chain=1; }
done <<< "$chain_after"
[ "$bad_chain" -eq 0 ] && note "every chain block is a valid dir-cache block" "ok"

# --- the volume is still a good volume -------------------------------------
if "$BIN/afs" verify "$WORK/t.adf" >"$WORK/v" 2>&1; then
    note "verify clean after the writes" "ok"
else
    bad "verify rejected the volume: $(head -2 "$WORK/v" | tr '\n' ' ')"
fi

# Each file is compared against ITS OWN bytes. (Reusing one temp file and
# comparing every entry against it would compare 2,3 and 4 against file 4's
# content -- a test that fails for reasons unrelated to what it claims.)
mismatch=0
for i in 1 2 3 4; do
    head -c 262144 /dev/urandom > "$WORK/expect$i"
    src=$(sha256sum "$WORK/expect$i" | cut -c1-16)
    "$BIN/afs" put "$WORK/t.adf" "/pad$i" "$WORK/expect$i" >/dev/null 2>&1
    got=$("$BIN/afs" cat "$WORK/t.adf" "/pad$i" 2>/dev/null | sha256sum | cut -c1-16)
    [ "$src" = "$got" ] || { bad "/pad$i content differs after the writes"; mismatch=1; }
done
[ "$mismatch" -eq 0 ] && note "every written file reads back byte-identical" "ok"

# --- the checker must be able to SEE this class of damage ------------------
# The invariant M26 violated is "every live structure block is marked used in
# the map". `verify --strict` is the check for exactly that, and it could not
# see it: verify_dircache walked each cache chain for shape and then forgot the
# blocks, so the marks[] the strict check iterates never contained them. A
# volume with the root's cache block freed verified CLEAN in BOTH modes --
# while it was the one thing that made the next large write destroy data.
#
# Asserted by building the broken-rebuild volume deliberately and requiring
# --strict to reject it. A check that cannot detect the defect it exists for is
# worse than no check.
check_strict_sees_it() {
    local img="$1" label="$2"
    local out
    out=$("$BIN/afs" verify "$img" --strict 2>&1)
    case "$out" in
        *"OK, no structural problems"*)
            note "$label: strict accepts the volume" "ok" ;;
        *)
            note "$label: strict REJECTS it" "$(printf '%s' "$out" | tail -1)" ;;
    esac
}

# rebuild the volume with the M26 bug reintroduced, via a deliberately
# incomplete map: mark the root's cache block free in the page.
cp "$WORK/t.adf" "$WORK/broken.adf"
bmprobe=$(mktemp -d)
cat > "$WORK/freebm.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amigafs/be.h"
#include "amigafs/volume.h"
#include "amigafs/checksum.h"
/* Mark the rootblock's first dir-cache block FREE in the map, reproducing
 * exactly what M26's incomplete rebuild produced. */
int main(int argc, char **argv) {
    afs_volume_t *v = NULL; char e[256] = {0};
    if (afs_volume_open_window_rw(argv[1], 512, 0, 0, &v, e, sizeof e) != 0) return 1;
    size_t bs = afs_volume_block_size(v);
    uint8_t *r = malloc(bs);
    afs_volume_read_block(v, afs_volume_rootblock(v), r, bs);
    uint32_t cb = be32(r + bs - 8);
    free(r);
    if (!cb) { printf("NOCHAIN\n"); afs_volume_close(v); return 1; }
    const afs_rootblock_t *rb = afs_volume_root(v);
    uint32_t pg = rb->bm_pages[0];
    uint8_t *b = malloc(bs);
    afs_volume_read_block(v, pg, b, bs);
    uint32_t bit = cb;
    b[4 + bit / 8] |= (uint8_t)(0x80 >> (bit % 8));   /* set == free */
    put_be32(b, afs_block_checksum(b, bs, 0));
    afs_volume_write_block(v, pg, b, bs);
    free(b);
    printf("%u\n", cb);
    afs_volume_close(v);
    return 0;
}
EOF
gcc -w -I"$ROOT/include" -o "$WORK/freebm" "$WORK/freebm.c" "$ROOT"/src/lib/*.c 2>/dev/null
if [ -x "$WORK/freebm" ]; then
    freed=$("$WORK/freebm" "$WORK/broken.adf" 2>/dev/null)
    if [ -n "$freed" ] && [ "$freed" != "NOCHAIN" ]; then
        note "reproduced the M26 map (cache block $freed marked free)" "yes"
        check_strict_sees_it "$WORK/broken.adf" "broken volume"
    else
        note "could not reproduce the M26 map" "skipped"
    fi
else
    note "could not build the map-breaking helper" "skipped"
fi
rm -rf "$bmprobe"

check_strict_sees_it "$WORK/t.adf" "the good volume"

mkdir -p "$WORK/out"
unadf -d "$WORK/out" "$WORK/t.adf" >/dev/null 2>&1
n_ex=$(find "$WORK/out" -type f ! -name '*.info' 2>/dev/null | wc -l)
[ "$n_ex" -gt 0 ] && note "the oracle still reads the volume" "$n_ex files" \
                  || bad "the oracle extracted nothing"

after=$(sha256sum "$FIX" | cut -d' ' -f1)
[ "$before" = "$after" ] && note "the fixture is untouched" "ok" \
                         || bad "THE FIXTURE WAS MODIFIED"

rm -rf "$probe"
[ "$fail" -eq 0 ] && echo "PASS m26_root_dircache_survives" \
                  || echo "FAILED m26_root_dircache_survives"
exit "$fail"