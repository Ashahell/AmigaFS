#!/usr/bin/env bash
# M12 gate: the `afs` CLI on RDB-partitioned images.
#
# The mount has understood partitions since M7/M10 (subdirectories named after
# the DriveName); the CLI did not -- every subcommand opens the whole image as
# one volume, so any RDB image fails with "no DOS filesystem id on block 0 or
# block 1". The CLI and the mount therefore disagree about what a disk
# contains, against the principle stated in src/cli/main.c's own header.
#
# What each check proves (and what it does NOT):
#   - `info` table vs `rdbtool`: independent oracle, exact diff. Catches a
#     wrong table read. Says nothing about windows.
#   - `ls -R` / `cat` per partition vs the FUSE mount: catches CLI plumbing
#     bugs (wrong partition selected, wrong path split, off-by-one window
#     surfacing as garbage). The mount is independently oracle-gated
#     (m10_mount_gate vs xdftool), so this is transitive trust -- stated, not
#     hidden. It CANNOT catch core-library bugs: both sides read through the
#     same libafs.
#   - `cat` bytes of one file also vs `xdftool` (partition 0): ends the
#     transitivity at a second independent oracle.
#   - floppy positive control (Rule 6): the same binary must still list and
#     cat a floppy byte-identically vs adfls/unadf. Proves the harness ran and
#     the old path is untouched.
#   - negatives name the reason: unknown partition, bitmap without partition.
set -uo pipefail

# ADFlib's CLI is the oracle. Every other oracle gate puts it on PATH itself
# (overridable with ADFLIB_BIN); this one did not, so it passed only when the
# ambient PATH happened to have adfls/unadf in it and FAILED otherwise -- under
# ctest, which does not inherit a shell's exports. A gate that fails for want
# of an environment variable reads exactly like a real regression.
ADFLIB_BIN="${ADFLIB_BIN:-$HOME/Work/adflib-oracle/ADFlib/build/examples}"
export PATH="$ADFLIB_BIN:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
IMG="$ROOT/tests/fixtures/a590_WD93028XA_6parts.hdd"
FLOPPY="$ROOT/tests/fixtures/testffs.adf"
BIN="$ROOT/build"
MNT=$(mktemp -d /tmp/amigafs-m12.XXXXXX)

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

echo "M12 gate: CLI on RDB-partitioned images"

if [ ! -f "$IMG" ]; then
    echo "M12 gate: SKIP (fixture $IMG absent)"
    exit 0
fi
if [ ! -x "$BIN/afs" ]; then
    echo "M12 gate: SKIP (build afs first)"
    exit 0
fi
for tool in adfls unadf rdbtool xdftool; do
    if ! command -v "$tool" >/dev/null; then
        echo "M12 gate: SKIP (oracle '$tool' not on PATH; set ADFLIB_BIN for ADFlib)"
        exit 0
    fi
done

# ---- 0. positive control: the floppy path still works ----------------------
# adfls/unadf are the independent oracles; exact match, bytes included.
# adfls lists the ROOT only, one bare name per line; `afs ls` without -R and
# without a dir does the same, prefixed by kind+nlink ("d1 /dir_1"), so strip
# exactly that prefix. Both sides sorted: neither tool promises order.
if [ -f "$FLOPPY" ]; then
    adfls_out=$(adfls "$FLOPPY" 2>/dev/null | sort)
    afs_out=$("$BIN/afs" ls "$FLOPPY" 2>/dev/null | sed 's/^[a-zA-Z][0-9]* \///' | sort)
    if [ -n "$adfls_out" ] && [ "$adfls_out" = "$afs_out" ]; then
        note "floppy control: ls == adfls" "ok"
    else
        bad "floppy control: ls vs adfls"
        diff <(printf '%s\n' "$adfls_out") <(printf '%s\n' "$afs_out") | head -10 | sed 's/^/      /'
    fi
    # unadf writes `.info` sidecars for entries with comments -- extract to a
    # scratch dir and filter `*.info` on BOTH sides of the diff, and hash from
    # stdin (GNU sha256sum escapes backslashes in filenames, which matters
    # for fixtures like win32-names.adf).
    ud=$(mktemp -d /tmp/amigafs-m12-unadf.XXXXXX)
    if unadf -d "$ud" "$FLOPPY" >/dev/null 2>&1; then
        mism=0; n=0
        while IFS= read -r -d '' rf; do
            case "$rf" in *.info) continue;; esac
            [ -f "$rf" ] || continue
            rel="${rf#$ud/}"
            n=$((n + 1))
            a=$(sha256sum < "$rf" | cut -d' ' -f1)
            b=$("$BIN/afs" cat "$FLOPPY" "/$rel" 2>/dev/null | sha256sum | cut -d' ' -f1)
            if [ "$a" != "$b" ]; then
                mism=$((mism + 1))
                printf '      MISMATCH %s unadf=%s afs=%s\n' "$rel" "${a:0:12}" "${b:0:12}"
            fi
        done < <(find "$ud" -type f -print0 2>/dev/null)
        if [ "$n" -gt 0 ] && [ "$mism" -eq 0 ]; then
            note "floppy control: cat bytes == unadf" "$n files identical"
        else
            bad "floppy control: cat bytes" "$mism of $n differ"
        fi
    else
        bad "floppy control: unadf extraction" "unadf failed"
    fi
    rm -rf "$ud"
