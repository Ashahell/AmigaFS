#!/usr/bin/env bash
# M13 gate: writes — `afs put` / `afs mkdir`, oracle-diffed in both directions.
#
# What each check proves (and what it does NOT):
#   - Our files read back by ADFlib READERS (adfls/unadf): round-trip through
#     the reference implementation's well-tested side. Catches malformed
#     headers, bad checksums, broken chains. Cannot catch us agreeing with
#     ourselves (same core both ways) -- that is what the next check is for.
#   - xdftool's files read by OUR `afs cat`/`ls`: agreement with an
#     INDEPENDENT writer. Catches systematic misreading that round-trip hides.
#   - `afs verify` clean + bitmap accounting (used grows by exactly the blocks
#     consumed): structural honesty of what we wrote.
#   - RDB: write into ONE partition of a copied hdd; every other partition
#     must read back byte-identical (window-correctness for writes -- a write
#     past the window corrupts the next partition silently).
#   - Mount the written copy read-only and read the files back: our on-disk
#     format must be mount-readable (the mount never writes, so this is a pure
#     read check of written bytes).
#
# NEVER the fixture itself: all writes go to a tmp copy (ADFlib's own README
# warns writes are experimental; fixtures are sha256-pinned besides).
# Empty files are covered explicitly: ADFlib issue #30 records that a 0-length
# file must be header-only (highSeq=0, firstData=0, all slots zero) and that a
# header pointing at block 0 corrupts the volume on next write.
set -uo pipefail

ADFLIB_BIN="${ADFLIB_BIN:-$HOME/Work/adflib-oracle/ADFlib/build/examples}"
export PATH="$ADFLIB_BIN:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="$ROOT/build"
WORK=$(mktemp -d /tmp/amigafs-m13.XXXXXX)

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

echo "M13 gate: writes (put/mkdir), oracle-diffed both directions"

for t in "$BIN/afs" "$BIN/amigafs-fuse"; do
    [ -x "$t" ] || { echo "M13 gate: SKIP (build first)"; exit 0; }
done
for f in blank.adf testffs.adf testofs.adf a590_WD93028XA_6parts.hdd; do
    [ -f "$ROOT/tests/fixtures/$f" ] || { echo "M13 gate: SKIP (fixtures absent)"; exit 0; }
done

# deterministic payloads (no randomness: byte-exact diffs must reproduce)
python3 -c "open('$WORK/big.bin','wb').write(bytes((i*7+1)&0xFF for i in range(100*1024)))"
printf 'hello amiga\n' > "$WORK/small.txt"
: > "$WORK/empty.txt"

# ---- 1. put + mkdir on OFS and FFS, read back via ADFlib -------------------
for img in blank.adf testffs.adf testofs.adf; do
    cp "$ROOT/tests/fixtures/$img" "$WORK/w.adf"
    # A stale map is what refuses writes, so every case starts from a
    # fresh one. Same remedy in every writing gate.
    "$BIN/afs" bitmap-rebuild "$WORK/w.adf" --yes >/dev/null 2>&1 || true
    if ! "$BIN/afs" mkdir "$WORK/w.adf" NDIR >/dev/null 2>&1; then
        bad "mkdir $img"; continue
    fi
    if ! "$BIN/afs" put "$WORK/w.adf" NDIR/big "$WORK/big.bin" >/dev/null 2>&1; then
        bad "put $img NDIR/big"; continue
    fi
    if ! "$BIN/afs" put "$WORK/w.adf" NDIR/small "$WORK/small.txt" >/dev/null 2>&1; then
        bad "put $img NDIR/small"; continue
    fi
    if ! "$BIN/afs" put "$WORK/w.adf" NDIR/empty "$WORK/empty.txt" >/dev/null 2>&1; then
        bad "put $img NDIR/empty"; continue
    fi
    # ADFlib readers must see exactly what we wrote
    if adfls "$WORK/w.adf" 2>/dev/null | grep -q NDIR; then
        note "adfls sees NDIR on $img" "ok"
    else
        bad "adfls sees NDIR on $img"; continue
    fi
    ud="$WORK/u-$img"
    mkdir -p "$ud"
    if ! unadf -d "$ud" "$WORK/w.adf" >/dev/null 2>&1; then
        bad "unadf extracts $img"; continue
    fi
    ok=1
    for pair in "big:$WORK/big.bin" "small:$WORK/small.txt" "empty:$WORK/empty.txt"; do
        f="${pair%%:*}"; src="${pair#*:}"
        a=$(sha256sum < "$src" | cut -d' ' -f1)
        b=$(cat "$ud/NDIR/$f" 2>/dev/null | sha256sum | cut -d' ' -f1)
        if [ "$a" != "$b" ]; then ok=0; printf '      MISMATCH %s/%s adflib=%s ours=%s\n' "$img" "$f" "${a:0:12}" "${b:0:12}"; fi
    done
    # empty file must be header-only: non-empty byte_size with no data blocks
    # is ADFlib issue #30's corruption shape (header pointing at block 0)
    if [ "$(stat -c%s "$ud/NDIR/empty" 2>/dev/null)" = "0" ]; then
        note "round-trip $img" "3/3 identical, empty is empty"
    else
        ok=0; printf '      EMPTY-FILE wrong size on %s\n' "$img"
    fi
    [ $ok -eq 1 ] || bad "round-trip $img" "see mismatches above"
    # structure still clean, bitmap still consistent
    if "$BIN/afs" verify "$WORK/w.adf" >/dev/null 2>&1; then
        note "verify after writes $img" "clean"
    else
        bad "verify after writes $img"
        "$BIN/afs" verify "$WORK/w.adf" 2>&1 | head -5 | sed 's/^/      /'
    fi
