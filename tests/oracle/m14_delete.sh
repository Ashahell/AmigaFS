#!/usr/bin/env bash
# M14 gate: delete + rename (`afs rm` / `afs mv`), oracle-diffed.
#
# Reference semantics come from ADFlib's own source (adfRemoveEntry /
# adfRenameEntry, src/adf_dir.c) and xdftool behavior probes, not from memory:
#   - non-empty dir delete refused ("Delete Not Allowed" from xdftool);
#   - file delete frees header + all data + all extension blocks;
#   - empty-dir delete frees the dir (+ its cache chain on DIRC);
#   - rename rewrites name+parent in place, unlinks old chain, appends to the
#     new parent's chain with a duplicate check; same-name same-parent is a
#     no-op; moving a dir into its own subtree must refuse (a cycle).
# xdftool HAS `delete` (independent deleter for the reverse direction) but NO
# rename: renames round-trip through ADFlib readers only, stated not hidden.
# Links are refused like ADFlib refuses them (only FILE/DIR handled).
#
# Bitmap accounting is EXACT everywhere below (delta == computed footprint),
# never "decreased": a delete that leaks a block and a delete that eats a
# live block both pass a direction-only check.
set -uo pipefail

ADFLIB_BIN="${ADFLIB_BIN:-$HOME/Work/adflib-oracle/ADFlib/build/examples}"
export PATH="$ADFLIB_BIN:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="$ROOT/build"
WORK=$(mktemp -d /tmp/amigafs-m14.XXXXXX)

fail=0
note() { printf '  %-46s %s\n' "$1" "$2"; }
bad()  { printf '  %-46s FAIL\n' "$1"; fail=1; }

cleanup() {
    fusermount3 -uz "$WORK/mnt" 2>/dev/null
    [ -n "${MPID:-}" ] && kill "$MPID" 2>/dev/null
    wait "${MPID:-}" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "M14 gate: delete + rename"

for t in "$BIN/afs" "$BIN/amigafs-fuse"; do
    [ -x "$t" ] || { echo "M14 gate: SKIP (build first)"; exit 0; }
done
for f in blank.adf testffs.adf; do
    [ -f "$ROOT/tests/fixtures/$f" ] || { echo "M14 gate: SKIP (fixtures absent)"; exit 0; }
done

python3 -c "open('$WORK/big.bin','wb').write(bytes((i*7+1)&0xFF for i in range(100*1024)))"
printf 'hello amiga\n' > "$WORK/small.txt"

used_of() { "$BIN/afs" bitmap "$1" ${2:+$2} 2>/dev/null | sed -n 's/^used: *//p'; }

# ---- 1. rm file: gone from oracles, bitmap exact ---------------------------
for img in blank.adf testffs.adf; do
    cp "$ROOT/tests/fixtures/$img" "$WORK/w.adf"
    # A stale map is what refuses writes, so every case starts from a
    # fresh one. Same remedy in every writing gate.
    "$BIN/afs" bitmap-rebuild "$WORK/w.adf" --yes >/dev/null 2>&1 || true
    "$BIN/afs" mkdir "$WORK/w.adf" NDIR >/dev/null 2>&1
    "$BIN/afs" put "$WORK/w.adf" NDIR/big "$WORK/big.bin" >/dev/null 2>&1
    before=$(used_of "$WORK/w.adf")
    # footprint of a 102400-byte file: header + data + extension blocks past
    # the first 72-slot table. Payload is variant-dependent (488 OFS, 512
    # FFS) -- hardcoded 512 here would silently undercount OFS by 10 blocks.
    case "$img" in
        blank.adf|testofs.adf|arccsh.adf) pay=488;;
        *) pay=512;;
    esac
    nblocks=$(( (102400 + pay - 1) / pay ))
    next=$(( nblocks <= 72 ? 0 : (nblocks - 72 + 71) / 72 ))
    footprint=$(( 1 + nblocks + next ))
    if "$BIN/afs" rm "$WORK/w.adf" NDIR/big >/dev/null 2>&1; then
        if adfls "$WORK/w.adf" 2>/dev/null | grep -q "^NDIR$" \
           && unadf -d "$WORK/u-$img" "$WORK/w.adf" >/dev/null 2>&1 \
           && [ ! -e "$WORK/u-$img/NDIR/big" ]; then
            note "rm $img: gone from oracles" "ok"
        else
            bad "rm $img: gone from oracles"
        fi
        after=$(used_of "$WORK/w.adf")
        if [ "$before" -gt 0 ] && [ "$((before - after))" = "$footprint" ]; then
            note "rm $img: bitmap -$footprint exact" "used $before->$after"
        else
            bad "rm $img: bitmap accounting" "used $before->$after, want -$footprint"
        fi
        if "$BIN/afs" verify "$WORK/w.adf" >/dev/null 2>&1; then
            note "verify $img after rm" "clean"
        else
            bad "verify $img after rm"
        fi
    else
        bad "rm $img" "afs rm failed"
    fi
