#!/usr/bin/env bash
# m23_hash_unreachable.sh — a mutated name is unreachable BY THE FORMAT, and the
# gate must not call that a mount defect.
#
# THE DEFECT THIS PINS. AmigaDOS files a directory entry at
# bucket = hash(name) % ht_size. readdir walks EVERY bucket, so it still shows
# a name whose bytes no longer hash to its own bucket; lookup computes the hash
# of the name it is given and reads THAT bucket, where the entry is not. So on
# any image whose name bytes have been mutated:
#
#     readdir  -> lists the name
#     lookup   -> ENOENT
#
# and that is the format working, not a bug. Measured on this tree:
# 'multidef' hashes to bucket 46; mutating one byte to 0xFE gives bucket 16;
# the entry stays filed at 46. ADFlib uses the same hash and the same bucket
# lookup, so it behaves identically.
#
# This bit mount_fuzz's black-box probe ~1 run in 40 and cost a full
# investigation: the probe counted ANY lstat failure as a mount defect, so a
# correct refusal was reported as "probe saw an unsanctioned failure".
#
# The fix does NOT whitelist ENOENT. A mount whose lookup was broken entirely
# would ENOENT on every name and then pass. So the probe classifies ENOENT
# separately and mount_fuzz.sh CROSS-CHECKS each one against the library, which
# is the format's own authority: if `afs ls` also cannot find the name, the
# VOLUME is at fault and the refusal is correct; if `afs ls` CAN find it, the
# MOUNT lost a reachable entry and that is a real defect.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
AFS="$REPO/build/afs"
WORK="$(mktemp -d /tmp/amigafs-m23.XXXXXX)"
trap 'fusermount3 -uz "$WORK/mnt" 2>/dev/null; rm -rf "$WORK"' EXIT

fail=0
ok()  { echo "  PASS $1"; }
bad() { echo "  FAIL $1"; fail=1; }

echo "M23 gate: a mutated name is unreachable by the format, not by the mount"

if [ ! -x "$AFS" ]; then echo "M23: $AFS not built" >&2; exit 1; fi
if ! command -v fusermount3 >/dev/null 2>&1; then
    echo "M23: SKIP - no fusermount3" >&2; exit 77
fi

SRC="$REPO/tests/fixtures/ffdisk0049.adf"
[ -f "$SRC" ] || { echo "M23: fixture absent" >&2; exit 1; }

# Mutate one byte of the name 'multidef' -- only the occurrence carrying a
# matching BCPL length byte, so the name is not silently clamped to 30 chars.
IMG="$WORK/mutated.adf"
cp "$SRC" "$IMG"
python3 - "$IMG" <<'PY'
import sys
p = sys.argv[1]
data = bytearray(open(p, "rb").read())
needle = b"\x08multidef"          # length byte 8, then the name
i = data.find(needle)
if i < 0:
    sys.exit("no length-byte-8 'multidef' in the fixture")
data[i + 1 + 2] = 0xFE            # 'l' -> 0xFE
open(p, "wb").write(data)
print("  (mutated one name byte)")
PY

NAME=$(printf 'mu\xFEtidef')
MNT="$WORK/mnt"; mkdir -p "$MNT"

# ---- 1. the volume itself cannot reach the mutated name -------------------
# This is the baseline: the format's authority must ALSO fail to find it. If it
# could find it, the whole premise is wrong and this gate means nothing.
if out=$("$AFS" ls "$IMG" "/Multidef/$NAME" 2>&1); then
    bad "the library FOUND the mutated name -- the premise of this gate is wrong"
    echo "      $out"
else
    ok "library also cannot reach the mutated name (same hash-table rule)"
fi

# ---- 2. readdir still SHOWS it (bucket walk, not a lookup) ----------------
lst=$("$AFS" ls "$IMG" /Multidef 2>/dev/null | cat -v)
if printf '%s' "$lst" | grep -q "muM-~tidef"; then
    ok "readdir still lists the mutated name (it walks every bucket)"
else
    bad "readdir no longer lists the mutated name: $lst"
fi

# ---- 3. the real mount: readdir shows it, stat fails ----------------------
"$REPO/build/amigafs-fuse" -f "$IMG" "$MNT" >/dev/null 2>&1 &
MPID=$!
for _ in $(seq 1 40); do
    grep -qF " $MNT fuse " /proc/self/mounts 2>/dev/null && break
    kill -0 "$MPID" 2>/dev/null || break
    sleep 0.25
done

if grep -qF " $MNT fuse " /proc/self/mounts 2>/dev/null; then
    ok "mount established"
    mls=$(ls "$MNT/Multidef" 2>/dev/null | cat -v)
    if printf '%s' "$mls" | grep -q "muM-~tidef"; then
        ok "the mount's readdir also lists the mutated name"
    else
        bad "the mount's readdir does not list it: $mls"
    fi
    if stat -c %s "$MNT/Multidef/$NAME" >/dev/null 2>&1; then
        bad "the mount stat SUCCEEDED on a name whose bucket no longer matches"
    else
        ok "the mount's stat refuses it, matching the volume"
    fi
    # And a name that IS reachable must still work, or the gate proves nothing.
    if [ -f "$MNT/Multidef/POSTER" ] || [ -f "$MNT/Multidef/Multidef.c" ]; then
        ok "a reachable sibling still resolves through the mount"
    else
        bad "even reachable siblings failed; the mount is simply broken"
    fi
else
    bad "could not mount the mutated image"
fi

fusermount3 -u "$MNT" >/dev/null 2>&1
for _ in $(seq 1 20); do kill -0 "$MPID" 2>/dev/null || break; sleep 0.1; done
kill "$MPID" 2>/dev/null; wait "$MPID" 2>/dev/null

echo
if [ "$fail" -eq 0 ]; then echo "m23_hash_unreachable: PASS"; exit 0; fi
echo "m23_hash_unreachable: FAIL"; exit 1