done

# ---- 2. reverse: xdftool writes, we read -----------------------------------
# blank.adf: empty, so nothing pre-exists to disturb, and xdftool tolerates it
# (it refuses testffs.adf/links.adf outright -- amitools cannot parse hardlink
# blocks -- and g1a30c.adf for its bitmap mismatch; probed, not assumed).
cp "$ROOT/tests/fixtures/blank.adf" "$WORK/x.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/x.adf" --yes >/dev/null 2>&1 || true
if xdftool "$WORK/x.adf" makedir XDIR >/dev/null 2>&1 \
   && xdftool "$WORK/x.adf" write "$WORK/small.txt" XDIR/hello >/dev/null 2>&1; then
    a=$(sha256sum < "$WORK/small.txt" | cut -d' ' -f1)
    b=$("$BIN/afs" cat "$WORK/x.adf" XDIR/hello 2>/dev/null | sha256sum | cut -d' ' -f1)
    if [ "$a" = "$b" ]; then
        note "read xdftool-written file" "${a:0:12}..."
    else
        bad "read xdftool-written file" "xdftool=${a:0:12} afs=${b:0:12}"
    fi
    if "$BIN/afs" ls "$WORK/x.adf" XDIR 2>/dev/null | grep -q hello; then
        note "list xdftool-written dir" "ok"
    else
        bad "list xdftool-written dir"
    fi
else
    bad "xdftool write fixture" "xdftool makedir/write failed"
fi

# ---- 3b. INTL folding applies to creates AND truncates ------------------
# ASCII case folds on EVERY variant (real AmigaDOS behavior, not just INTL),
# so FOO-vs-foo proves nothing. The INTL-vs-plain discriminator is the
# accented range: caf\xe9 vs CAF\xc9 fold equal if and only if INTL is on
# (capitalch folds 224..254 except 247 only for flags >= 2).
# On INTL, putting the folded spelling truncates the SAME file (fopen "w"
# through a case-insensitive lookup): one entry keeps the original spelling,
# content is replaced, verify stays clean. A truncate path that duplicated
# the entry (or refused) would show here.
N1=$(printf 'caf\xe9'); N2=$(printf 'CAF\xc9')
printf 'first\n' > "$WORK/first.txt"; printf 'second\n' > "$WORK/second.txt"
cp "$ROOT/tests/fixtures/testofs.adf" "$WORK/case.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/case.adf" --yes >/dev/null 2>&1 || true
if "$BIN/afs" put "$WORK/case.adf" "$N1" "$WORK/first.txt" >/dev/null 2>&1 \
   && "$BIN/afs" put "$WORK/case.adf" "$N2" "$WORK/second.txt" >/dev/null 2>&1; then
    n=$("$BIN/afs" ls "$WORK/case.adf" 2>/dev/null | grep -c "caf")
    c=$("$BIN/afs" cat "$WORK/case.adf" "$N1" 2>/dev/null)
    if [ "$n" = "1" ] && [ "$c" = "second" ] \
       && "$BIN/afs" verify "$WORK/case.adf" >/dev/null 2>&1; then
        note "INTL truncate resolves folding" "one entry, new content, clean"
    else
        bad "INTL truncate resolves folding" "entries=$n content='$c'"
    fi
else
    bad "INTL truncate resolves folding" "put failed"
