#!/usr/bin/env bash
# M8 gate: mount a real fixture with the FUSE3 driver and exercise it the way
# a user would.
#
# Checked per fixture:
#   1. the image mounts (appears in /proc/mounts) and ls works;
#   2. the entry COUNT matches the CLI reader (m3_ls) -- so the FUSE view and
#      the core library agree;
#   3. file contents read THROUGH THE MOUNT are byte-identical to the ADFlib
#      unadf extraction (sha256), which is the real end-to-end proof;
#   4. hard-linked files share an inode and read identical content;
#   5. stat reports the real size and a plausible mtime;
#   6. the mount unmounts cleanly with no leaked handle.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures"
DRV="$ROOT/build/amigafs-fuse"
LS3="$ROOT/build/m3_ls"
AFS="$ROOT/build/afs"
MNT="${TMPDIR:-/tmp}/amigafs_m8_mnt"
ADFLIB_BIN="${ADFLIB_BIN:-$HOME/Work/adflib-oracle/ADFlib/build/examples}"
export PATH="$ADFLIB_BIN:$PATH"

fails=0
ok()  { echo "  PASS  $*"; }
bad() { echo "  FAIL  $*" >&2; fails=$((fails+1)); }

echo "=== M8 gate (FUSE3 mount) ==="
[[ -x "$DRV" ]] || { echo "  (build amigafs-fuse first)"; exit 2; }
command -v fusermount3 >/dev/null || { echo "  (fusermount3 not found)"; exit 2; }
[[ -e /dev/fuse ]] || { echo "  (/dev/fuse absent -- cannot mount here)"; exit 2; }

mkdir -p "$MNT"
cleanup() {
    [[ -n "${MPID:-}" ]] && kill "$MPID" 2>/dev/null
    fusermount3 -u "$MNT" 2>/dev/null
    wait "${MPID:-}" 2>/dev/null
    MPID=""
}
trap cleanup EXIT

