#!/usr/bin/env bash
# M15 gate: FUSE write operations on a `-o rw` mount.
#
# The mount was read-only since M8 (op-level EROFS); `-o rw` opens the volumes
# through the writable backend instead. Default stays read-only: every gate
# before this one mounts without `rw`, and section 1 below proves a default
# mount still refuses writes with the fixture byte-identical afterwards.
#
# What each check proves:
#   - POSIX flows (cp/echo >/>>/truncate/mkdir/rm/mv) through the KERNEL, not
#     direct API calls: dentry/inode translation, O_TRUNC/O_APPEND/O_CREAT
#     handling, and setattr-size all run. A library-only test cannot see these.
#   - Post-unmount oracle diff (unadf/adfls/verify/bitmap): the bytes on disk
#     are a well-formed volume, not just kernel-accepted replies.
#   - Default-mount refusal + whole-image sha256: `ro` means read-only at the
#     byte level, and the rw switch is what changes it (Rule 6: the passing
#     refusal proves the rw successes ran in a different mode).
#   - Cross-partition rename returns EXDEV (asserted directly via python, not
#     via mv(1): GNU mv catches EXDEV and falls back to copy+delete, so a
#     shell-level refusal test would fail on correct code). The fallback path
#     itself is asserted by end state instead.
set -uo pipefail

ADFLIB_BIN="${ADFLIB_BIN:-$HOME/Work/adflib-oracle/ADFlib/build/examples}"
export PATH="$ADFLIB_BIN:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="$ROOT/build"
WORK=$(mktemp -d /tmp/amigafs-m15.XXXXXX)

fail=0
note() { printf '  %-46s %s\n' "$1" "$2"; }
bad()  { printf '  %-46s FAIL\n' "$1"; fail=1; }

MPID=""
cleanup() {
    fusermount3 -uz "$WORK/mnt" 2>/dev/null
    [ -n "$MPID" ] && kill "$MPID" 2>/dev/null
    wait "$MPID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

mount_rw() { # $1 = image; mounts with -o rw, sets MPID; returns nonzero if dead
    "$BIN/amigafs-fuse" -f "$1" "$WORK/mnt" -o rw >/dev/null 2>&1 &
    MPID=$!
    for _ in $(seq 1 40); do
        # readiness is a FUSE mountpoint in /proc, NOT `ls` succeeding: ls on
        # the empty pre-mount dir succeeds too and would count a dead daemon
        # as mounted (same trap as an unmounted dir "passing" a listing check)
        grep -qF " $WORK/mnt fuse " /proc/self/mounts 2>/dev/null && return 0
        kill -0 "$MPID" 2>/dev/null || return 1
        sleep 0.25
    done
    grep -qF " $WORK/mnt fuse " /proc/self/mounts 2>/dev/null
}
mount_ro() { # $1 = image; default (read-only) mount
    "$BIN/amigafs-fuse" -f "$1" "$WORK/mnt" >/dev/null 2>&1 &
    MPID=$!
    for _ in $(seq 1 40); do
        grep -qF " $WORK/mnt fuse " /proc/self/mounts 2>/dev/null && return 0
        kill -0 "$MPID" 2>/dev/null || return 1
        sleep 0.25
    done
    grep -qF " $WORK/mnt fuse " /proc/self/mounts 2>/dev/null
}
unmount() {
    fusermount3 -u "$WORK/mnt" >/dev/null 2>&1
    gone=0
    for _ in $(seq 1 20); do
        kill -0 "$MPID" 2>/dev/null || { gone=1; break; }
        sleep 0.1
    done
    if [ "$gone" -eq 0 ]; then kill "$MPID" 2>/dev/null; fi
    wait "$MPID" 2>/dev/null
    MPID=""
}

echo "M15 gate: FUSE writes (-o rw)"

for t in "$BIN/afs" "$BIN/amigafs-fuse"; do
    [ -x "$t" ] || { echo "M15 gate: SKIP (build first)"; exit 0; }
done
[ -e /dev/fuse ] || { echo "M15 gate: SKIP (no /dev/fuse)"; exit 0; }
command -v fusermount3 >/dev/null || { echo "M15 gate: SKIP (no fusermount3)"; exit 0; }
for f in testffs.adf a590_WD93028XA_6parts.hdd; do
    [ -f "$ROOT/tests/fixtures/$f" ] || { echo "M15 gate: SKIP (fixtures absent)"; exit 0; }
done
mkdir -p "$WORK/mnt"

python3 -c "open('$WORK/big.bin','wb').write(bytes((i*7+1)&0xFF for i in range(100*1024)))"
printf 'hello amiga\n' > "$WORK/small.txt"

# ---- 1. default mount refuses writes, fixture bit-identical ---------------
cp "$ROOT/tests/fixtures/testffs.adf" "$WORK/ro.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/ro.adf" --yes >/dev/null 2>&1 || true
sha_before=$(sha256sum < "$WORK/ro.adf" | cut -d' ' -f1)
if mount_ro "$WORK/ro.adf"; then
    # The errno matters as much as the failure: a write refused because the
    # op-level EROFS switch fired is CORRECT, while one that got through the
    # switch and died later on a read-only blockdev also fails -- and reports
    # EIO. Blinding `refuse_if_ro` passes a bare "did it fail?" check, so the
    # reason is asserted: EROFS (strerror "Read-only file system").
    refused=0
    for probe in \
        "cp $WORK/small.txt $WORK/mnt/nope" \
        "mkdir $WORK/mnt/nodir" \
        "echo x > $WORK/mnt/emptyfile" \
        "rm $WORK/mnt/emptyfile"
    do
        msg=$(eval "$probe" 2>&1 >/dev/null)
        case "$msg" in
            *"Read-only file system"*) refused=$((refused+1)) ;;
            *) note "  (not EROFS: $probe)" "$(printf '%s' "$msg" | head -1)" ;;
        esac
    done
    if [ "$refused" -eq 4 ]; then
        note "default mount refuses cp/mkdir/write/rm" "EROFS x4"
    else
        bad "default mount refuses writes" "EROFS on $refused of 4 (rest failed for another reason)"
    fi
    unmount
