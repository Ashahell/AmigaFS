#!/usr/bin/env bash
# m25_fuse_link.sh -- POSIX link() through a live FUSE mount (M24).
#
# The library and CLI paths are covered by tests/unit/test_links.c and
# tests/oracle/m25_link_write.sh. This gate covers what neither can: that the
# kernel's own link() reaches our .link op, and that the mount reports ONE inode
# with the right st_nlink.
#
# Two real defects were found by writing this and are the reason it exists:
#
#   1. The op ended in `fuse_reply_err(req, 0)`. A .link op accepts ONLY
#      fuse_reply_entry or fuse_reply_err (fuse_lowlevel.h); a bare success
#      reply made libfuse fail with "writing device: Invalid argument", so `ln`
#      returned EIO EVEN THOUGH the link had been correctly written to the
#      volume. The tell: the daemon log showed the failure while `afs ls` on the
#      same image already showed the new link. "The operation happened and the
#      caller was told it failed" is the shape of a reply bug, not a write bug.
#   2. fill_stat hardcoded st_nlink = 1. The kernel uses st_nlink to decide
#      whether an unlink may drop an inode's data, so under-reporting it risks
#      freeing a file that still has names pointing at it.
#
# Readiness is a FUSE mountpoint in /proc, NOT `ls` succeeding: `ls` on the
# empty pre-mount dir succeeds too and would count a dead daemon as mounted.

set -uo pipefail

ADFLIB_BIN="${ADFLIB_BIN:-$HOME/Work/adflib-oracle/ADFlib/build/examples}"
export PATH="$ADFLIB_BIN:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="$ROOT/build"
FIX="$ROOT/tests/fixtures/testffs.adf"
WORK=$(mktemp -d /tmp/amigafs-m25fuse.XXXXXX)

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

[ -x "$BIN/amigafs-fuse" ] || { echo "SKIP: fuse binary not built"; exit 77; }
[ -f "$FIX" ]              || { echo "FAIL: fixture missing"; exit 1; }

before_sum=$(sha256sum "$FIX" | cut -d' ' -f1)
cp "$FIX" "$WORK/t.adf"
# a stale map must refuse writes (M17), so rebuild the COPY first
"$BIN/afs" bitmap-rebuild "$WORK/t.adf" --yes >/dev/null 2>&1 \
    || { echo "FAIL: could not prepare the copy"; exit 1; }

mkdir -p "$WORK/mnt"
"$BIN/amigafs-fuse" -f "$WORK/t.adf" "$WORK/mnt" -o rw >"$WORK/log" 2>&1 &
MPID=$!
mounted=0
for _ in $(seq 1 40); do
    grep -qF " $WORK/mnt fuse " /proc/self/mounts 2>/dev/null && { mounted=1; break; }
    kill -0 "$MPID" 2>/dev/null || break
    sleep 0.25
done
[ "$mounted" = 1 ] || { echo "FAIL: no mount"; cat "$WORK/log"; exit 1; }

# --- ln: the kernel's link() must succeed -----------------------------------
if ln "$WORK/mnt/secret.S" "$WORK/mnt/secret.bak" 2>"$WORK/ln.err"; then
    note "ln via POSIX link()" "rc=0"
else
    bad "ln via POSIX link()"
    note "  stderr" "$(head -1 "$WORK/ln.err")"
    note "  daemon" "$(grep -i 'invalid\|error' "$WORK/log" | head -1)"
fi

# --- ONE inode, and the real link count -------------------------------------
ino_a=$(stat -c %i "$WORK/mnt/secret.S" 2>/dev/null)
ino_b=$(stat -c %i "$WORK/mnt/secret.bak" 2>/dev/null)
nl_a=$(stat -c %h "$WORK/mnt/secret.S" 2>/dev/null)
nl_b=$(stat -c %h "$WORK/mnt/secret.bak" 2>/dev/null)

[ -n "$ino_a" ] && [ "$ino_a" = "$ino_b" ] \
    && note "both names are ONE inode" "$ino_a" \
    || bad "names are not one inode ($ino_a vs $ino_b)"

[ "$nl_a" = 2 ] && note "st_nlink on the target" "$nl_a" \
    || bad "st_nlink on the target is '$nl_a', expected 2"
[ "$nl_b" = 2 ] && note "st_nlink on the link" "$nl_b" \
    || bad "st_nlink on the link is '$nl_b', expected 2"

a=$(sha256sum "$WORK/mnt/secret.S"   | cut -c1-16)
b=$(sha256sum "$WORK/mnt/secret.bak" | cut -c1-16)
[ -n "$a" ] && [ "$a" = "$b" ] && note "both names read the same bytes" "$a" \
    || bad "contents differ ($a vs $b)"

# --- unlink one name: the other must survive --------------------------------
if rm "$WORK/mnt/secret.bak" 2>"$WORK/rm.err"; then
    note "rm one name" "rc=0"
else
    bad "rm one name"
    note "  stderr" "$(head -1 "$WORK/rm.err")"
fi
[ -f "$WORK/mnt/secret.S" ] && note "the other name survives" "yes" \
    || bad "the surviving name was lost"
c=$(sha256sum "$WORK/mnt/secret.S" | cut -c1-16)
[ "$c" = "$a" ] && note "and its bytes are unchanged" "$c" \
    || bad "the target's bytes changed ($a -> $c)"
nl_after=$(stat -c %h "$WORK/mnt/secret.S" 2>/dev/null)
[ "$nl_after" = 1 ] && note "st_nlink drops back to 1" "$nl_after" \
    || bad "st_nlink after unlink is '$nl_after', expected 1"

# --- the daemon must not have logged a failed reply -------------------------
if grep -qi "writing device\|Invalid argument" "$WORK/log"; then
    bad "the daemon logged a failed FUSE reply"
    note "  line" "$(grep -i 'writing device\|invalid argument' "$WORK/log" | head -1)"
else
    note "no failed FUSE replies in the daemon log" "clean"
fi

fusermount3 -u "$WORK/mnt" >/dev/null 2>&1
sleep 0.5

# --- the bytes on disk are a well-formed volume ----------------------------
if "$BIN/afs" verify "$WORK/t.adf" >"$WORK/v" 2>&1; then
    note "verify clean after the mount round trip" "ok"
else
    bad "verify rejected the volume: $(head -2 "$WORK/v" | tr '\n' ' ')"
fi
if "$BIN/afs" ls "$WORK/t.adf" | grep -q '^f1 /secret.S'; then
    note "the target is back to an ordinary file entry" "ok"
else
    bad "the target did not survive as a plain file"
fi
if "$BIN/afs" ls "$WORK/t.adf" | grep -q 'secret.bak'; then
    bad "the unlinked name is still present after the mount round trip"
else
    note "the unlinked name is gone from the volume" "ok"
fi

after_sum=$(sha256sum "$FIX" | cut -d' ' -f1)
[ "$before_sum" = "$after_sum" ] && note "the fixture is untouched" "ok" \
    || bad "THE FIXTURE WAS MODIFIED"

[ "$fail" -eq 0 ] && echo "PASS m25_fuse_link" || echo "FAILED m25_fuse_link"
exit "$fail"