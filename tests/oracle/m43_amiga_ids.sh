#!/bin/bash
# M43 -- `-o amiga_ids`, end to end through a real mount.
#
# WHY THIS GATE EXISTS. `amiga_ids` is a SHIPPED, USER-FACING mount option --
# registered at src/fuse/main.c:1271, in the option help, plumbed through
# afs_translate_ids into fill_stat, and documented in the README as "off". Two
# records in this project claimed it was "not offered":
#
#   - the plan's open risk: "until it is closed, amiga_ids is not offered"
#   - M38 (mine): "stays unoffered, which is where it already was"
#
# Both were wrong, inherited from a stale line rather than read off the code. That
# matters beyond bookkeeping: because the option IS offered, the GID-width
# question from M38 is not academic -- a user who enables it is served whatever
# be16() returns, from src/lib/dir.c:1009.
#
# test_translate.c already covers afs_translate_ids at the library level. Nothing
# covered the wiring: no gate ever passed -o amiga_ids to a mount. This one does.
#
# THE FIXTURE PROBLEM, and why the test patches headers. Every fixture in
# tests/fixtures stores (uid, gid) = (0, 0) in every file header -- measured, all
# ten. So an end-to-end test on an unmodified fixture can only ever prove
# "amiga_ids reports 0 where the default reports the mounting user", which is
# necessary and far from sufficient. Patching headers to a distinctive pair and
# asserting the mount reports THAT is what exercises the read, and it pins be16()
# specifically: widening the field to be32 would break this gate, which is the
# point.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FUSE=$ROOT/build/amigafs-fuse
AFS=$ROOT/build/afs
FIXTURE=${1:-$ROOT/tests/fixtures/ffdisk0049.adf}
WANT_UID=4660          # 0x1234
WANT_GID=22136         # 0x5678

fails=0
note() { printf '  %-48s %s\n' "$1" "$2"; }
bad()  { printf '  %-48s FAIL (%s)\n' "$1" "$2"; fails=$((fails + 1)); }

[ -x "$FUSE" ] && [ -x "$AFS" ] || { echo "M43 SKIP: build missing"; exit 77; }
[ -f "$FIXTURE" ] || { echo "M43 SKIP: fixture not present"; exit 77; }
command -v stat >/dev/null 2>&1 || { echo "M43 SKIP: stat(1) not available"; exit 77; }

W=$(mktemp -d /tmp/amigafs-m43.XXXXXX)
MPID=""
cleanup() {
    [ -n "$MPID" ] && { fusermount3 -uz "$W/mnt" 2>/dev/null; kill "$MPID" 2>/dev/null; }
    rm -rf "$W"
}
trap cleanup EXIT

# Stamp the distinctive ids into EVERY plain-file header, restamping each block's
# checksum. Patching all of them rather than one keeps the test independent of
# block->path mapping, and it cannot mask a per-file difference because the
# assertion is that they are all the SAME distinctive pair.
n=$(python3 - "$FIXTURE" "$W/v.adf" <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1],'rb').read())
n = 0
for blk in range(2, len(d)//512):
    o = blk*512
    if struct.unpack_from('>I', d, o)[0] != 2: continue
    if struct.unpack_from('>I', d, o+508)[0] != 0xFFFFFFFD: continue   # ST_FILE
    struct.pack_into('>H', d, o+512-196, 0x1234)
    struct.pack_into('>H', d, o+512-194, 0x5678)
    s = sum(d[o+i] for i in range(20, 508)) & 0xFFFFFFFF
    struct.pack_into('>I', d, o+20, s)
    n += 1
open(sys.argv[2],'wb').write(d)
print(n)
PY
)
if [ "${n:-0}" -lt 1 ]; then
    echo "M43 gate: FAIL -- no plain-file header found in $FIXTURE"
    exit 1
fi

REL=$("$AFS" ls "$W/v.adf" -R 2>/dev/null | awk '/^f1 /{print $2; exit}')
if [ -z "${REL:-}" ]; then
    echo "M43 gate: FAIL -- afs ls found no file to stat"
    exit 1
fi