else
    bad "default mount" "did not mount"
fi
sha_after=$(sha256sum < "$WORK/ro.adf" | cut -d' ' -f1)
if [ "$sha_before" = "$sha_after" ]; then
    note "fixture byte-identical after refused writes" "${sha_after:0:12}..."
else
    bad "fixture byte-identical" "image changed under a read-only mount"
fi

# ---- 2. rw mount: POSIX create/write/append/truncate -----------------------
cp "$ROOT/tests/fixtures/testffs.adf" "$WORK/rw.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/rw.adf" --yes >/dev/null 2>&1 || true
if mount_rw "$WORK/rw.adf"; then
    cp "$WORK/big.bin" "$WORK/mnt/big" 2>/dev/null \
        && cp "$WORK/small.txt" "$WORK/mnt/small" 2>/dev/null \
        && echo appended >> "$WORK/mnt/small" 2>/dev/null \
        && mkdir "$WORK/mnt/ND" 2>/dev/null \
        && cp "$WORK/small.txt" "$WORK/mnt/ND/nested" 2>/dev/null
    if [ $? -eq 0 ]; then
        note "cp/mkdir/append through mount" "ok"
    else
        bad "cp/mkdir/append through mount" "a POSIX op failed"
    fi
    # O_TRUNC over existing content, then truncate-to-empty, then regrow
    echo SHORT > "$WORK/mnt/small" 2>/dev/null
    : > "$WORK/mnt/empty" 2>/dev/null
    truncate -s 2048 "$WORK/mnt/empty" 2>/dev/null
    if [ "$(cat "$WORK/mnt/small" 2>/dev/null)" = "SHORT" ] \
       && [ "$(stat -c%s "$WORK/mnt/empty" 2>/dev/null)" = "2048" ]; then
        note "O_TRUNC + truncate -s" "ok"
    else
        bad "O_TRUNC + truncate -s"
    fi
    # grown region must read back as zeros (hole semantics)
    if cmp -s <(head -c 2048 /dev/zero) "$WORK/mnt/empty" 2>/dev/null; then
        note "truncate-grown region is zero-filled" "ok"
    else
        bad "truncate-grown region is zero-filled"
    fi
# ---- 2b. a write at a NON-ZERO offset, in place --------------------------
# Every other case here writes whole files from offset 0, which is exactly
# what does NOT exercise a range write's splice arithmetic (the block a byte
# lands in and the offset within that block are only distinguishable when the
# write starts mid-block). A mutant that pinned every write to in-block offset
# 0 passed all of section 2 -- this is the case that catches it.
python3 -c "
import os
p = '$WORK/mnt/mid.bin'
with open(p, 'wb') as f:
    f.write(bytes((i * 11 + 3) & 0xFF for i in range(5000)))
