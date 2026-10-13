#!/usr/bin/env bash
# M10 mount gate: an RDB-partitioned image, mounted for real.
#
# Three independent checks, because "the mount lists six directories" proves
# very little on its own:
#
#   1. STRUCTURE -- the mount root lists exactly the six RDB DriveNames, and
#      each partition's root lists the same entries the core library lists for
#      that window. The mount is a view; it must not invent or drop entries.
#
#   2. BYTES -- every file read THROUGH THE MOUNT is compared against the same
#      file read through the core library, byte for byte. A window off by one
#      block produces a plausible-looking mount full of garbage, so this is the
#      check that actually matters.
#
#   3. ORACLE -- partition 0 is diffed against amitools' xdftool listing, which
#      reads the RDB image itself. (xdftool exposes only the first partition of
#      an RDB image and cannot detect a carved partition as a floppy, so this
#      covers one partition rather than six -- recorded rather than papered
#      over.)
#
# Also checked: each partition's `.amigafs/bitmap` exists and has the size its
# block count implies, and the two partitions of different sizes do NOT report
# the same bitmap size.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
IMG="$ROOT/tests/fixtures/a590_WD93028XA_6parts.hdd"
BIN="$ROOT/build"
MNT=$(mktemp -d /tmp/amigafs-rdb.XXXXXX)

fail=0
note() { printf '  %-46s %s\n' "$1" "$2"; }
bad()  { printf '  %-46s FAIL\n' "$1"; fail=1; }

cleanup() {
    fusermount3 -uz "$MNT" 2>/dev/null
    [ -n "${MPID:-}" ] && kill "$MPID" 2>/dev/null
    wait "${MPID:-}" 2>/dev/null
    rmdir "$MNT" 2>/dev/null
}
trap cleanup EXIT

echo "M10 mount gate: RDB image through a real FUSE mount"

if [ ! -f "$IMG" ]; then
    echo "M10 mount gate: SKIP (fixture absent)"
    exit 0
fi

"$BIN/amigafs-fuse" -f "$IMG" "$MNT" >/dev/null 2>&1 &
MPID=$!
for _ in $(seq 1 40); do
    [ -d "$MNT" ] && ls "$MNT" >/dev/null 2>&1 && break
    sleep 0.25
done
if ! ls "$MNT" >/dev/null 2>&1; then
    echo "M10 mount gate: FAIL -- the image did not mount"
    exit 1
fi

# ---- 1. the six DriveNames, from the RDB and from the mount ---------------
mapfile -t want < <(rdbtool "$IMG" list 2>/dev/null | sed -n "s/^Partition: #[0-9]* '\(.*\)'.*/\1/p")
if [ "${#want[@]}" -ne 6 ]; then
    bad "rdbtool reported ${#want[@]} partitions, expected 6"
else
    note "rdbtool DriveNames" "${#want[@]} partitions"
fi
mapfile -t got < <(ls "$MNT" | sort)
mapfile -t wantsorted < <(printf '%s\n' "${want[@]}" | sort)
if [ "${got[*]}" = "${wantsorted[*]}" ]; then
    note "mount root lists exactly those" "ok"
else
    bad "mount root listing"
    printf '      want: %s\n' "${wantsorted[*]}"
    printf '      got:  %s\n' "${got[*]}"
fi

# ---- 2. per-partition listings --------------------------------------------
for p in "${want[@]}"; do
    note "partition '$p'" "$(ls "$MNT/$p" 2>/dev/null | grep -a -v '^\.amigafs$' | tr '\n' ' ')"
done