fi
cp "$ROOT/tests/fixtures/blank.adf" "$WORK/case2.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/case2.adf" --yes >/dev/null 2>&1 || true
if "$BIN/afs" put "$WORK/case2.adf" "$N1" "$WORK/first.txt" >/dev/null 2>&1 \
   && "$BIN/afs" put "$WORK/case2.adf" "$N2" "$WORK/second.txt" >/dev/null 2>&1; then
    n=$("$BIN/afs" ls "$WORK/case2.adf" 2>/dev/null | grep -c "caf\|CAF")
    if [ "$n" = "2" ]; then
        note "plain volume keeps accents apart" "ok"
    else
        bad "plain volume keeps accents apart" "entries=$n, want 2"
    fi
else
    bad "plain volume keeps accents apart" "put failed"
fi

# ---- 3. put truncates (fopen "w"); mkdir duplicates refuse --------------
# `afs put` over a live file truncates and rewrites (grow AND shrink AND to
# empty); only genuinely unwritable targets refuse. create-alone still refuses
# duplicates at the library level, but the CLI's job is cp-like, not
# adfFileOpen-"w"-like -- assert the truncation, not the refusal.
cp "$ROOT/tests/fixtures/testffs.adf" "$WORK/n.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/n.adf" --yes >/dev/null 2>&1 || true
"$BIN/afs" mkdir "$WORK/n.adf" NDIR >/dev/null 2>&1
"$BIN/afs" put "$WORK/n.adf" NDIR/f "$WORK/small.txt" >/dev/null 2>&1
printf '0123456789abcdef0123456789abcdef' > "$WORK/mid32.txt"
if "$BIN/afs" put "$WORK/n.adf" NDIR/f "$WORK/mid32.txt" >/dev/null 2>&1; then
    mkdir -p "$WORK/nu"
    a=$(sha256sum < "$WORK/mid32.txt" | cut -d' ' -f1)
    b=$("$BIN/afs" cat "$WORK/n.adf" NDIR/f 2>/dev/null | sha256sum | cut -d' ' -f1)
    s=$(unadf -d "$WORK/nu" "$WORK/n.adf" >/dev/null 2>&1; stat -c%s "$WORK/nu/NDIR/f" 2>/dev/null)
    if [ "$a" = "$b" ] && [ "$s" = "32" ]; then
        note "put grows 13 -> 32 bytes" "${a:0:12}..."
    else
        bad "put grows" "want=${a:0:12} got=${b:0:12} adfls-size=$s"
    fi
else
    bad "put grows" "second put failed"
fi
if "$BIN/afs" put "$WORK/n.adf" NDIR/f "$WORK/small.txt" >/dev/null 2>&1; then
    a=$(sha256sum < "$WORK/small.txt" | cut -d' ' -f1)
    b=$("$BIN/afs" cat "$WORK/n.adf" NDIR/f 2>/dev/null | sha256sum | cut -d' ' -f1)
    if [ "$a" = "$b" ] && "$BIN/afs" verify "$WORK/n.adf" >/dev/null 2>&1; then
        note "put shrinks 32 -> 13, still clean" "${a:0:12}..."
    else
        bad "put shrinks" "want=${a:0:12} got=${b:0:12}"
    fi
else
    bad "put shrinks" "third put failed"
fi
if "$BIN/afs" put "$WORK/n.adf" NDIR/f "$WORK/empty.txt" >/dev/null 2>&1; then
    mkdir -p "$WORK/ne"
    s=$(unadf -d "$WORK/ne" "$WORK/n.adf" >/dev/null 2>&1; stat -c%s "$WORK/ne/NDIR/f" 2>/dev/null)
    if [ "$s" = "0" ] && "$BIN/afs" verify "$WORK/n.adf" >/dev/null 2>&1; then
        note "put truncates to empty" "size 0, verify clean"
    else
        bad "put truncates to empty" "adfls-size=$s"
    fi
else
    bad "put truncates to empty" "put failed"
fi
out=$("$BIN/afs" put "$WORK/n.adf" NDIR "$WORK/small.txt" 2>&1)
if [ $? -ne 0 ] && printf '%s' "$out" | grep -qi "exist"; then
    note "put over a directory refused" "ok"
else
    bad "put over a directory" "rc=$? out=$(printf '%s' "$out" | head -1)"
fi
out=$("$BIN/afs" put "$WORK/n.adf" NODIR/f "$WORK/small.txt" 2>&1)
if [ $? -ne 0 ] && printf '%s' "$out" | grep -qi "not found\|no such"; then
    note "put to missing dir refused" "ok"
else
    bad "put to missing dir" "rc=$? out=$(printf '%s' "$out" | head -1)"