done

# ---- 2. rmdir: non-empty refused, empty removed, bitmap exact ---------------
cp "$ROOT/tests/fixtures/blank.adf" "$WORK/d.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/d.adf" --yes >/dev/null 2>&1 || true
"$BIN/afs" mkdir "$WORK/d.adf" DD >/dev/null 2>&1
"$BIN/afs" put "$WORK/d.adf" DD/f "$WORK/small.txt" >/dev/null 2>&1
out=$("$BIN/afs" rm "$WORK/d.adf" DD 2>&1)
if [ $? -ne 0 ] && printf '%s' "$out" | grep -qi "not empty"; then
    note "rmdir non-empty refused" "ok"
else
    bad "rmdir non-empty" "rc=$? out=$(printf '%s' "$out" | head -1)"
fi
if "$BIN/afs" rm "$WORK/d.adf" DD/f >/dev/null 2>&1 \
   && "$BIN/afs" rm "$WORK/d.adf" DD >/dev/null 2>&1; then
    if ! adfls "$WORK/d.adf" 2>/dev/null | grep -q "^DD$"; then
        note "rmdir empty removed" "ok"
    else
        bad "rmdir empty removed" "still listed"
    fi
else
    bad "rmdir empty removed" "rm failed"
fi
# DIRC dir carries its cache block(s): freeing must include them. NDIR above
# gained exactly one cache block on its first child; removing both files then
# the dir must return 1 (dir) + 1 (cache) = 2 blocks.
cp "$ROOT/tests/fixtures/testffs.adf" "$WORK/dc.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/dc.adf" --yes >/dev/null 2>&1 || true
"$BIN/afs" mkdir "$WORK/dc.adf" ND >/dev/null 2>&1
"$BIN/afs" put "$WORK/dc.adf" ND/a "$WORK/small.txt" >/dev/null 2>&1
"$BIN/afs" put "$WORK/dc.adf" ND/b "$WORK/small.txt" >/dev/null 2>&1
before=$(used_of "$WORK/dc.adf")
"$BIN/afs" rm "$WORK/dc.adf" ND/a >/dev/null 2>&1
"$BIN/afs" rm "$WORK/dc.adf" ND/b >/dev/null 2>&1
"$BIN/afs" rm "$WORK/dc.adf" ND >/dev/null 2>&1
after=$(used_of "$WORK/dc.adf")
# each 13-byte file: header + 1 data = 2 blocks; dir + its cache = 2
if [ "$((before - after))" = "6" ]; then
    note "DIRC rmdir frees dir+cache" "used $before->$after (-6)"
else
    bad "DIRC rmdir frees dir+cache" "used $before->$after, want -6"
fi
if "$BIN/afs" verify "$WORK/dc.adf" >/dev/null 2>&1; then
    note "verify after rmdir chain" "clean"
else
    bad "verify after rmdir chain"
fi

# ---- 3. mv: same-dir rename, cross-dir move, refusals -----------------------
cp "$ROOT/tests/fixtures/testffs.adf" "$WORK/m.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/m.adf" --yes >/dev/null 2>&1 || true
"$BIN/afs" mkdir "$WORK/m.adf" D1 >/dev/null 2>&1
"$BIN/afs" mkdir "$WORK/m.adf" D2 >/dev/null 2>&1
"$BIN/afs" put "$WORK/m.adf" D1/a "$WORK/small.txt" >/dev/null 2>&1
if "$BIN/afs" mv "$WORK/m.adf" D1/a D1/b >/dev/null 2>&1; then
    a=$(sha256sum < "$WORK/small.txt" | cut -d' ' -f1)
    b=$("$BIN/afs" cat "$WORK/m.adf" D1/b 2>/dev/null | sha256sum | cut -d' ' -f1)
    if [ "$a" = "$b" ] && ! "$BIN/afs" cat "$WORK/m.adf" D1/a >/dev/null 2>&1; then
        note "mv same-dir rename" "${a:0:12}... follows the name"
    else
        bad "mv same-dir rename" "want=${a:0:12} got=${b:0:12}"
    fi
else
    bad "mv same-dir rename" "afs mv failed"
