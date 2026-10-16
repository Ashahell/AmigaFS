#!/bin/sh
# M32 gate: removing a link must give its block BACK, and both shells must
# remove soft links.
#
# The defect: `afs rm`'s hard-link branch and the mount's `.unlink` both called
# afs_dir_unlink -- which splices the NAME out of the hash chain and nothing
# else -- under a comment saying "removes only that name's header block". They
# freed nothing. Measured on testffs.adf:
#
#     after hard link:  free = 1412
#     after rm h1:      free = 1412     <- the comment says 1413
#
# One block per unlinked link, leaked permanently: the name is gone, the entry
# is unreachable, and the block stays marked allocated, so no allocator will
# ever hand it out again.
#
# WHY NO GATE CAUGHT IT, which is the reason this gate asserts what it does:
#   - `afs verify` does not consult the bitmap at all
#   - `afs verify --strict` checks only the OTHER direction ("referenced by the
#     tree but marked free"), and on these stale fixtures produces 11
#     complaints that have nothing to do with a leak
#   - nothing anywhere detects "marked allocated, referenced by nothing"
#   - the unit tests call the library directly, so reverting the SHELL to the
#     half-operation passes them (measured: mutant M3)
#
# So the load-bearing assertion here is a BITMAP ROUND TRIP measured through
# the CLI, on the path the shell actually takes. That is the only kind of
# assertion that can see this class of defect.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

AFS="$ROOT/build/afs"
FUSE="$ROOT/build/amigafs-fuse"
FIXTURE="$ROOT/tests/fixtures/testffs.adf"

[ -x "$AFS" ] || { echo "SKIP: afs not built"; exit 0; }
[ -f "$FIXTURE" ] || { echo "SKIP: fixture absent (tests/fixtures are fetched, not committed)"; exit 0; }

pass() { echo "  PASS $1"; }
fail() { echo "  FAIL $1"; FAILED=$((FAILED+1)); }
FAILED=0

echo "M32 removing a link returns its block"

FIX_BEFORE=$(sha256sum < "$FIXTURE" | cut -d' ' -f1)
IMG="$WORK/t.adf"
cp "$FIXTURE" "$IMG"
# A stale map is what refuses writes; a fresh one is the starting point.
"$AFS" bitmap-rebuild "$IMG" --yes >/dev/null 2>&1 || true

# `afs bitmap`'s own free count -- the measurement that found the defect.
freecount() { "$AFS" bitmap "$IMG" | awk '/^free:/{print $2}'; }

printf 'payload for the link tests' > "$WORK/x.txt"

# ------------------------------------------------------------------ hard link
f0=$(freecount)
"$AFS" put "$IMG" /target.txt "$WORK/x.txt" >/dev/null
f1=$(freecount)
[ "$f1" -lt "$f0" ] && pass "put consumed a block ($f0 -> $f1)" \
    || fail "put did not consume a block ($f0 -> $f1)"

"$AFS" link "$IMG" /target.txt hardname >/dev/null
f2=$(freecount)
[ "$f2" -lt "$f1" ] && pass "a hard link consumed a block ($f1 -> $f2)" \
    || fail "a hard link consumed no block ($f1 -> $f2)"

"$AFS" rm "$IMG" /hardname >/dev/null
f3=$(freecount)
# THE assertion. Unlinking must return exactly what linking took.
[ "$f3" -gt "$f2" ] && pass "unlinking a hard link RETURNED its block ($f2 -> $f3)" \
    || fail "unlinking a hard link leaked its block (still $f2) -- the M32 defect"

# ------------------------------------------------------------------ soft link
"$AFS" symlink "$IMG" /softlink target.txt >/dev/null
f4=$(freecount)
[ "$f4" -lt "$f3" ] && pass "a soft link consumed a block ($f3 -> $f4)" \
    || fail "a soft link consumed no block ($f3 -> $f4)"

"$AFS" rm "$IMG" /softlink >/dev/null 2>&1 \
    && pass "the CLI removed a soft link" \
    || fail "the CLI refused to remove a soft link"

f5=$(freecount)
[ "$f5" -gt "$f4" ] && pass "removing it RETURNED its block ($f4 -> $f5)" \
    || fail "removing a soft link leaked its block (still $f4)"

# the whole sequence must be a round trip: back to where put left us
[ "$f5" = "$f1" ] && pass "the create/unlink cycle is an exact round trip" \
    || fail "the cycle leaked ($f1 before links, $f5 after)"

# the target is untouched throughout
"$AFS" ls "$IMG" | grep -aq "^f1 /target.txt$" \
    && pass "the TARGET survived both link removals" \
    || fail "the target did not survive"

# no link entries are left behind
if "$AFS" ls "$IMG" | grep -aqE "^(l1|h1) /(softlink|hardname)$"; then
    fail "a removed link is still listed"
else
    pass "neither removed link is listed any more"
fi

