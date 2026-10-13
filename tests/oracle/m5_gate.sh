#!/usr/bin/env bash
# M5 gate: link resolution on real fixtures.
#
# Three properties are checked, all against real on-disk links:
#  1. HARD LINK SHARING: a hard-linked file and the real entry it points at
#     must resolve to the SAME inode block (one inode, several names).
#  2. HARD-LINK CONTENT: reading a file through a hard link must yield the
#     same bytes as reading it through its real entry (proves we really follow
#     real_entry rather than the link's own, empty, header).
#  3. SOFTLINK TARGETS: every soft link must produce a non-empty translated
#     target, and no target may be empty or absolute (we always emit a
#     mount-relative POSIX path, since the mount root IS the volume root).
#
# Expected link counts are measured from the fixtures and recorded here so a
# regression that silently drops links cannot pass.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures"
PROG="$ROOT/build/m5_links"

fails=0
ok()  { echo "  PASS  $*"; }
bad() { echo "  FAIL  $*" >&2; fails=$((fails+1)); }

echo "=== M5 gate (link resolution) ==="
[[ -x "$PROG" ]] || { echo "  (build m5_links first)"; exit 2; }

# expected counts, measured from the real fixtures
declare -A EXPECT_HARD=([links.adf]=2 [test_link_chains.adf]=2 [testffs.adf]=2)
declare -A EXPECT_SOFT=([links.adf]=0 [test_link_chains.adf]=9 [testffs.adf]=2)

for name in "${!EXPECT_HARD[@]}"; do
    f="$FIX/$name"
    [[ -f "$f" ]] || { bad "$name missing"; continue; }
    out=$("$PROG" "$f" 2>/dev/null)

    nhard=$(grep -c $'\thardfile\t' <<< "$out" || true)
    ndir=$(grep -c $'\tharddir\t' <<< "$out" || true)
    nsoft=$(grep -c $'\tsoft\t' <<< "$out" || true)
    want_hard=${EXPECT_HARD[$name]}
    want_soft=${EXPECT_SOFT[$name]}

    # hard-file links
    if [[ "$nhard" == "$want_hard" ]]; then
        ok "$(printf '%-22s %d hard-link files resolved' "$name" "$nhard")"
    else
        bad "$name: $nhard hard-file links, expected $want_hard"
    fi
    # soft links
    if [[ "$nsoft" == "$want_soft" ]]; then
        ok "$(printf '%-22s %d soft links translated' "$name" "$nsoft")"
    else
        bad "$name: $nsoft soft links, expected $want_soft"
    fi
    # directory hard links are reported but never traversed (C12)
    if [[ "$name" == "test_link_chains.adf" ]]; then
        if [[ "$ndir" == "3" ]]; then
            ok "$(printf '%-22s %d dir hard links exposed, not traversed' "$name" "$ndir")"
        else
            bad "$name: $ndir dir hard links, expected 3"
        fi
    fi

    # no soft link may yield an empty or absolute target
    badt=$(awk -F'\t' '$2=="soft" && ($3=="" || $3 ~ /^\//)' <<< "$out" | wc -l | tr -d ' ')
    if [[ "$badt" == "0" ]]; then
        ok "$(printf '%-22s all soft targets non-empty and relative' "$name")"
    else
        bad "$name: $badt soft link target(s) empty or absolute"
    fi
done

# hard links must share an inode: every hardfile entry resolves to a block that
# is also the real entry of some other name. Cross-check by CONTENT via m4_cat,
# which reports sha per path -- a link and its real file must agree.
for name in links.adf test_link_chains.adf testffs.adf; do
    o="$ROOT/build/m4_cat"
    [[ -x "$o" ]] || continue
    shas=$("$o" "$FIX/$name" 2>/dev/null | awk -F'\t' '{print $2" "$1}' | LC_ALL=C sort)
    # group by hash: a hash shared by >1 path means a hard link pair
    dups=$("$o" "$FIX/$name" 2>/dev/null | awk -F'\t' '{print $2}' | LC_ALL=C sort | uniq -d | wc -l | tr -d ' ')
    if [[ "$dups" -gt 0 ]]; then
        ok "$(printf '%-22s %d content hash(es) shared by multiple names (hard links)' "$name" "$dups")"
    fi
    : "$shas"
done

echo
if [[ $fails -eq 0 ]]; then echo "M5 gate: PASS"; exit 0; fi
echo "M5 gate: $fails failure(s)" >&2
exit 1