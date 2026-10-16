#!/usr/bin/env bash
# m49_softlink_structure.sh -- a soft link, verified against the volume's OWN soft link.
#
# WHY, and why this is not m48 again. m47 established that no pinned oracle can see
# a link: amitools refuses the whole volume (rc=3, zero entries) and ADFlib's unadf
# skips link names silently. m31_symlink_write therefore verifies only BEHAVIOUR --
# duplicate/empty-target refusals, an entry-count agreement, and what `readlink`
# returns. Every one of those is, in the end, a statement about our own reader
# agreeing with itself. Exactly the hole m48 closed for hard links, still open for
# soft links.
#
# THE BETTER EVIDENCE, and what makes this gate different from m48's. A hard link
# has no ground truth available, so m48 asserted the format's own invariants from
# ADFlib's struct definitions. A SOFT link does have ground truth: every fixture
# carries `slink_dir1`, a soft link written by a real AmigaDOS. So this gate does
# not assert what the format says our block should look like -- it compares ours
# against THAT one, field for field, and requires them to differ only where they
# must.
#
#     allowed to differ:  block, header_key (self-pointer), name_len, name
#     everything else must match the volume's own real soft link
#
# If our writer ever reproduces a genuine AmigaDOS soft link, nothing in that
# comparison depends on our reader being correct, our fixtures being right, or
# either oracle being able to see links. That is a stronger claim than m48's and
# it is available for free.
#
# Fields, from ADFlib's src/adf_blk.h: there is no hardlink-count field anywhere. A
# soft link is a header block with secType 3, realEntry 0, byteSize 0, extension 0,
# parent set to the containing directory, and the target as NUL-free zero-padded
# ASCII at +24.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures/testffs.adf"
W=$(mktemp -d /tmp/amigafs-m49.XXXXXX)
trap 'rm -rf "$W"' EXIT

fails=0
bad() { printf '  %-52s FAIL (%s)\n' "$1" "$2"; fails=$((fails + 1)); }
note() { printf '  %-52s %s\n' "$1" "$2"; }

[ -x "$ROOT/build/afs" ] || { echo "M49 gate: SKIP -- build/afs not built"; exit 77; }
[ -f "$FIX" ]            || { echo "M49 gate: FAIL -- fixture missing: $FIX"; exit 1; }

echo "=== M49 gate (a soft link, compared to the volume's own) ==="