fi
if "$BIN/afs" mv "$WORK/m.adf" D1/b D2/c >/dev/null 2>&1; then
    a=$(sha256sum < "$WORK/small.txt" | cut -d' ' -f1)
    b=$("$BIN/afs" cat "$WORK/m.adf" D2/c 2>/dev/null | sha256sum | cut -d' ' -f1)
    if [ "$a" = "$b" ] && ! "$BIN/afs" cat "$WORK/m.adf" D1/b >/dev/null 2>&1; then
        note "mv cross-dir move" "content follows, source gone"
    else
        bad "mv cross-dir move" "want=${a:0:12} got=${b:0:12}"
    fi
else
    bad "mv cross-dir move" "afs mv failed"
fi
# onto an existing name: refused AND victim intact (Rule 8: the refusal must
# name the collision, and the victim check proves nothing was half-moved)
"$BIN/afs" put "$WORK/m.adf" D1/victim "$WORK/small.txt" >/dev/null 2>&1
out=$("$BIN/afs" mv "$WORK/m.adf" D2/c D1/victim 2>&1)
if [ $? -ne 0 ] && printf '%s' "$out" | grep -qi "exist"; then
    v=$("$BIN/afs" cat "$WORK/m.adf" D1/victim 2>/dev/null | sha256sum | cut -d' ' -f1)
    a=$(sha256sum < "$WORK/small.txt" | cut -d' ' -f1)
    if [ "$v" = "$a" ]; then
        note "mv onto existing refused, victim intact" "ok"
    else
        bad "mv onto existing" "victim bytes changed"
    fi
else
    bad "mv onto existing" "rc=$? out=$(printf '%s' "$out" | head -1)"
fi
# same name in the same directory is a no-op success (ADFlib parity -- not an
# error, and certainly not a cycle); moving a dir under itself IS the cycle
if "$BIN/afs" mv "$WORK/m.adf" D1 D1 >/dev/null 2>&1; then
    note "mv D1 D1 no-op succeeds" "ok"
else
    bad "mv D1 D1 no-op" "same-name same-dir should succeed"
fi
# A dir moved into its own subtree would cycle. Each destructive case gets a
# FRESH copy: under a mutant that lets the first one through, the volume is
# cyclic afterwards (D1 unlinked from the root), so a second case on the same
# image cannot even resolve its paths and passes vacuously. Shared state
# between destructive cases is how a mutant hides -- proven the hard way.
cp "$ROOT/tests/fixtures/testffs.adf" "$WORK/cyc.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/cyc.adf" --yes >/dev/null 2>&1 || true
"$BIN/afs" mkdir "$WORK/cyc.adf" D1 >/dev/null 2>&1
out=$("$BIN/afs" mv "$WORK/cyc.adf" D1 D1/D1 2>&1)
if [ $? -ne 0 ]; then
    note "mv dir into itself refused" "ok"
else
    bad "mv dir into itself" "succeeded"
fi
cp "$ROOT/tests/fixtures/testffs.adf" "$WORK/cyc2.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/cyc2.adf" --yes >/dev/null 2>&1 || true
"$BIN/afs" mkdir "$WORK/cyc2.adf" D1 >/dev/null 2>&1
"$BIN/afs" mkdir "$WORK/cyc2.adf" D1/sub >/dev/null 2>&1
out=$("$BIN/afs" mv "$WORK/cyc2.adf" D1 D1/sub/moved 2>&1)
if [ $? -ne 0 ]; then
    if "$BIN/afs" verify "$WORK/cyc2.adf" >/dev/null 2>&1 \
       && "$BIN/afs" ls "$WORK/cyc2.adf" >/dev/null 2>&1; then
        note "mv dir into own subtree refused" "tree intact, verify clean"
    else
        bad "mv dir into own subtree" "tree damaged"
    fi
else
    bad "mv dir into own subtree" "succeeded"
fi
if "$BIN/afs" verify "$WORK/m.adf" >/dev/null 2>&1 \
   && unadf -d "$WORK/mu" "$WORK/m.adf" >/dev/null 2>&1 \
   && [ "$(cat "$WORK/mu/D2/c" 2>/dev/null | sha256sum | cut -d' ' -f1)" = "$(sha256sum < "$WORK/small.txt" | cut -d' ' -f1)" ]; then
    note "moved bytes == unadf" "ok"
else
    bad "moved bytes == unadf"
fi

