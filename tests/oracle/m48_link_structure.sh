#!/usr/bin/env bash
# m48_link_structure.sh -- verify the hard link on DISK, not just through our reader.
#
# WHY. m47 established the constraint this exists to work around. On any volume
# carrying a hard link, amitools refuses the whole volume (rc=3, zero entries) and
# ADFlib's unadf silently skips link names. So there is NO oracle that can look at
# a hard link. m25_fuse_link and m32_link_unlink therefore verify only st_nlink,
# inode identity and byte equality -- all THROUGH OUR OWN MOUNT, which means our
# reader agreeing with itself.
#
# The third kind of evidence, and the one this gate uses: the FORMAT. AmigaDOS
# expresses a hard link structurally, not with a count. A linked name is a *link
# block* (secType -4 for a file, 4 for a directory, 3 for a soft link) whose
# realEntry (0x1d4) points at the real file's header block, and link blocks for the
# same file are chained through nextLink (0x1d8). Every block also carries a
# self-pointer headerKey (0x004) equal to its own block number.
#
#     >>> There is NO hardlink-count field on an FFS file header. <<<
#
# The number of names is implied by the length of the chain. Asserting "hardlinks
# == 2 at some offset" would assert a field that does not exist -- which is why
# this gate asserts STRUCTURE. Offsets are taken from ADFlib's own definitions
# (src/adf_blk.h), not from memory.
#
# WHY STRUCTURE IS WORTH A GATE. These invariants are properties of the on-disk
# format, read from raw blocks. They do not require this implementation's reader to
# be correct -- a reader bug cannot make a wrong secType or a dangling realEntry
# look right. So this is the first check on the link path backed by anything other
# than us.
#
# One external check remains available and is asserted too: the volume must still
# be readable by ADFlib afterwards, with the linked file's bytes intact. Writing a
# link must not damage what the oracle can still see.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures/testffs.adf"
W=$(mktemp -d /tmp/amigafs-m48.XXXXXX)
trap 'rm -rf "$W"' EXIT

fails=0
bad() { printf '  %-52s FAIL (%s)\n' "$1" "$2"; fails=$((fails + 1)); }
note() { printf '  %-52s %s\n' "$1" "$2"; }

command -v unadf >/dev/null 2>&1 || { echo "M48 gate: SKIP -- unadf not on PATH"; exit 0; }
[ -x "$ROOT/build/afs" ] || { echo "M48 gate: SKIP -- build/afs not built"; exit 0; }
[ -f "$FIX" ]            || { echo "M48 gate: FAIL -- fixture missing: $FIX"; exit 1; }

echo "=== M48 gate (a hard link, verified on disk) ==="

