#!/usr/bin/env bash
# M11 gate: comments, diffed against amitools.
#
# Our reader and the oracle must agree exactly on WHICH entries carry a
# comment, on WHICH block, and WHAT it says. The oracle
# (tests/oracle/comment_oracle.py) drives amitools' own EntryBlock and
# DirCacheRecord parsers over the raw bytes, so this is a real cross-check
# against the reference implementation rather than against our own fixtures.
#
# Three things are checked beyond the plain diff:
#
#   1. the comment field must be present on BOTH a DIRC and a non-DIRC volume
#      (it lives in the entry block everywhere; the dir cache is only a
#      redundant copy);
#   2. a fixture known to have no comments must yield none -- a reader that
#      returned the name, or a stray length byte, would show up here;
#   3. the two copies of the SAME comment -- entry block and dir cache -- must
#      agree. A reader wrong in one place and right in the other disagrees
#      instead of agreeing by luck, which is a much sharper signal than either
#      copy alone.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures"
BIN="$ROOT/build"
ORACLE="$HERE/comment_oracle.py"

fail=0
note() { printf '  %-46s %s\n' "$1" "$2"; }
bad()  { printf '  %-46s FAIL\n' "$1"; fail=1; }

echo "M11 gate: entry comments vs amitools"

if ! python3 -c "import amitools" >/dev/null 2>&1; then
    echo "M11 gate: SKIP (amitools not importable by this python)"
    exit 0
fi
if [ ! -x "$BIN/m11_comment" ]; then
    echo "M11 gate: SKIP (build m11_comment first)"
    exit 0
fi