fi

# ---- 1. info table vs rdbtool (exact) --------------------------------------
info_out=$("$BIN/afs" info "$IMG" 2>&1)
info_rc=$?
if [ $info_rc -ne 0 ]; then
    bad "afs info on RDB image" "rc=$info_rc: $(printf '%s' "$info_out" | head -1)"
else
    # normalise both sides to "name|dostype|size" (DriveNames contain spaces;
    # cut by shape, never by whitespace position)
    ours=$(printf '%s\n' "$info_out" | awk '/^part [0-9]+ / { rest=$0; sub(/^part [0-9]+ /,"",rest); name=rest; sub(/^name=/,"",name); sub(/ dos=.*/,"",name); dos=rest; sub(/.* dos=/,"",dos); sub(/ .*/,"",dos); size=rest; sub(/.* size=/,"",size); sub(/ .*/,"",size); printf "%s|%s|%s\n", name, dos, size; }')
    theirs=$(rdbtool "$IMG" list 2>/dev/null | awk -v q="'" '/^Partition: #[0-9]+ / { line=$0; sub(/^Partition: #[0-9]+ /,"",line); split(line, a, q); name=a[2]; rest=a[3]; m=split(rest, f, /[ \t]+/); size=""; d=""; for (i=1; i<=m; i++) { if (f[i] == "") continue; if (f[i] ~ /^DOS[0-9]\//) { d=f[i]; sub(/^DOS[0-9]\//,"",d); } else if (size == "" && f[i] ~ /^[0-9]+$/ && i >= 4) size=f[i]; } printf "%s|%s|%s\n", name, d, size; }')
    if [ -n "$ours" ] && [ "$ours" = "$theirs" ]; then
        note "info table == rdbtool" "$(printf '%s\n' "$ours" | wc -l | tr -d ' ') partitions"
    else
        bad "info table vs rdbtool"
        diff <(printf '%s\n' "$theirs") <(printf '%s\n' "$ours") | head -20 | sed 's/^/      /'
    fi
fi

# ---- 1b. bare `ls` lists the partitions, as the mount root does -------------
# Both spellings (no dir, "/") must agree with each other and with the table.
bare1=$("$BIN/afs" ls "$IMG" 2>/dev/null | sort)
bare2=$("$BIN/afs" ls "$IMG" / 2>/dev/null | sort)
want=$(printf '%s\n' "${ours:-}" | awk -F'|' 'NF==3 {printf "d1 %s\n", $1}' | sort)
if [ "$bare1" = "$want" ] && [ "$bare2" = "$want" ] && [ -n "$want" ]; then
    note "bare ls lists partitions" "$(printf '%s\n' "$want" | wc -l | tr -d ' ') names"
else
    bad "bare ls lists partitions"
    diff <(printf '%s\n' "$want") <(printf '%s\n' "$bare1") | head -10 | sed 's/^/      /'
fi

# ---- mount once for the CLI-vs-mount checks --------------------------------
"$BIN/amigafs-fuse" -f "$IMG" "$MNT" >/dev/null 2>&1 &
MPID=$!
for _ in $(seq 1 40); do
    [ -d "$MNT" ] && ls "$MNT" >/dev/null 2>&1 && break
    sleep 0.25