fi
out=$("$BIN/afs" mkdir "$WORK/n.adf" NDIR 2>&1)
if [ $? -ne 0 ] && printf '%s' "$out" | grep -qi "exist"; then
    note "duplicate mkdir refused" "ok"
else
    bad "duplicate mkdir" "rc=$? out=$(printf '%s' "$out" | head -1)"
fi

# ---- 4. RDB: write one partition, others byte-identical ---------------------
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
if "$BIN/afs" mkdir "$WORK/r.hdd" "FFS/NEWDIR" >/dev/null 2>&1 \
   && "$BIN/afs" put "$WORK/r.hdd" "FFS/NEWDIR/payload" "$WORK/small.txt" >/dev/null 2>&1; then
    after=$(for p in OFS "OFS INTL" "OFS DirCache" FFS "FFS INTL" "FFS DirCache"; do "$BIN/afs" cat "$WORK/r.hdd" "$p/Trashcan.info" 2>/dev/null | sha256sum | cut -d' ' -f1; done)
    if [ "$before" = "$after" ]; then
        note "other 5 partitions untouched" "ok"
    else
        bad "other 5 partitions untouched" "a write escaped its window"
    fi
    a=$(sha256sum < "$WORK/small.txt" | cut -d' ' -f1)
    b=$("$BIN/afs" cat "$WORK/r.hdd" "FFS/NEWDIR/payload" 2>/dev/null | sha256sum | cut -d' ' -f1)
    if [ "$a" = "$b" ]; then
        note "RDB partition write reads back" "${a:0:12}..."
    else
        bad "RDB partition write reads back" "want=${a:0:12} got=${b:0:12}"
    fi
    # DOS\4 (OFS data blocks + dir cache) is the one variant combination the
    # floppy loop never writes: OFS data layout on blank/testofs, DIRC cache
    # updates on testffs, but never together. A combination-specific defect
    # (OFS chain fields vs cache record content) would hide otherwise.
    if "$BIN/afs" mkdir "$WORK/r.hdd" "OFS DirCache/OD" >/dev/null 2>&1 \
       && "$BIN/afs" put "$WORK/r.hdd" "OFS DirCache/OD/f" "$WORK/small.txt" >/dev/null 2>&1; then
        b=$("$BIN/afs" cat "$WORK/r.hdd" "OFS DirCache/OD/f" 2>/dev/null | sha256sum | cut -d' ' -f1)
        if [ "$a" = "$b" ]; then
            note "OFS+DIRC partition write reads back" "${a:0:12}..."
        else
            bad "OFS+DIRC partition write reads back" "want=${a:0:12} got=${b:0:12}"
        fi
    else
        bad "OFS+DIRC partition write" "mkdir/put failed"
    fi
    if "$BIN/afs" verify "$WORK/r.hdd" >/dev/null 2>&1; then
        note "verify RDB after writes" "clean"
    else
        bad "verify RDB after writes"
    fi
else
    bad "RDB partition write" "mkdir/put failed"
fi

# ---- 5. mount the written copy: written bytes must be mount-readable --------
# Self-contained: fresh copy + own writes, no reliance on section 1 leftovers.
cp "$ROOT/tests/fixtures/testofs.adf" "$WORK/w.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/w.adf" --yes >/dev/null 2>&1 || true
"$BIN/afs" mkdir "$WORK/w.adf" NDIR >/dev/null 2>&1
"$BIN/afs" put "$WORK/w.adf" NDIR/big "$WORK/big.bin" >/dev/null 2>&1
mkdir -p "$WORK/mnt"
"$BIN/amigafs-fuse" -f "$WORK/w.adf" "$WORK/mnt" >/dev/null 2>&1 &
MPID=$!
for _ in $(seq 1 40); do
    [ -d "$WORK/mnt" ] && ls "$WORK/mnt" >/dev/null 2>&1 && break
    sleep 0.25
done
if ls "$WORK/mnt" >/dev/null 2>&1; then
    # The mount never writes, so this is a pure read check of written bytes.
    a=$(sha256sum < "$WORK/big.bin" | cut -d' ' -f1)
    b=$(cat "$WORK/mnt/NDIR/big" 2>/dev/null | sha256sum | cut -d' ' -f1)
    if [ -n "$b" ] && [ "$a" = "$b" ]; then
        note "written file mount-readable" "${a:0:12}..."
    else
        bad "written file mount-readable" "want=${a:0:12} got=${b:0:12}"
    fi
else
    bad "mount written copy" "did not mount"
fi

echo
if [ $fail -eq 0 ]; then
    echo "M13 gate: PASS"
    exit 0
fi
echo "M13 gate: FAIL"
exit 1