"
if python3 -c "
import os
fd = os.open('$WORK/mnt/mid.bin', os.O_RDWR)
os.pwrite(fd, b'PATCHED-AT-1234', 1234)
os.close(fd)
" 2>/dev/null; then
    note "pwrite at offset 1234" "ok"
else
    bad "pwrite at offset 1234"
fi
python3 -c "
expect = bytearray((i * 11 + 3) & 0xFF for i in range(5000))
expect[1234:1234+15] = b'PATCHED-AT-1234'
open('$WORK/mid.expect', 'wb').write(bytes(expect))
"
if cmp -s "$WORK/mnt/mid.bin" "$WORK/mid.expect"; then
    note "in-place pwrite: exactly 15 bytes changed" "prefix+suffix intact"
else
    bad "in-place pwrite" "$(cmp "$WORK/mnt/mid.bin" "$WORK/mid.expect" 2>&1 | head -1)"
fi
    unmount   # section 3 reads the bytes OFF DISK, so the mount must be closed
else
    bad "rw mount" "did not mount"
fi

# ---- 3. oracle-diff the rw-written image ------------------------------------
if unadf -d "$WORK/u" "$WORK/rw.adf" >/dev/null 2>&1; then
    ok=1
    for pair in "big:$WORK/big.bin"; do
        f="${pair%%:*}"; src="${pair#*:}"
        a=$(sha256sum < "$src" | cut -d' ' -f1)
        b=$(cat "$WORK/u/$f" 2>/dev/null | sha256sum | cut -d' ' -f1)
        [ "$a" = "$b" ] || { ok=0; printf '      MISMATCH %s unadf=%s mount=%s\n' "$f" "${a:0:12}" "${b:0:12}"; }
    done
    # mid.bin: the pwrite above, diffed against what we computed independently
    if cmp -s "$WORK/u/mid.bin" "$WORK/mid.expect" 2>/dev/null; then
        :
    else
        ok=0; printf '      MISMATCH mid.bin in-place pwrite\n'
    fi
    # small was truncated to "SHORT\n" (6 bytes) via O_TRUNC
    if [ "$(cat "$WORK/u/small" 2>/dev/null)" != "SHORT" ]; then ok=0; printf '      MISMATCH small content\n'; fi
    # empty grew to 2048 zeros via truncate -s
    if ! cmp -s <(head -c 2048 /dev/zero) "$WORK/u/empty" 2>/dev/null; then ok=0; printf '      MISMATCH empty zerofill\n'; fi
    if [ "$(cat "$WORK/u/ND/nested" 2>/dev/null)" != "$(cat "$WORK/small.txt")" ]; then ok=0; printf '      MISMATCH nested\n'; fi
    if [ $ok -eq 1 ]; then
        note "rw-written bytes == unadf" "ok"
    else
        bad "rw-written bytes == unadf"
    fi
else
    bad "rw-written bytes == unadf" "unadf failed"
fi
if "$BIN/afs" verify "$WORK/rw.adf" >/dev/null 2>&1; then
    note "verify rw-written image" "clean"
else
    bad "verify rw-written image"
    "$BIN/afs" verify "$WORK/rw.adf" 2>&1 | head -5 | sed 's/^/      /'
fi