# ---- an unknown option must be NAMED, not blamed on the image --------------
#
# fuse_opt_parse leaves UNMATCHED options at the FRONT of the array, ahead of the
# positionals: `-o ro` yields [prog] [-o] [ro] [image] [mnt]. Reading argv[1] as
# the image then reported "cannot open image '-o'" -- telling the user their disk
# image was broken when they had misspelled a flag. `ro` is the natural thing to
# type precisely because read-only IS the default and there is no `ro` option for
# it, so the most guessable flag had the most misleading message.
#
# This is a MESSAGE-QUALITY gate: it asserts on what the user is told, because the
# defect was not a crash or a wrong volume but a sentence pointing at the wrong
# thing. Nothing else in the suite would notice.
opt_out() {
    "$FUSE" -f "$W/v.adf" "$W/mnt" $1 2>&1 | head -6
}
mkdir -p "$W/mnt"
msg=$(opt_out "-o ro")
if printf '%s' "$msg" | grep -q "unknown option"; then
    note "an unknown -o option is reported as such" "'-o ro'"
else
    bad "an unknown -o option is reported as such" "got: $(printf '%s' "$msg" | head -1)"
fi
if printf '%s' "$msg" | grep -q "cannot open image"; then
    bad "an unknown option does NOT blame the image" "still says 'cannot open image'"
else
    note "an unknown option does NOT blame the image" "clean"
fi
if printf '%s' "$msg" | grep -qi "read-only is the DEFAULT"; then
    note "the 'ro' case explains that read-only is the default" "yes"
else
    bad "the 'ro' case explains that read-only is the default" "no hint given"
fi
if opt_out "-o bogus_flag" | grep -q "bogus_flag"; then
    note "an unknown option is named" "bogus_flag"
else
    bad "an unknown option is named" "not mentioned"
fi
if opt_out "-o ro" | grep -q -- "-o rw"; then
    note "the help text now lists -o rw" "yes"
else
    bad "the help text now lists -o rw" "absent from usage()"
fi

echo "=== M43 gate (-o amiga_ids end to end) ==="
echo "  patched $n file header(s) on $(basename "$FIXTURE"); stat'ing /$REL"

# $1 is the VALUE of -o, or empty for no options at all.
#
# Two things the first two versions of this gate got wrong, both worth recording:
#  - passing "-o amiga_ids" through "$1" made it ONE argv entry containing a space;
#  - passing "-o ro" assumed `ro` is an option. IT IS NOT: the table at
#    src/fuse/main.c:1270 is { amiga_ids, tz=local, rw, -f } and read-only is the
#    DEFAULT, so `-o ro` is an unknown option, fuse_opt_parse leaves the bare
#    `-o` in the array, and main() reports "cannot open image '-o'".
mount_ids() {
    mkdir -p "$W/mnt"
    if [ -n "$1" ]; then
        "$FUSE" -f "$W/v.adf" "$W/mnt" -o "$1" >"$W/fuse.log" 2>&1 &
    else
        "$FUSE" -f "$W/v.adf" "$W/mnt" >"$W/fuse.log" 2>&1 &
    fi
    MPID=$!
    for _ in $(seq 1 25); do
        grep -qF " $W/mnt fuse " /proc/self/mounts 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

if ! mount_ids ""; then
    bad "mount without the option" "did not come up"; head -3 "$W/fuse.log" | sed 's/^/      /'
    echo "M43 gate: FAIL"; exit 1
fi
off=$(stat -c '%u %g' "$W/mnt/$REL" 2>/dev/null)
fusermount3 -uz "$W/mnt" 2>/dev/null; sleep 1; MPID=""

if ! mount_ids "amiga_ids"; then
    bad "mount with -o amiga_ids" "did not come up"; head -3 "$W/fuse.log" | sed 's/^/      /'
    echo "M43 gate: FAIL"; exit 1
fi
on=$(stat -c '%u %g' "$W/mnt/$REL" 2>/dev/null)
fusermount3 -uz "$W/mnt" 2>/dev/null; sleep 1; MPID=""

echo "  without the option : uid/gid = $off"
echo "  with    the option : uid/gid = $on"

if [ "$on" = "$WANT_UID $WANT_GID" ]; then
    note "amiga_ids reports the patched stored ids" "$on"
else
    bad "amiga_ids reports the patched stored ids" "want '$WANT_UID $WANT_GID', got '$on'"
fi

if [ "${off%% *}" = "$(id -u)" ]; then
    note "without it, the mounting user's ids are used" "$off"
else
    bad "without it, the mounting user's ids are used" "want uid $(id -u), got '$off'"
fi

if [ "$on" != "$off" ]; then
    note "the option demonstrably changes what is served" "yes"
else
    bad "the option demonstrably changes what is served" "identical output"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "M43 gate: PASS"
    exit 0
fi
echo "M43 gate: FAIL ($fails)"
exit 1