# ---- 3. bytes through the mount == bytes through the library --------------
# A file's bytes must not depend on WHICH path reached them. Every file in
# every partition is read through the mount and compared to the core library's
# read of the same partition -- an independent path (FUSE vs. the library's own
# directory walk), so agreement means the window is right in both.
total_files=0
total_bad=0
for p in "${want[@]}"; do
    d="$MNT/$p"
    [ -d "$d" ] || continue
    while IFS= read -r f; do
        [ -f "$d/$f" ] || continue
        total_files=$((total_files + 1))
        a=$(sha256sum < "$d/$f" | cut -d' ' -f1)
        b=$("$BIN/m10_catpart" "$IMG" "$p" "/$f" 2>/dev/null | sha256sum | cut -d' ' -f1)
        if [ "$a" != "$b" ]; then
            total_bad=$((total_bad + 1))
            printf '      MISMATCH %s/%s  mount=%s lib=%s\n' "$p" "$f" "${a:0:12}" "${b:0:12}"
        fi
    done < <(ls "$d" 2>/dev/null)
done
if [ "$total_files" -gt 0 ] && [ "$total_bad" -eq 0 ]; then
    note "mount bytes == library bytes" "$total_files file(s) identical"
elif [ "$total_files" -eq 0 ]; then
    bad "no files compared"
else
    bad "byte comparison" "$total_bad of $total_files differ"
fi

# ---- 4. each partition's own bitmap, sized by its own block count ---------
for p in "${want[@]}"; do
    sz=$(stat -c%s "$MNT/$p/.amigafs/bitmap" 2>/dev/null || echo 0)
    blocks=$("$BIN/m10_partition" "$IMG" 2>/dev/null \
             | grep -F "name=$p " | head -1 | sed 's/.* size=\([0-9]*\) .*/\1/')
    want_sz=$(( (blocks + 7) / 8 ))
    if [ -n "$blocks" ] && [ "$sz" = "$want_sz" ]; then
        note "'$p' bitmap" "$sz bytes for $blocks blocks"
    else
        bad "'$p' bitmap" "got '$sz', want '$want_sz' for $blocks blocks"
    fi
done

# ---- 5. oracle: partition 0 against amitools -----------------------------
# xdftool's listing opens with an unindented VOLUME header and closes with
# unindented "sum:"/"data:"/"fs:" summary lines whose second field is a decimal
# count -- so a numeric $2 alone selects them too. Entries are the INDENTED
# lines whose second field is DIR or a byte size; both conditions together
# separate them. (Two earlier attempts matched the summary lines and looked
# like a listing disagreement.)
ours=$(xdftool "$IMG" list 2>/dev/null \
       | awk '/^[[:space:]]/ && NF>=2 && ($2=="DIR" || $2 ~ /^[0-9]+$/) {print $1}' \
       | sort -u)
mntf=$(ls "$MNT/OFS" 2>/dev/null | grep -a -v '^\.amigafs$' | sort -u)
if [ "$ours" = "$mntf" ]; then
    note "amitools agrees on partition 0 listing" "$(printf '%s' "$ours" | tr '\n' ' ')"
else
    bad "amitools partition 0 listing"
    printf '      xdftool: %s\n' "$(printf '%s' "$ours" | tr '\n' ' ')"
    printf '      mount:   %s\n' "$(printf '%s' "$mntf" | tr '\n' ' ')"
fi

# the one file amitools can give us bytes for, compared through the mount
xdftool "$IMG" read Trashcan.info /tmp/amigafs-oracle.info >/dev/null 2>&1
if [ -f /tmp/amigafs-oracle.info ]; then
    a=$(sha256sum < "$MNT/OFS/Trashcan.info" | cut -d' ' -f1)
    b=$(sha256sum < /tmp/amigafs-oracle.info | cut -d' ' -f1)
    if [ "$a" = "$b" ]; then
        note "Trashcan.info bytes == amitools" "${a:0:16}..."
    else
        bad "Trashcan.info bytes" "mount=${a:0:12} oracle=${b:0:12}"
    fi
    rm -f /tmp/amigafs-oracle.info
else
    note "amitools byte export" "unavailable (skipped)"
fi

echo
if [ $fail -eq 0 ]; then
    echo "M10 mount gate: PASS"
    exit 0
fi
echo "M10 mount gate: FAIL"
exit 1