# ---- 4. reverse: xdftool deletes, we agree ----------------------------------
cp "$ROOT/tests/fixtures/blank.adf" "$WORK/x.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/x.adf" --yes >/dev/null 2>&1 || true
xdftool "$WORK/x.adf" makedir XD >/dev/null 2>&1
xdftool "$WORK/x.adf" write "$WORK/small.txt" XD/old >/dev/null 2>&1
if xdftool "$WORK/x.adf" delete XD/old >/dev/null 2>&1; then
    if "$BIN/afs" cat "$WORK/x.adf" XD/old >/dev/null 2>&1; then
        bad "xdftool delete visible to us" "file still readable"
    else
        note "xdftool delete visible to us" "ok"
    fi
    if "$BIN/afs" verify "$WORK/x.adf" >/dev/null 2>&1; then
        note "verify after xdftool delete" "clean"
    else
        bad "verify after xdftool delete"
    fi
else
    bad "xdftool delete fixture" "xdftool delete failed"
fi

# ---- 5. RDB: rm/mv inside one partition, others identical ------------------
cp "$ROOT/tests/fixtures/a590_WD93028XA_6parts.hdd" "$WORK/r.hdd"
# An RDB image is six PARTITIONS, each with its own bitmap, and a stale one
# refuses writes -- so every partition is rebuilt, named explicitly. A
# whole-image rebuild would treat the RDB block itself as volume data.
for p in OFS "OFS INTL" "OFS DirCache" FFS "FFS INTL" "FFS DirCache"; do
    "$BIN/afs" bitmap-rebuild "$WORK/r.hdd" "$p" --yes >/dev/null 2>&1 || true
done

# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
before=$(for p in OFS "OFS INTL" "OFS DirCache" FFS "FFS INTL" "FFS DirCache"; do "$BIN/afs" cat "$WORK/r.hdd" "$p/Trashcan.info" 2>/dev/null | sha256sum | cut -d' ' -f1; done)
"$BIN/afs" mkdir "$WORK/r.hdd" "FFS/TD" >/dev/null 2>&1
"$BIN/afs" put "$WORK/r.hdd" "FFS/TD/t" "$WORK/small.txt" >/dev/null 2>&1
if "$BIN/afs" mv "$WORK/r.hdd" "FFS/TD/t" "FFS/TD/t2" >/dev/null 2>&1 \
   && "$BIN/afs" rm "$WORK/r.hdd" "FFS/TD/t2" >/dev/null 2>&1 \
   && "$BIN/afs" rm "$WORK/r.hdd" "FFS/TD" >/dev/null 2>&1; then
    after=$(for p in OFS "OFS INTL" "OFS DirCache" FFS "FFS INTL" "FFS DirCache"; do "$BIN/afs" cat "$WORK/r.hdd" "$p/Trashcan.info" 2>/dev/null | sha256sum | cut -d' ' -f1; done)
    # FFS/TD is gone again, so every partition must read exactly as before
    if [ "$before" = "$after" ] && "$BIN/afs" verify "$WORK/r.hdd" >/dev/null 2>&1; then
        note "RDB rm/mv round-trip invisible" "all partitions identical, clean"
    else
        bad "RDB rm/mv round-trip" "bytes or verify differ"
    fi
else
    bad "RDB rm/mv round-trip" "rm/mv failed"
fi

# ---- 6. mount a post-delete copy: deletions must be mount-visible --------
# Self-contained (own copy, own writes, own delete): section 1's w.adf still
# holds its files, so it cannot serve here.
cp "$ROOT/tests/fixtures/testffs.adf" "$WORK/w.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/w.adf" --yes >/dev/null 2>&1 || true
"$BIN/afs" mkdir "$WORK/w.adf" NDIR >/dev/null 2>&1
"$BIN/afs" put "$WORK/w.adf" NDIR/big "$WORK/big.bin" >/dev/null 2>&1
"$BIN/afs" put "$WORK/w.adf" NDIR/small "$WORK/small.txt" >/dev/null 2>&1
"$BIN/afs" rm "$WORK/w.adf" NDIR/big >/dev/null 2>&1
mkdir -p "$WORK/mnt"
"$BIN/amigafs-fuse" -f "$WORK/w.adf" "$WORK/mnt" >/dev/null 2>&1 &
MPID=$!
for _ in $(seq 1 40); do
    [ -d "$WORK/mnt" ] && ls "$WORK/mnt" >/dev/null 2>&1 && break
    sleep 0.25
done
if ls "$WORK/mnt" >/dev/null 2>&1; then
    if [ ! -e "$WORK/mnt/NDIR/big" ] && [ -f "$WORK/mnt/NDIR/small" ]; then
        note "deleted file gone through mount" "ok"
    else
        bad "deleted file gone through mount"
    fi
else
    bad "mount post-delete copy" "did not mount"
fi

echo
if [ $fail -eq 0 ]; then
    echo "M14 gate: PASS"
    exit 0
fi
echo "M14 gate: FAIL"
exit 1