images=$(ls "$FIX"/*.adf 2>/dev/null)
[ -n "$images" ] || { echo "M11 gate: SKIP (no fixtures)"; exit 0; }

# ---- 0. restrict the oracle to volumes we can actually open ---------------
# We refuse DOS\6/DOS\7 by name (decision C1) and amitools happily reads them,
# so the oracle must not be asked to compare a volume we never claimed to
# support. Filtering by whether OUR reader opens the image keeps the
# difference attributed to the refusal rather than to the comment offset.
openable=""
for img in $images; do
    if "$BIN/afs" info "$img" >/dev/null 2>&1; then
        openable="$openable $img"
    else
        note "$(basename "$img")" "refused by us; excluded from the diff"
    fi
done
# shellcheck disable=SC2086
images="$openable"

# ---- 1. exact diff, whole fixture set -------------------------------------
ours=$(mktemp); theirs=$(mktemp)
trap 'rm -f "$ours" "$theirs"' EXIT

# shellcheck disable=SC2086
"$BIN/m11_comment" $images > "$ours" 2>/dev/null
# drop ONLY the dir-cache lines (tagged "dircache:<offset>" in field 3); the
# oracle emits genuine directories as "<img> <blk> dir <comment>", and an
# earlier filter on $3 != "dir" threw those away as well.
python3 "$ORACLE" $images 2>/dev/null \
    | awk '$3 !~ /^dircache:/' | sort > "$theirs"
sort -o "$ours" "$ours"

if diff -u "$theirs" "$ours" > /tmp/amigafs_m11.diff 2>&1; then
    note "our comments == amitools (entry blocks)" \
         "$(wc -l < "$ours" | tr -d ' ') comment(s) matched"
else
    bad "comment diff vs amitools"
    sed 's/^/      /' /tmp/amigafs_m11.diff | head -30
fi

# ---- 2. coverage: a comment on a DIRC and on a non-DIRC volume ------------
dirc=$(grep -c '^testffs.adf ' "$ours" || true)
nondirc=$(grep -c '^testofs.adf ' "$ours" || true)
if [ "$dirc" -gt 0 ] && [ "$nondirc" -gt 0 ]; then
    note "found on a DIRC and a non-DIRC volume" "testffs=$dirc testofs=$nondirc"
else
    bad "comment coverage" "dirc=$dirc non-dirc=$nondirc (both must be > 0)"
fi

# ---- 3. a known-comment-free fixture yields none --------------------------
strays=$(grep -vE '^(testffs|testhd|testofs|g1a30c|cache_crash)\.adf ' "$ours" | wc -l | tr -d ' ')
if [ "$strays" -eq 0 ]; then
    note "fixtures without comments yield none" "ok"
else
    bad "unexpected comments" "$strays line(s) from comment-free fixtures"
fi

# ---- 4. the entry-block copy and the dir-cache copy agree -----------------
# The oracle emits the dircache copy on separate lines tagged "dircache:<off>".
# shellcheck disable=SC2086
python3 "$ORACLE" $images 2>/dev/null > /tmp/amigafs_m11_all.txt
mismatch=0; checked=0
while read -r img blk cmt; do
    checked=$((checked + 1))
    if ! grep -qF "$img $blk " "$ours" >/dev/null 2>&1 \
       || ! grep -F "$img $blk " "$ours" | grep -qF "$cmt"; then
        mismatch=$((mismatch + 1))
        printf '      DIRC copy %s:%s has no matching entry-block comment: %s\n' \
               "$img" "$blk" "$cmt"
    fi
done < <(awk '$3 ~ /^dircache:/ { s=""; for (i=4;i<=NF;i++) s=s (i>4?" ":"") $i; print $1, $2, s }' \
             /tmp/amigafs_m11_all.txt)
if [ "$mismatch" -eq 0 ] && [ "$checked" -gt 0 ]; then
    note "dir-cache copy agrees with the entry block" "$checked comment(s) cross-checked"
else
    bad "dir-cache vs entry-block agreement" "$mismatch of $checked differ"
fi

# ---- 5/6. through a real mount, as user.amiga.comment ---------------------
# The library agreeing with the oracle is not enough: the value has to survive
# FUSE's getxattr/listxattr, INCLUDING the negative cases -- an attribute that
# is listed but then cannot be served is worse than one not listed at all.
MNT=$(mktemp -d /tmp/amigafs-m11.XXXXXX)
MPID=""
cleanup() {
    fusermount3 -uz "$MNT" 2>/dev/null
    [ -n "$MPID" ] && kill "$MPID" 2>/dev/null
    wait "$MPID" 2>/dev/null
    rmdir "$MNT" 2>/dev/null
    rm -f "$ours" "$theirs"
}
trap cleanup EXIT

mount_it() {
    "$BIN/amigafs-fuse" -f "$1" "$MNT" >/dev/null 2>&1 &
    MPID=$!
    for _ in $(seq 1 40); do
        ls "$MNT" >/dev/null 2>&1 && return 0
        sleep 0.25
    done
    return 1
}

unmount_it() {
    fusermount3 -u "$MNT" 2>/dev/null
    kill "$MPID" 2>/dev/null
    wait "$MPID" 2>/dev/null
    MPID=""
}

if [ -e /dev/fuse ] && command -v fusermount3 >/dev/null 2>&1 \
   && command -v getfattr >/dev/null 2>&1; then
    if mount_it "$FIX/testffs.adf"; then
        v1=$(getfattr -d -m - --only-values "$MNT/mod.And.DistantCall" 2>/dev/null)
        v2=$(getfattr -d -m - --only-values "$MNT/dir_1" 2>/dev/null)
        if [ "$v1" = "protracker module" ]; then
            note "mount: a file's comment" "\"$v1\""
        else
            bad "mount: file comment" "got '$v1', want 'protracker module'"
        fi
        if [ "$v2" = "hlink_dir1 comment" ]; then
            note "mount: a directory's comment" "\"$v2\""
        else
            bad "mount: directory comment" "got '$v2', want 'hlink_dir1 comment'"
        fi

        strays=0
        for e in empty_dir emptyfile secret.S same_hash; do
            getfattr -d -m - "$MNT/$e" 2>/dev/null | grep -q amiga.comment && strays=$((strays+1))
            getfattr -m - "$MNT/$e" 2>/dev/null | grep -q amiga.comment && strays=$((strays+1))
        done
        if [ $strays -eq 0 ]; then
            note "mount: comment-free entries stay silent" "getxattr and listxattr"
        else
            bad "mount: comment-free entries" "$strays unexpected attribute(s)"
        fi

        synth=0
        for p in "$MNT/.amigafs/bitmap" "$MNT"; do
            getfattr -d -m - "$p" 2>/dev/null | grep -q amiga.comment && synth=$((synth+1))
        done
        if [ $synth -eq 0 ]; then
            note "mount: synthetic and root nodes refuse cleanly" "ok"
        else
            bad "mount: synthetic nodes" "$synth served an attribute"
        fi

        unmount_it

        # A NON-DIRC volume exposes comments too: the field is in the entry
        # block everywhere and the dir cache is only a redundant copy. If this
        # ever fails, someone has started reading comments from the cache.
        if mount_it "$FIX/testofs.adf"; then
            v=$(getfattr -d -m - --only-values "$MNT/MOON.GIF" 2>/dev/null)
            if [ "$v" = "comment of MOON.GIF" ]; then
                note "mount: a non-DIRC volume exposes a comment" "\"$v\""
            else
                bad "mount: non-DIRC comment" "got '$v', want 'comment of MOON.GIF'"
            fi
        else
            bad "mount: testofs.adf did not mount"
        fi
    else
        bad "mount: testffs.adf did not mount"
    fi
else
    note "mount checks" "skipped (no /dev/fuse, fusermount3 or getfattr)"
fi

echo
if [ $fail -eq 0 ]; then
    echo "M11 gate: PASS"
    exit 0
fi
echo "M11 gate: FAIL"
exit 1