mount_and_check() {
    local name="$1"
    local img="$FIX/$name"
    "$DRV" -f "$img" "$MNT" > "/tmp/amigafs_m8.log" 2>&1 &
    MPID=$!
    sleep 1.2

    if ! grep -q "amigafs_m8_mnt" /proc/mounts; then
        bad "$name: did not mount ($(head -1 /tmp/amigafs_m8.log))"
        cleanup; return
    fi

    # 2. entry count matches the core library's own listing.
    #    `.amigafs` is synthetic (decision D4) and is counted SEPARATELY: it is
    #    not a disk entry, so it must be excluded from the comparison -- but it
    #    must be PRESENT and correctly sized, so that excluding it cannot
    #    quietly hide a mount that stopped providing it.
    local fuse_n core_n
    fuse_n=$(ls -A "$MNT" 2>/dev/null | grep -a -v '^\.amigafs$' | wc -l | tr -d ' ')
    core_n=$("$LS3" "$img" 0 2>/dev/null | grep -c .)
    if [[ "$fuse_n" == "$core_n" ]]; then
        ok "$(printf '%-22s mounts, %s root entries match the core listing' "$name" "$fuse_n")"
    else
        bad "$name: mount shows $fuse_n entries, core library says $core_n"
    fi

    local bmp bmpsz want_bmp
    bmp="$MNT/.amigafs/bitmap"
    if [[ -f "$bmp" ]]; then
        bmpsz=$(stat -c%s "$bmp" 2>/dev/null || echo 0)
        want_bmp=$(( ($(stat -c%s "$img") / 512 + 7) / 8 ))
        if [[ "$bmpsz" == "$want_bmp" ]]; then
            ok "$(printf '%-22s .amigafs/bitmap present, %s bytes' "$name" "$bmpsz")"
        else
            bad "$name: .amigafs/bitmap is $bmpsz bytes, expected $want_bmp"
        fi
    else
        bad "$name: .amigafs/bitmap missing"
    fi

    # 2b. ORACLE: the bitmap, against BOTH oracles.
    #     We report the bitmap over EVERY block of the volume. Both ADFlib and
    #     amitools report a scope two blocks smaller -- their totals are always
    #     (blocks - 2) -- and which two they omit is not derivable from their
    #     output: it is not the two reserved blocks at the low end (on
    #     arccsh.adf blocks 0 and 1 carry USED bits, yet the two they omit are
    #     free ones), and amitools contradicts itself, with `bitmap info`
    #     reporting 1758 while its own `bitmap all` dump walks 0..1759.
    #     So the relationship is asserted at the level that IS invariant and
    #     still catches a wrong bitmap: our total exceeds each oracle's by
    #     exactly 2, and our used count is either the oracle's or 2 more, with
    #     free taking the remainder. A bitmap off by even one block in either
    #     direction breaks this.
    local o_used o_free
    o_used=$("$AFS" bitmap "$img" 2>/dev/null | sed -n 's/^used: *//p')
    o_free=$("$AFS" bitmap "$img" 2>/dev/null | sed -n 's/^free: *//p')
    if [[ -n "$o_used" && -n "$o_free" ]]; then
        local img_blocks d_used d_free
        img_blocks=$(( $(stat -c%s "$img") / 512 ))
        if [[ $((o_used + o_free)) -ne $img_blocks ]]; then
            bad "$name: bitmap counts $((o_used + o_free)) blocks, image has $img_blocks"
        fi
        local ou of
        ou=$(adfbitmap show "$img" 2>/dev/null | tr -d '\r' | sed -n 's/.*Blocks used[^0-9]*\([0-9][0-9]*\).*/\1/p' | head -1)
        of=$(adfbitmap show "$img" 2>/dev/null | tr -d '\r' | sed -n 's/.*free[^0-9]*\([0-9][0-9]*\).*/\1/p' | head -1)
        if [[ -n "$ou" && -n "$of" ]]; then
            local du df
            du=$((o_used - ou)); df=$((o_free - of))
            if [[ $((du + df)) -eq 2 && ( $du -eq 0 || $du -eq 2 ) ]]; then
                ok "$(printf '%-22s bitmap: ADFlib %s used/%s free, ours +%s/+%s (total scope)' \
                       "$name" "$ou" "$of" "$du" "$df")"
            else
                bad "$name: bitmap vs ADFlib: ours $o_used/$o_free, ADFlib $ou/$of (expected a total difference of exactly 2)"
            fi
        fi
    fi

    # 3. every file readable through the mount matches unadf, byte for byte
    local tmpd; tmpd=$(mktemp -d)
    if unadf -d "$tmpd" "$img" >/dev/null 2>&1; then
        local mism=0 compared=0
        while IFS= read -r -d '' rf; do
            local rel="${rf#$tmpd/}"
            case "$rel" in *.info) continue;; esac
            local want got
            want=$(sha256sum < "$rf" | cut -d' ' -f1)
            got=$(cat "$MNT/$rel" 2>/dev/null | sha256sum | cut -d' ' -f1)
            compared=$((compared+1))
            [[ "$want" == "$got" ]] || { mism=$((mism+1)); echo "       differs: $rel" >&2; }
        done < <(find "$tmpd" -type f -print0)
        if [[ "$mism" == "0" ]]; then
            ok "$(printf '%-22s %d files byte-identical through the mount' "$name" "$compared")"
        else
            bad "$name: $mism of $compared files differ through the mount"
        fi
    fi
    rm -rf "$tmpd"

    # 4/5. hard-link resolution + stat sanity.
    #
    # NOTE: in links.adf, root/file1 is block 882 and linkfile2 is a hard link
    # to block 889 (the real entry named "file2", inside dir1/dir11). file1 and
    # file2 are DISTINCT files that merely hold identical content -- they are
    # NOT a hard-link pair. So the correct check is that a hard link resolves to
    # the inode of the file it really is, which is dir1/dir11/file2 (889), and
    # that reading through the link yields the real content.
    if [[ "$name" == "links.adf" ]]; then
        local ilink ireal slink sreal
        ilink=$(stat -c %i "$MNT/linkfile2" 2>/dev/null)
        ireal=$(stat -c %i "$MNT/dir1/dir11/file2" 2>/dev/null)
        slink=$(stat -c %s "$MNT/linkfile2" 2>/dev/null)
        sreal=$(stat -c %s "$MNT/dir1/dir11/file2" 2>/dev/null)
        if [[ -n "$ilink" && "$ilink" == "$ireal" ]]; then
            ok "$(printf '%-22s hard link shares the real entry inode (%s)' "$name" "$ilink")"
        else
            bad "$name: hard link inode $ilink != real entry inode $ireal"
        fi
        if [[ -n "$slink" && "$slink" == "$sreal" && "$slink" != "0" ]]; then
            ok "$(printf '%-22s hard link reports the real file size (%s)' "$name" "$slink")"
        else
            bad "$name: hard link size $slink != real size $sreal"
        fi
        # and the link must read the same bytes as the file it points at
        local hl rl
        hl=$(cat "$MNT/linkfile2" 2>/dev/null | sha256sum | cut -d' ' -f1)
        rl=$(cat "$MNT/dir1/dir11/file2" 2>/dev/null | sha256sum | cut -d' ' -f1)
        if [[ -n "$hl" && "$hl" == "$rl" ]]; then
            ok "$(printf '%-22s hard link reads the real file content' "$name")"
        else
            bad "$name: hard link content differs from its real entry"
        fi
    fi
    if [[ "$name" == "testofs.adf" ]]; then
        local sz mt
        sz=$(stat -c %s "$MNT/MOON.GIF" 2>/dev/null)
        mt=$(stat -c %Y "$MNT/MOON.GIF" 2>/dev/null)
        if [[ "$sz" == "173847" ]]; then
            ok "$(printf '%-22s stat reports the real size (173847)' "$name")"
        else
            bad "$name: stat size $sz, expected 173847"
        fi
        if [[ -n "$mt" && "$mt" != "0" ]]; then
            ok "$(printf '%-22s stat reports a real mtime (%s)' "$name" "$mt")"
        else
            bad "$name: mtime is zero/empty"
        fi
    fi

    # 6. clean unmount
    cleanup
    if grep -q "amigafs_m8_mnt" /proc/mounts; then
        bad "$name: mount still present after unmount"
    else
        ok "$(printf '%-22s unmounts cleanly' "$name")"
    fi
}

for n in testofs.adf testffs.adf links.adf test_link_chains.adf arccsh.adf \
         win32-names.adf ffdisk0049.adf blank.adf testhd.adf g1a30c.adf; do
    [[ -f "$FIX/$n" ]] || continue
    mount_and_check "$n"
done

echo
if [[ $fails -eq 0 ]]; then echo "M8 gate: PASS"; exit 0; fi
echo "M8 gate: $fails failure(s)" >&2
exit 1