done
if ! ls "$MNT" >/dev/null 2>&1; then
    echo "M12 gate: FAIL -- the image did not mount (cannot check CLI-vs-mount)"
    exit 1
fi

# ---- 2. info <part> per partition ------------------------------------------
# Rootblock must equal (2 + last)/2 of the reported size AND the DOS type must
# be the one the RDB advertises for THAT partition (parsed off the `DOS\N`
# display). The second half is load-bearing: a resolver that always opens
# partition 0 reports root=3078 for five partitions -- which the size check
# alone accepts -- but shows DOS\0 where the RDB says DOS\2.
for part in OFS "OFS INTL" "OFS DirCache" FFS "FFS INTL" "FFS DirCache"; do
    out=$("$BIN/afs" info "$IMG" "$part" 2>&1)
    if [ $? -ne 0 ]; then
        bad "info $part" "$(printf '%s' "$out" | head -1)"
        continue
    fi
    root=$(printf '%s\n' "$out" | sed -n 's/^rootblock: *//p')
    dos=$(printf '%s\n' "$out" | sed -n 's/^dos type: *//p')
    dosnum=$(printf '%s' "$dos" | sed -n 's/^DOS\\\([0-7]\).*/\1/p')
    wantdos=$(rdbtool "$IMG" list 2>/dev/null | awk -v q="'" -v want="$part" '/^Partition: / { line=$0; sub(/^Partition: #[0-9]+ /,"",line); split(line, a, q); if (a[2] == want) print a[3]; }' | grep -o 'DOS[0-9]/' | grep -o '[0-9]' | head -1)
    blocks=$(printf '%s\n' "$out" | sed -n 's/^blocks: *\([0-9]*\).*/\1/p')
    if [ -n "$blocks" ] && [ "$root" = "$(( (2 + blocks - 1) / 2 ))" ] && [ -n "$dos" ] && [ "$dosnum" = "$wantdos" ]; then
        note "info $part" "root=$root dos='$dos'"
    else
        bad "info $part" "root=$root blocks=$blocks dos='$dos' (rdb says DOS\\$wantdos)"
    fi
done

# ---- 3. ls -R per partition vs mount (exact, full relative paths) ---------
# The two listings have different formats (CLI prints `kind nlink path`, one
# per line, paths relative to the partition root; mount `ls -R` prints bare
# basenames under `dir:` headers), so both sides are normalised to sorted
# relative paths. `find` on the mount preserves directory structure, which a
# basename-only comparison would collapse. The `.amigafs` synthetic subtree
# exists only in the mount (decision D4), so it is excluded by path.
for part in OFS "OFS INTL" "OFS DirCache" FFS "FFS INTL" "FFS DirCache"; do
    a=$("$BIN/afs" ls "$IMG" "$part" -R 2>/dev/null | sed 's/^[a-zA-Z][0-9]* \///' | sort)
    b=$(find "$MNT/$part" -mindepth 1 2>/dev/null | sed "s|^$MNT/$part/||" | grep -a -v '^\.amigafs' | sort)
    if [ "$a" = "$b" ] && [ -n "$a" ]; then
        note "ls -R $part == mount" "$(printf '%s\n' "$a" | wc -l | tr -d ' ') entries"
    else
        bad "ls -R $part vs mount"
        diff <(printf '%s\n' "$b") <(printf '%s\n' "$a") | head -10 | sed 's/^/      /'
    fi
done

# ---- 4. cat bytes vs mount, plus xdftool for partition 0 ------------------
total=0
mism=0
while IFS= read -r -d '' rf; do
    rel="${rf#$MNT/}"
    case "$rel" in .amigafs/*|*/.amigafs/*) continue;; esac
    [ -f "$rf" ] || continue
    total=$((total + 1))
    part="${rel%%/*}"; path="/${rel#*/}"
    a=$(sha256sum < "$rf" | cut -d' ' -f1)
    c=$("$BIN/afs" cat "$IMG" "$part$path" 2>/dev/null | sha256sum | cut -d' ' -f1)
    if [ "$a" != "$c" ]; then
        mism=$((mism + 1))
        printf '      MISMATCH %s  mount=%s cli=%s\n' "$rel" "${a:0:12}" "${c:0:12}"
    fi
