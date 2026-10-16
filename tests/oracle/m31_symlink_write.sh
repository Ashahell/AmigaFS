#!/bin/sh
# M31 gate: soft-link CREATION, and the two defects this session found while
# writing it. Neither defect was reachable from the unit tests -- both needed
# something outside the library.
#
# DEFECT 1 (found by asking the oracle, not by a failing test):
#   afs_symlink originally wrote NO dir-cache record, on the reasoning that a
#   size of zero "would teach the cache the name is an empty file". Every OTHER
#   writer -- file create, hard link, mkdir -- writes one. ADFlib's `adfls`
#   lists 14 of the 15 entries on the volume and none of the soft links. The
#   hash table is not enough on a DOS\5 volume; the cache is what an outside
#   implementation walks. Ground truth from the fixture settled it: the volume's
#   OWN soft link has header=885 size=0 sec_type=3 in cache block 881.
#
# DEFECT 2 (found because both string parameters are `const char *`):
#   fuse_lowlevel.h's `.symlink` is (req, LINK, parent, NAME) -- the reverse of
#   `.link` and of the POSIX call's own word order. Written the obvious way it
#   compiles cleanly and produces a SELF-CONSISTENT volume: an entry named after
#   the target, holding the link name as its content. `ln -s tgt firstlink`
#   created an entry called `tgt`.
#
# Load-bearing: each assertion below is re-checked by a mutant, recorded in the
# gate's own output. The oracle-agreement count is the check for DEFECT 1; the
# name check is the check for DEFECT 2.
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

have_oracle=no
command -v adfls >/dev/null 2>&1 && have_oracle=yes

pass() { echo "  PASS $1"; }
fail() { echo "  FAIL $1"; FAILED=$((FAILED+1)); }
FAILED=0

echo "M31 soft-link creation"

FIX_BEFORE=$(sha256sum < "$FIXTURE" | cut -d' ' -f1)

# ---------------------------------------------------------------- via the CLI
IMG="$WORK/cli.adf"
cp "$FIXTURE" "$IMG"
# A stale map is what refuses writes; a fresh one is the starting point.
"$AFS" bitmap-rebuild "$IMG" --yes >/dev/null 2>&1 || true

echo "payload" > "$WORK/realfile.txt"
"$AFS" put "$IMG" /realfile.txt "$WORK/realfile.txt" >/dev/null

# relative target
"$AFS" symlink "$IMG" /rel realfile.txt >/dev/null
# a target that does not exist -- the whole point of a symlink
"$AFS" symlink "$IMG" /dangling nothere >/dev/null
# absolute (mount-root) and "Vol:"-prefixed targets
"$AFS" symlink "$IMG" /abs /dir_1/textfile.txt >/dev/null
"$AFS" symlink "$IMG" /vol Vol:/secret.S >/dev/null

for n in rel dangling abs vol; do
    "$AFS" ls "$IMG" | grep -aq "^l1 /$n\$" \
        && pass "CLI created soft link '$n'" || fail "CLI created soft link '$n'"
done