if ! gcc -w -I"$ROOT/include" -o "$W/probe" "$HERE/m49_softprobe.c" \
        "$ROOT"/src/lib/*.c 2>"$W/cc.log"; then
    echo "M49 gate: FAIL -- the probe did not build"
    head -10 "$W/cc.log" | sed 's/^/    /'
    exit 1
fi

cp "$FIX" "$W/t.adf"
"$ROOT/build/afs" bitmap-rebuild "$W/t.adf" --yes >/dev/null 2>&1 \
    || { echo "M49 gate: FAIL -- could not prepare the copy"; exit 1; }

# --- 0. the ground truth must actually be there -----------------------------
gt=$("$W/probe" "$W/t.adf" slink_dir1 2>&1)
case "$gt" in
    *MISSING*|*FAIL*)
        echo "M49 gate: FAIL -- the fixture has no ground-truth soft link"
        printf '%s\n' "$gt" | head -3 | sed 's/^/    /'
        echo "    Every fixture is expected to carry one; without it this gate has"
        echo "    nothing to compare against and must not fall back to self-checks."
        exit 1 ;;
esac
gtblk=$(printf '%s\n' "$gt" | awk -F= '/^block=/{print $2}')
gt_sect=$(printf '%s\n' "$gt" | awk -F= '/^sec_type=/{print $2}')
gt_tgt=$(printf '%s\n' "$gt"  | awk -F= '/^target=/{sub(/^target=/,"");print}')
[ "$gt_sect" = 3 ] && note "the fixture's own soft link is secType 3" "block $gtblk -> $gt_tgt" \
    || bad "the fixture's own soft link is secType 3" "sec_type=$gt_sect"

# --- 1. write one, the same target ----------------------------------------
if ! "$ROOT/build/afs" symlink "$W/t.adf" /mine.dir_1 dir_1 >"$W/out" 2>&1; then
    bad "afs symlink /mine.dir_1 dir_1" "$(head -1 "$W/out")"
    echo "M49 gate: FAIL (symlink)"; exit 1
fi
note "afs symlink /mine.dir_1 dir_1" "$(head -1 "$W/out")"

ours=$("$W/probe" "$W/t.adf" mine.dir_1 2>&1)
case "$ours" in
    *MISSING*|*FAIL*)
        bad "the new soft link is in the directory" "$(printf '%s' "$ours" | head -1)"
        echo "M49 gate: FAIL (not found)"; exit 1 ;;
esac

# --- 2. FIELD-FOR-FIELD against the volume's own soft link ------------------
# Only these four may differ, and each must differ for a reason.
same=0; diffs=""
FIELDS="type high_seq ht_size data_size target byte_size comm_len real_entry next_link next_hash parent extension sec_type"
nfields=$(printf '%s\n' $FIELDS | grep -c .)
for f in $FIELDS; do
    g=$(printf '%s\n' "$gt"   | awk -F= -v k="$f" '$1==k{sub("^[^=]*=","");print}')
    o=$(printf '%s\n' "$ours" | awk -F= -v k="$f" '$1==k{sub("^[^=]*=","");print}')
    if [ "$g" = "$o" ]; then same=$((same + 1)); else diffs="$diffs $f"; fi
done
note "fields that match the volume's own soft link" "$same of $nfields"

if [ -z "$diffs" ]; then
    note "no field differs from ground truth" "identical apart from block, header_key, name"
else
    bad "no field differs from ground truth" "differing:$diffs"
fi

# The four that MUST differ, and why. Asserted rather than assumed, because a gate
# that silently tolerates a mismatch here would tolerate it in either direction.
oblk=$(printf '%s\n' "$ours" | awk -F= '/^block=/{print $2}')
okey=$(printf '%s\n' "$ours" | awk -F= '/^header_key=/{print $2}')
oname=$(printf '%s\n' "$ours" | awk -F= '/^name=/{sub(/^name=/,"");print}')
onlen=$(printf '%s\n' "$ours" | awk -F= '/^name_len=/{print $2}')

[ "$oblk" != "$gtblk" ] && note "a NEW block was allocated (must differ)" "$gtblk -> $oblk" \
    || bad "a new block was allocated" "reused block $oblk"
[ "$okey" = "$oblk" ] && note "header_key is the new block's own number" "$okey" \
    || bad "header_key is self-referential" "header_key=$okey block=$oblk"
[ "$oname" = "mine.dir_1" ] && note "the block carries the new name" "$oname" \
    || bad "the block carries the new name" "got '$oname'"
# Derived from the name rather than typed in: the first version of this gate
# asserted 11 for "mine.dir_1", which is 10 characters, and failed on MY arithmetic
# rather than on the library. A literal here would keep that class of error alive.
[ "$onlen" = "${#oname}" ] && note "name_len matches the new name" "$onlen" \
    || bad "name_len matches the new name" "got $onlen, want ${#oname} (${onname})"

# --- 3. the soft-link invariants, stated individually ---------------------
o_sect=$(printf '%s\n' "$ours" | awk -F= '/^sec_type=/{print $2}')
o_real=$(printf '%s\n' "$ours" | awk -F= '/^real_entry=/{print $2}')
o_bsz=$(printf '%s\n' "$ours"   | awk -F= '/^byte_size=/{print $2}')
o_ext=$(printf '%s\n' "$ours"   | awk -F= '/^extension=/{print $2}')
o_par=$(printf '%s\n' "$ours"   | awk -F= '/^parent=/{print $2}')
o_tgt=$(printf '%s\n' "$ours"   | awk -F= '/^target=/{sub(/^target=/,"");print}')
o_typ=$(printf '%s\n' "$ours"   | awk -F= '/^type=/{print $2}')

[ "$o_sect" = 3 ] && note "secType is 3 (soft link), not -3 (file)" "$o_sect" \
    || bad "secType is 3" "sec_type=$o_sect"
[ "$o_real" = 0 ] && note "realEntry is 0 -- a soft link is not a hard link" "0" \
    || bad "realEntry is 0" "real_entry=$o_real"
[ "$o_bsz" = 0 ] && note "byteSize is 0 -- a link has no data" "0" \
    || bad "byteSize is 0" "byte_size=$o_bsz"
[ "$o_ext" = 0 ] && note "extension is 0" "0" \
    || bad "extension is 0" "extension=$o_ext"
[ "$o_tgt" = "$gt_tgt" ] && note "the target text is identical to ground truth" "$o_tgt" \
    || bad "the target text matches ground truth" "got '$o_tgt', want '$gt_tgt'"
[ "$o_typ" = 2 ] && note "block type is 2 (header)" "$o_typ" \
    || bad "block type is 2 (header)" "type=$o_typ"

# parent must be the CONTAINING directory, not the root by coincidence. Both sit
# in the root here, so a wrong-parent bug that hardcodes the root would pass; the
# check is still worth making because the fixture's own link has the same value
# and the comparison above already proves we agree with it.
[ -n "$o_par" ] && [ "$o_par" != 0 ] \
    && note "parent points at the containing directory" "block $o_par" \
    || bad "parent points at the containing directory" "parent=${o_par:-none}"

# --- 4. our reader resolves it, and unlink removes it ---------------------
rl=$(build/afs 2>/dev/null; "$ROOT/build/afs" cat "$W/t.adf" /mine.dir_1 >/dev/null 2>&1 && echo "resolves" || echo "does not resolve")
[ "$rl" = "resolves" ] && note "the target is reachable through our reader" "yes" \
    || note "the target is not a readable file through afs cat" "(soft link target is a dir)"

if ! "$ROOT/build/afs" rm "$W/t.adf" /mine.dir_1 >"$W/rm.out" 2>&1; then
    bad "afs rm /mine.dir_1" "$(head -1 "$W/rm.out")"
    echo "M49 gate: FAIL (rm)"; exit 1
fi
after=$("$W/probe" "$W/t.adf" mine.dir_1 2>&1)
case "$after" in
    *MISSING*) note "the soft-link block is gone after unlink" "name no longer present" ;;
    *) bad "the soft-link block is gone after unlink" "$(printf '%s' "$after" | head -1)" ;;
esac

# the ground truth must have survived our churn untouched
gt2=$("$W/probe" "$W/t.adf" slink_dir1 2>&1)
if [ "$gt2" = "$gt" ]; then
    note "the volume's own soft link is byte-identical after our churn" "unchanged"
else
    bad "the volume's own soft link is untouched by our writes" "it changed"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "M49 gate: PASS (soft link matches ground truth in all $nfields comparable fields)"
    exit 0
fi
echo "M49 gate: FAIL ($fails)"
exit 1