done < <(find "$MNT" -type f -print0 2>/dev/null)
if [ "$total" -gt 0 ] && [ "$mism" -eq 0 ]; then
    note "cat bytes == mount" "$total files identical"
else
    bad "cat bytes" "$mism of $total differ"
fi
# xdftool reads partition 0 of the RDB itself: ends transitivity there.
# Only indented entry lines count -- the VOLUME header and the sum:/data:/fs:
# summary lines must not be mistaken for files (sum: has a numeric $2, so
# match on shape AND exclude labels ending in a colon).
xo=$(xdftool "$IMG" list 2>/dev/null | awk '$1 !~ /:$/ && NF>=2 && ($2=="DIR" || $2 ~ /^[0-9]+$/) {print $1}')
for f in $xo; do
    [ "$f" = "Trashcan" ] && continue # directories have no bytes to compare
    if xdftool "$IMG" read "$f" /tmp/amigafs-m12-oracle 2>/dev/null && [ -f /tmp/amigafs-m12-oracle ]; then
        a=$(sha256sum < /tmp/amigafs-m12-oracle | cut -d' ' -f1)
        c=$("$BIN/afs" cat "$IMG" "OFS/$f" 2>/dev/null | sha256sum | cut -d' ' -f1)
        if [ -n "$a" ] && [ "$a" = "$c" ]; then
            note "cat OFS/$f == xdftool" "${a:0:12}..."
        else
            bad "cat OFS/$f" "xdftool=${a:0:12} cli=${c:0:12}"
        fi
    else
        bad "cat OFS/$f" "xdftool read failed"
    fi
    rm -f /tmp/amigafs-m12-oracle
done

# ---- 5. verify + bitmap per partition --------------------------------------
vout=$("$BIN/afs" verify "$IMG" 2>&1)
vrc=$?
if [ $vrc -eq 0 ]; then
    note "verify all partitions" "clean"
else
    bad "verify all partitions" "rc=$vrc: $(printf '%s' "$vout" | head -3 | tr '\n' ';')"
fi
for part in OFS "OFS INTL" "OFS DirCache" FFS "FFS INTL" "FFS DirCache"; do
    out=$("$BIN/afs" bitmap "$IMG" "$part" 2>/dev/null)
    used=$(printf '%s\n' "$out" | sed -n 's/^used: *//p')
    free=$(printf '%s\n' "$out" | sed -n 's/^free: *//p')
    size=$(rdbtool "$IMG" list 2>/dev/null | awk -v q="'" -v want="$part" '/^Partition: / { line=$0; sub(/^Partition: #[0-9]+ /,"",line); split(line, a, q); if (a[2] == want) { m=split(a[3], f, /[ \t]+/); for (i=1; i<=m; i++) if (f[i] ~ /^[0-9]+$/ && i >= 4 && s == "") s=f[i]; print s; } }' | head -1)
    if [ -n "$used" ] && [ -n "$free" ] && [ "$((used + free))" = "$size" ]; then
        note "bitmap $part" "used=$used free=$free total=$size"
    else
        bad "bitmap $part" "used=$used free=$free rdbtool-size=$size"
    fi
done

# ---- 6. negatives name the reason ------------------------------------------
out=$("$BIN/afs" ls "$IMG" "NoSuchPartition" 2>&1)
if [ $? -ne 0 ] && printf '%s' "$out" | grep -q "NoSuchPartition"; then
    note "unknown partition refused by name" "ok"
else
    bad "unknown partition" "rc=$? out=$(printf '%s' "$out" | head -1)"
fi
out=$("$BIN/afs" bitmap "$IMG" 2>&1)
if [ $? -ne 0 ] && printf '%s' "$out" | grep -qi "partition"; then
    note "bitmap without partition refused" "ok"
else
    bad "bitmap without partition" "rc=$? out=$(printf '%s' "$out" | head -1)"
fi

echo
if [ $fail -eq 0 ]; then
    echo "M12 gate: PASS"
    exit 0
fi
echo "M12 gate: FAIL"
exit 1