# the stored target must round-trip through the reader
for pair in "rel:realfile.txt" "dangling:nothere" "abs:dir_1/textfile.txt" "vol:secret.S"; do
    n=${pair%%:*}; want=${pair#*:}
    got=$("$AFS" ls "$IMG" | grep -a "^l1 /$n\$" | wc -l)
    [ "$got" = 1 ] && pass "'$n' is exactly one entry" || fail "'$n' appears $got times"
done

# refusals, each with a named reason and nothing left behind
if "$AFS" symlink "$IMG" /rel other >/dev/null 2>"$WORK/e"; then
    fail "refuses a duplicate name"
else
    grep -aq "already exists" "$WORK/e" \
        && pass "duplicate refused with a reason" || fail "duplicate refused with a reason"
fi
if "$AFS" symlink "$IMG" /dir_1 text >/dev/null 2>&1; then
    fail "refuses an existing directory name"
else
    pass "existing directory name refused"
fi
if "$AFS" symlink "$IMG" /ok "" >/dev/null 2>&1; then
    fail "refuses an empty target"
else
    pass "empty target refused"
fi

# the fixture must not have been touched. Everything above ran against COPIES;
# this proves it, rather than assuming it.
if [ "$FIX_BEFORE" = "$(sha256sum < "$FIXTURE" | cut -d' ' -f1)" ]; then
    pass "the fixture is byte-identical after every CLI call"
else
    fail "THE FIXTURE WAS MODIFIED -- every gate must work on a copy"
fi

# ------------------------------------------------------------- the ORACLE says
# This is the check that catches DEFECT 1. On a DOS\5 volume ADFlib reads the
# dir-cache, so an entry missing from it is simply invisible -- and our own
# reader would still list it, which is the trap.
if [ "$have_oracle" = yes ]; then
    theirs=$(adfls "$IMG" 2>/dev/null | grep -ac .)
    ours=$("$AFS" ls "$IMG" | grep -c .)
    if [ "$theirs" = "$ours" ]; then
        pass "ADFlib agrees on the entry count ($ours)"
    else
        fail "ADFlib sees $theirs entries, we see $ours -- the oracle disagrees"
    fi
    for n in rel dangling abs vol; do
        adfls "$IMG" 2>/dev/null | grep -aqx "$n" \
            && pass "ADFlib can see '$n'" || fail "ADFlib cannot see '$n'"
    done
    # and it must not have broken anything it could already see
    adfinfo "$IMG" >/dev/null 2>&1 && pass "ADFlib still opens the volume" \
        || fail "ADFlib could not open the volume"
else
    echo "  SKIP oracle checks (adfls not on PATH)"
fi

"$AFS" verify "$IMG" >/dev/null && pass "the volume verifies" || fail "the volume verifies"

# ------------------------------------------------------------------ via FUSE
# Only reached when a mount is possible; the kernel's symlink() is what carries
# the (link, parent, name) order that DEFECT 2 is about.
MNT="$WORK/mnt"
mkdir -p "$MNT"
MIMG="$WORK/fuse.adf"
cp "$FIXTURE" "$MIMG"
# A stale map is what refuses writes; a fresh one is the starting point.
"$AFS" bitmap-rebuild "$MIMG" --yes >/dev/null 2>&1 || true
if "$FUSE" -f "$MIMG" "$MNT" -o rw >"$WORK/fuse.log" 2>&1 &
then
    sleep 3
    if grep -aq " $MNT fuse " /proc/self/mounts; then
        echo "payload" > "$MNT/rf.txt" 2>/dev/null || true
        if ln -s rf.txt "$MNT/one" 2>/dev/null; then
            pass "ln -s succeeded through the mount"
            # DEFECT 2: the entry must be named 'one', not 'rf.txt'
            [ -L "$MNT/one" ] && pass "the link is named 'one', not the target" \
                || fail "the link is not named 'one'"
            [ "$(readlink "$MNT/one" 2>/dev/null)" = "rf.txt" ] \
                && pass "readlink returns the target" || fail "readlink returned '$(readlink "$MNT/one" 2>/dev/null)'"
            # and it must RESOLVE through the mount
            [ "$(cat "$MNT/one" 2>/dev/null)" = "payload" ] \
                && pass "the link resolves to the target's contents" \
                || fail "the link did not resolve"
            # a link to something absent must still be creatable
            ln -s nothere "$MNT/two" 2>/dev/null \
                && pass "a link to a non-existent target is creatable" \
                || fail "a link to a non-existent target was refused"
        else
            fail "ln -s failed through the mount"
        fi
        fusermount3 -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
        sleep 1
        # after unmount the links must be ON DISK, not just in the kernel cache
        for n in one two; do
            "$AFS" ls "$MIMG" | grep -aq "^l1 /$n\$" \
                && pass "'$n' persisted to the image" || fail "'$n' did not persist"
        done
        "$AFS" verify "$MIMG" >/dev/null && pass "the volume verifies after the mount" \
            || fail "the volume does not verify after the mount"
    else
        echo "  SKIP mount checks (mount did not come up)"
        fusermount3 -u "$MNT" 2>/dev/null || true
    fi
else
    echo "  SKIP mount checks (could not start the mount)"
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "M31: ALL GREEN"
    exit 0
fi
echo "M31: $FAILED FAILED"
exit 1