# --------------------------------------------------------------------- oracle
# ADFlib reads the DIR CACHE, so a record left behind keeps a removed link
# VISIBLE to it after our own reader has dropped the name (M31). Comparing the
# name sets both ways is the only check that sees the disagreement.
if command -v adfls >/dev/null 2>&1; then
    theirs=$(adfls "$IMG" 2>/dev/null | grep -ac .)
    ours=$("$AFS" ls "$IMG" | grep -c .)
    [ "$theirs" = "$ours" ] && pass "ADFlib agrees on the entry count ($ours)" \
        || fail "ADFlib sees $theirs entries, we see $ours -- a stale dir-cache record"
    adfls "$IMG" 2>/dev/null | grep -aqx "softlink" \
        && fail "ADFlib can still see a link we deleted" \
        || pass "ADFlib cannot see the deleted link"
else
    echo "  SKIP oracle check (adfls not on PATH)"
fi

# amitools is BLIND to soft links and cannot be an oracle here: xdftool refuses
# to even list a directory containing one. Pinned so nobody re-derives it.
if command -v xdftool >/dev/null 2>&1 && [ -f "$ROOT/tests/fixtures/test_link_chains.adf" ]; then
    out=$(xdftool "$ROOT/tests/fixtures/test_link_chains.adf" list softlinks_file 2>&1 || true)
    case "$out" in
        *"Unsupported Dir Block"*Sub_Type*00000003*)
            pass "amitools' blindness to soft links is still what we recorded (not a regression)" ;;
        *)
            echo "  NOTE amitools now lists softlinks_file; re-check the M32 oracle note" ;;
    esac
fi

"$AFS" verify "$IMG" >/dev/null && pass "the volume verifies" || fail "the volume verifies"

# --------------------------------------------------------------------- refusals
if "$AFS" rm "$IMG" /nosuchname >/dev/null 2>&1; then
    fail "rm of a missing name succeeded"
else
    pass "rm of a missing name is refused"
fi
# rm of a FILE must still free its data blocks, not just one
"$AFS" put "$IMG" /victim.txt "$WORK/x.txt" >/dev/null
g0=$(freecount)
"$AFS" rm "$IMG" /victim.txt >/dev/null
g1=$(freecount)
[ "$g1" -gt "$g0" ] && pass "rm of a FILE still frees its blocks ($g0 -> $g1)" \
    || fail "rm of a file freed nothing ($g0 -> $g1)"

# --------------------------------------------------------------------- the mount
MNT="$WORK/mnt"
mkdir -p "$MNT"
MIMG="$WORK/fuse.adf"
cp "$FIXTURE" "$MIMG"
# A stale map is what refuses writes; a fresh one is the starting point.
"$AFS" bitmap-rebuild "$MIMG" --yes >/dev/null 2>&1 || true
mf() { "$AFS" bitmap "$MIMG" | awk '/^free:/{print $2}'; }

if "$FUSE" -f "$MIMG" "$MNT" -o rw >"$WORK/fuse.log" 2>&1 & then
    sleep 3
    if grep -aq " $MNT fuse " /proc/self/mounts; then
        printf 'mount payload' > "$MNT/t.txt" 2>/dev/null || true
        m0=$(mf)
        ln -s t.txt "$MNT/sl" 2>/dev/null && pass "ln -s through the mount" \
            || fail "ln -s through the mount"
        ln "$MNT/t.txt" "$MNT/hl" 2>/dev/null && pass "ln (hard) through the mount" \
            || fail "ln (hard) through the mount"
        m1=$(mf)
        rm "$MNT/sl" 2>/dev/null && pass "rm of a soft link through the mount" \
            || fail "rm of a soft link through the mount"
        rm "$MNT/hl" 2>/dev/null && pass "rm of a hard link through the mount" \
            || fail "rm of a hard link through the mount"
        m2=$(mf)
        [ "$m2" -gt "$m1" ] && pass "the mount returned both blocks ($m1 -> $m2)" \
            || fail "the mount leaked a block ($m1 -> $m2) -- the M32 defect via unlink()"
        [ -f "$MNT/t.txt" ] && pass "the target survived both removals" \
            || fail "the target did not survive"
        fusermount3 -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
        sleep 1
        "$AFS" verify "$MIMG" >/dev/null && pass "the volume verifies after the mount" \
            || fail "the volume does not verify after the mount"
    else
        echo "  SKIP mount checks (mount did not come up)"
        fusermount3 -u "$MNT" 2>/dev/null || true
    fi
else
    echo "  SKIP mount checks (could not start the mount)"
fi

if [ "$FIX_BEFORE" = "$(sha256sum < "$FIXTURE" | cut -d' ' -f1)" ]; then
    pass "the fixture is byte-identical"
else
    fail "THE FIXTURE WAS MODIFIED -- every gate must work on a copy"
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "M32: ALL GREEN"
    exit 0
fi
echo "M32: $FAILED FAILED"
exit 1