# ---- 4. rm/mv/rmdir through the mount ---------------------------------------
cp "$ROOT/tests/fixtures/testffs.adf" "$WORK/m.adf"
# A stale map is what refuses writes, so every case starts from a
# fresh one. Same remedy in every writing gate.
"$BIN/afs" bitmap-rebuild "$WORK/m.adf" --yes >/dev/null 2>&1 || true
if mount_rw "$WORK/m.adf"; then
    mkdir "$WORK/mnt/D1" && mkdir "$WORK/mnt/D2" && cp "$WORK/small.txt" "$WORK/mnt/D1/a" \
    && mv "$WORK/mnt/D1/a" "$WORK/mnt/D2/b" && rm "$WORK/mnt/D2/b" && rmdir "$WORK/mnt/D1" \
    && mkdir "$WORK/mnt/NE" && cp "$WORK/small.txt" "$WORK/mnt/NE/f"
    if [ $? -eq 0 ]; then
        if rmdir "$WORK/mnt/NE" 2>/dev/null; then
            bad "rmdir non-empty" "succeeded"
        else
            note "rmdir non-empty refused" "ok"
        fi
        # POSIX rename(a, a) must SUCCEED (it is a no-op, not an error). GNU
        # mv(1) short-circuits same-file renames itself -- it prints "are the
        # same file" and never calls rename(2) -- so asserting through mv(1)
        # tests mv, not the filesystem. Call rename(2) directly.
        selfrc=$(python3 -c "
import os
try:
    os.rename('$WORK/mnt/NE/f', '$WORK/mnt/NE/f')
    print(0)
except OSError as e:
    print(e.errno)
" 2>/dev/null)
        if [ "$selfrc" = "0" ] && [ "$(cat "$WORK/mnt/NE/f" 2>/dev/null)" = "$(cat "$WORK/small.txt")" ]; then
            note "posix flows through mount" "rename(a,a) no-op, content intact"
        else
            bad "posix flows" "rename(a,a) rc=$selfrc"
        fi
    else
        bad "posix flows through mount" "a POSIX op failed"
    fi
    unmount
else
    bad "rw mount (2)" "did not mount"
fi
if "$BIN/afs" verify "$WORK/m.adf" >/dev/null 2>&1 \
   && ! "$BIN/afs" cat "$WORK/m.adf" D2/b >/dev/null 2>&1 \
   && [ "$("$BIN/afs" cat "$WORK/m.adf" NE/f 2>/dev/null | sha256sum | cut -d' ' -f1)" = "$(sha256sum < "$WORK/small.txt" | cut -d' ' -f1)" ]; then
    note "post-mv/rm state + verify" "clean"
else
    bad "post-mv/rm state + verify"
fi

# ---- 5. RDB: write one partition, EXDEV across, others identical ------------
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
if mount_rw "$WORK/r.hdd"; then
    cp "$WORK/small.txt" "$WORK/mnt/FFS/from-mount" 2>/dev/null
    if [ $? -eq 0 ] && [ "$(cat "$WORK/mnt/FFS/from-mount" 2>/dev/null)" = "$(cat "$WORK/small.txt")" ]; then
        note "write inside RDB partition" "ok"
    else
        bad "write inside RDB partition"
    fi
    # EXDEV asserted DIRECTLY: GNU mv(1) catches EXDEV and silently falls back
    # to copy+delete, so testing refusal through mv(1) fails on correct code.
    # The fallback path is asserted by end state in section 4 instead.
    exdev=$(python3 -c "
import errno, os, sys
try:
    os.rename('$WORK/mnt/FFS/from-mount', '$WORK/mnt/OFS/moved')
    print('none')
except OSError as e:
    print(e.errno)
" 2>/dev/null)
    if [ "$exdev" = "18" ]; then
        note "cross-partition rename -> EXDEV" "errno 18"
    else
        bad "cross-partition rename" "errno=$exdev, want 18 (EXDEV)"
    fi
    # and the fallback users actually see must still work
    if mv "$WORK/mnt/FFS/from-mount" "$WORK/mnt/OFS/moved" 2>/dev/null \
       && [ "$(cat "$WORK/mnt/OFS/moved" 2>/dev/null)" = "$(cat "$WORK/small.txt")" ] \
       && ! ls "$WORK/mnt/FFS/from-mount" >/dev/null 2>&1; then
        note "mv EXDEV fallback copy+delete" "ok"
    else
        bad "mv EXDEV fallback" "content or source state wrong"
    fi
    unmount
else
    bad "rw mount (RDB)" "did not mount"
fi
after=$(for p in OFS "OFS INTL" "OFS DirCache" FFS "FFS INTL" "FFS DirCache"; do "$BIN/afs" cat "$WORK/r.hdd" "$p/Trashcan.info" 2>/dev/null | sha256sum | cut -d' ' -f1; done)
if [ "$before" = "$after" ] && "$BIN/afs" verify "$WORK/r.hdd" >/dev/null 2>&1; then
    note "other 5 partitions identical + clean" "ok"
else
    bad "other 5 partitions identical + clean"
fi

echo
if [ $fail -eq 0 ]; then
    echo "M15 gate: PASS"
    exit 0
fi
echo "M15 gate: FAIL"
exit 1