# The probe reads raw blocks. If it does not build, SKIP loudly -- a probe that
# cannot compile must never look like a volume with no links.
if ! gcc -w -I"$ROOT/include" -o "$W/probe" "$HERE/m48_linkprobe.c" \
        "$ROOT"/src/lib/*.c 2>"$W/cc.log"; then
    echo "M48 gate: FAIL -- the probe did not build"
    head -10 "$W/cc.log" | sed 's/^/    /'
    exit 1
fi

cp "$FIX" "$W/t.adf"
"$ROOT/build/afs" bitmap-rebuild "$W/t.adf" --yes >/dev/null 2>&1 \
    || { echo "M48 gate: FAIL -- could not prepare the copy"; exit 1; }

# The fixture carries orphans already (M33), so the assertion is that our writes
# ADD none -- not that the count is zero. Measured 216 before and after.
orph0=$("$ROOT/build/afs" verify "$W/t.adf" --orphans 2>&1 | grep -c "leaked")

size_before=$(cd "$W" && rm -rf ex && unadf -d ex t.adf >/dev/null 2>&1 \
              && stat -c%s ex/secret.S 2>/dev/null)
n_before=$(cd "$W/ex" 2>/dev/null && ls 2>/dev/null | wc -l)

# --- 1. create the link, on disk ------------------------------------------
if ! "$ROOT/build/afs" link "$W/t.adf" /secret.S secret.bak >"$W/link.out" 2>&1; then
    bad "afs link /secret.S secret.bak" "$(head -1 "$W/link.out")"
    echo "M48 gate: FAIL (link)"; exit 1
fi
note "afs link /secret.S secret.bak" "$(head -1 "$W/link.out")"

p=$("$W/probe" "$W/t.adf" secret.S secret.bak 2>&1)

real_blk=$(printf '%s\n' "$p" | awk '/^HDR /{print $2; exit}')
real_type=$(printf '%s\n' "$p" | awk '/^HDR /{print $1; exit}')
[ "$real_type" = "HDR" ] && [ -n "$real_blk" ] \
    && note "the real file is a file header (secType -3)" "block $real_blk" \
    || bad "the real file is a file header" "$(printf '%s' "$p" | head -1)"

printf '%s\n' "$p" | grep -q '^HDRKEYBAD' \
    && bad "the file header's headerKey is its own block number" "see probe output" \
    || note "the file header's headerKey is its own block number" "block $real_blk"

link_blk=$(printf '%s\n' "$p" | awk '/^LINK /{print $2; exit}')
link_real=$(printf '%s\n' "$p" | awk '/^LINK /{sub(".*real=","");print $1; exit}')
link_key=$(printf '%s\n' "$p" | awk '/^LINK /{for(i=1;i<=NF;i++) if($i ~ /^key=/) {sub("key=","",$i); print $i; exit}}')
link_name=$(printf '%s\n' "$p" | awk '/^LINK /{$1="";$2="";$3="";$4="";$5="";sub(/^ +/,"");print;exit}')

[ -n "$link_blk" ] && note "a LINK BLOCK was allocated for the new name" "block $link_blk" \
    || bad "a link block was allocated" "$(printf '%s' "$p" | tr '\n' ' ')"

# The load-bearing structural assertion: the link points at the REAL header.
[ -n "$link_real" ] && [ "$link_real" = "$real_blk" ] \
    && note "the link block's realEntry points at the real header" "$link_real == $real_blk" \
    || bad "realEntry points at the real header" "realEntry=${link_real:-none}, header=$real_blk"

[ -n "$link_key" ] && [ "$link_key" = "$link_blk" ] \
    && note "the link block's headerKey is its own block number" "$link_key" \
    || bad "the link block's headerKey is self-referential" "headerKey=${link_key:-none}, block=${link_blk:-none}"

[ "$(printf '%s\n' "$p" | awk '/^chainlen /{print $2}')" = 1 ] \
    && note "exactly one link block on the chain" "1" \
    || bad "exactly one link block on the chain" "$(printf '%s\n' "$p" | awk '/^chainlen /{print $2}')"

printf '%s\n' "$p" | grep -q '^LINKKEYBAD' \
    && bad "no broken self-pointer in the link chain" "LINKKEYBAD" \
    || note "no broken self-pointer in the link chain" "ok"

st=$(printf '%s\n' "$p" | awk '/^NAMELINK /{for(i=1;i<=NF;i++) if($i ~ /^secType=/){sub("secType=","",$i);print $i; exit}}')
[ "$st" = "-4" ] && note "the linked name's secType is -4 (link to file)" "$st" \
    || bad "the linked name's secType is -4" "secType=${st:-none}"

# The linked name must carry the NEW name, not the original's.
case "$link_name" in
    secret.bak*) note "the link block carries the new name" "$link_name" ;;
    *) bad "the link block carries the new name" "got '${link_name:-none}'" ;;
esac

orph1=$("$ROOT/build/afs" verify "$W/t.adf" --orphans 2>&1 | grep -c "leaked")
[ "$orph1" = "$orph0" ] && note "creating the link leaks no blocks" "$orph1 orphans, unchanged" \
    || bad "creating the link leaks no blocks" "$orph0 -> $orph1"

# --- 2. remove the link, on disk ------------------------------------------
if ! "$ROOT/build/afs" rm "$W/t.adf" /secret.bak >"$W/rm.out" 2>&1; then
    bad "afs rm /secret.bak" "$(head -1 "$W/rm.out")"
    echo "M48 gate: FAIL (rm)"; exit 1
fi
p2=$("$W/probe" "$W/t.adf" secret.S secret.bak 2>&1)

printf '%s\n' "$p2" | grep -q '^MISSING secret.bak' \
    && note "the link block is gone after unlink" "name no longer present" \
    || bad "the link block is gone after unlink" "$(printf '%s' "$p2" | grep -E 'NAMELINK|MISSING' | head -1)"

[ "$(printf '%s\n' "$p2" | awk '/^chainlen /{print $2}')" = 0 ] \
    && note "the link chain is empty again" "0" \
    || bad "the link chain is empty again" "$(printf '%s\n' "$p2" | awk '/^chainlen /{print $2}')"

printf '%s\n' "$p2" | grep -q '^HDR 1193\|^HDR ' \
    && note "the real file's header survived the unlink" \
          "$(printf '%s\n' "$p2" | awk '/^HDR /{print $2; exit}')" \
    || bad "the real file's header survived the unlink" "no HDR line"

orph2=$("$ROOT/build/afs" verify "$W/t.adf" --orphans 2>&1 | grep -c "leaked")
[ "$orph2" = "$orph0" ] && note "removing the link leaks no blocks" "$orph2 orphans, unchanged" \
    || bad "removing the link leaks no blocks" "$orph0 -> $orph2"

# --- 3. the oracle that CAN still see this volume must still agree ---------
size_after=$(cd "$W" && rm -rf ex2 && unadf -d ex2 t.adf >/dev/null 2>&1 \
             && stat -c%s ex2/secret.S 2>/dev/null)
n_after=$(cd "$W/ex2" 2>/dev/null && ls 2>/dev/null | wc -l)

[ -n "$size_after" ] && [ "$size_after" = "$size_before" ] \
    && note "ADFlib still reads the linked file, byte-identical" "$size_after bytes" \
    || bad "ADFlib still reads the linked file" "before=${size_before:-none} after=${size_after:-none}"

[ "$n_after" = "$n_before" ] \
    && note "ADFlib's entry count is unchanged by our link churn" "$n_after" \
    || bad "ADFlib's entry count is unchanged" "$n_before -> $n_after"

echo
if [ "$fails" -eq 0 ]; then
    echo "M48 gate: PASS (link and unlink verified on disk, oracle unaffected)"
    exit 0
fi
echo "M48 gate: FAIL ($fails)"
exit 1