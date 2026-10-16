#!/usr/bin/env bash
# m47_oracle_link_coverage.sh -- pin WHICH ENTRIES HAVE NO EXTERNAL ORACLE.
#
# WHY THIS EXISTS. m25_fuse_link and m32_link_unlink are the only write gates that
# verify nothing against ADFlib. They check st_nlink, inode identity and byte
# equality -- all THROUGH OUR OWN MOUNT. So their green is self-consistent and
# externally unbacked, and a reader of the suite could reasonably take them for
# oracle-verified. This gate makes the gap explicit instead of implicit.
#
# WHY THE GAP IS REAL, MEASURED. On tests/fixtures/testffs.adf, pristine, no writes
# and no involvement from this implementation:
#
#     unadf extracts   10 names
#     afs ls reports   14 names
#     in ours, not in unadf's extraction:  hlink_blue  hlink_dir1  hlink_dir2  slink_dir1
#     in unadf's, not in ours:             (none)
#
# Two pinned oracles, two different failures on the same entries:
#
#   - ADFlib's unadf skips them SILENTLY. That is a materialisation choice, not a
#     corruption signal: it extracts each real file once per link group. Proof it
#     is not reporting damage: adfGetEntryBlock resolves all four names without an
#     error, and the same volume is fine for every other oracle check in the suite.
#   - amitools' xdftool REFUSES the volume outright:
#         'list' FSError: Unsupported Dir Block(8):block=Block:@1222,Sub_Type: fffffffc
#     and 0xfffffffc is -4, ADF_ST_LFILE, a hard link.
#
# This is M41's shape and it is worth being exact about the direction of the
# inference, because the sentence is easy to get backwards:
#
#     "the oracle cannot process these entries" is NOT "these entries are invalid".
#
# Refusing to CREATE a volume is not refusing to READ one (M41). Refusing to
# PROCESS a hard link is not a claim that the hard link is malformed. amitools says
# "Unsupported"; ADFlib declines silently. Neither says "corrupt". So the correct
# reading is that hard and soft links are legitimate AmigaDOS constructs that both
# pinned oracles implement incompletely, and that this reader sees more than they do.
#
# WHAT IS ASSERTED, and what is deliberately NOT.
#
#   Asserted: our listing is a SUPERSET of the oracle's. Nothing ADFlib can see is
#             invisible to us. This is the load-bearing direction -- if we ever LOSE
#             an entry the oracle sees, that is a real defect and this gate fails.
#   Recorded: the exact divergence set, against a committed baseline, so a change in
#             oracle behaviour is noticed rather than absorbed.
#
# NOT asserted: that the divergence is empty. It is not empty, it is four entries,
# and pretending otherwise would mean either deleting working link support or
# weakening the gate to hide it.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BASE="$ROOT/tests/data/oracle_link_coverage.tsv"
FIX="$ROOT/tests/fixtures/testffs.adf"
W=$(mktemp -d /tmp/amigafs-m47.XXXXXX)
trap 'rm -rf "$W"' EXIT

fails=0
bad() { printf '  %-52s FAIL (%s)\n' "$1" "$2"; fails=$((fails + 1)); }
note() { printf '  %-52s %s\n' "$1" "$2"; }

command -v unadf >/dev/null 2>&1 || { echo "M47 gate: SKIP -- unadf not on PATH"; exit 77; }
[ -x "$ROOT/build/afs" ]      || { echo "M47 gate: SKIP -- build/afs not built"; exit 77; }
[ -f "$FIX" ]                 || { echo "M47 gate: FAIL -- fixture missing: $FIX"; exit 1; }

echo "=== M47 gate (which entries have no external oracle) ==="

# unadf extracts into the CURRENT directory, so it must not run from the repo.
( cd "$W" && unadf -d ex "$FIX" ) >"$W/unadf.log" 2>&1
urc=$?
if [ $urc -ne 0 ]; then
    echo "  unadf failed on the PRISTINE fixture (rc=$urc) -- the volume is not"
    echo "  readable by the oracle at all, so there is nothing to compare."
    head -3 "$W/unadf.log" | sed 's/^/      /'
    echo "M47 gate: FAIL (oracle cannot read the fixture)"
    exit 1
fi
( cd "$W/ex" && ls ) | sort > "$W/oracle.txt"

"$ROOT/build/afs" ls "$FIX" 2>/dev/null | awk '{print $2}' | sed 's|^/||' | sort > "$W/ours.txt"

on=$(wc -l < "$W/oracle.txt" | tr -d ' ')
mn=$(wc -l < "$W/ours.txt" | tr -d ' ')
[ "$on" -gt 0 ] && [ "$mn" -gt 0 ] || { echo "M47 gate: FAIL -- empty listing"; exit 1; }

# The direction that matters: we must not LOSE anything the oracle can see.
lost=$(comm -23 "$W/oracle.txt" "$W/ours.txt")
if [ -n "$lost" ]; then
    bad "our listing is a superset of the oracle's" "$(printf '%s' "$lost" | grep -c .) entr(ies) lost"
    printf '%s\n' "$lost" | sed 's/^/        /'
    echo
    echo "  These are readable by ADFlib and INVISIBLE to us. That is a defect in"
    echo "  this reader, and it is the one thing this gate exists to catch."
    echo
    echo "M47 gate: FAIL (lost entries)"
    exit 1
fi
note "our listing is a superset of the oracle's" "$mn >= $on, nothing lost"

extra=$(comm -13 "$W/oracle.txt" "$W/ours.txt")
ecount=$(printf '%s' "$extra" | grep -c . || true)

# The SECOND oracle, and the finding that turned this from "four entries have no
# oracle" into something larger. Measured on this same fixture: unadf extracts 10
# entries, while xdftool lists NOTHING at all -- not the four links, and not the
# ten plain entries either. One hard link makes the WHOLE VOLUME unreadable to
# amitools:
#
#     'list' FSError: Unsupported Dir Block(8):block=Block:@1222,Sub_Type: fffffffc
#     rc=3, zero entries listed
#
# So the gap is PER-VOLUME, not per-entry. On any volume carrying a hard link,
# ADFlib is the only oracle left -- and ADFlib does not expose links. That is a
# much stronger constraint on the verification strategy than "four entries are
# uncovered", and it is why this is recorded rather than left for a reader to
# infer from two greens.
amitools_state="absent"
if command -v xdftool >/dev/null 2>&1; then
    xo=$(cd "$W" && xdftool "$FIX" list 2>&1); xrc=$?
    if [ $xrc -eq 0 ]; then
        amitools_state="lists $(printf '%s\n' "$xo" | grep -c .) lines"
    else
        sub=$(printf '%s\n' "$xo" | sed -n 's/.*Sub_Type: *\([0-9a-f]*\).*/\1/p' | head -1)
        case "$sub" in
            fffffffc) sub="$sub (-4, ADF_ST_LFILE: a HARD LINK to a file)" ;;
            *)        sub="${sub:-unknown}" ;;
        esac
        amitools_state="REFUSED rc=$xrc, 0 entries listed; refuses on Sub_Type $sub"
    fi
fi

{
    printf 'oracle\tunadf extracts into cwd; amitools xdftool list\n'
    printf 'fixture\ttestffs.adf (pristine, no writes)\n'
    printf 'adflib_unadf_extracts\t%s\n' "$on"
    printf 'we_report\t%s\n' "$mn"
    printf 'amitools_volume\t%s\n' "$amitools_state"
    printf 'no_external_oracle\t%s\n' "$(printf '%s' "$extra" | tr '\n' ' ')"
    printf 'scope\tPER-VOLUME: one hard link makes amitools blind to every entry on the volume, not only the links\n'
} > "$W/table.tsv"

if [ ! -f "$BASE" ]; then
    # An EXPLICIT opt-in, not an automatic first-run convenience. This gate's whole
    # value is that it pins a measured fact; a version that manufactures the fact
    # when the record is missing would report green on a fresh clone having checked
    # nothing, which is the vacuous-green trap in its purest form. Generating it by
    # hand instead -- by re-deriving the numbers in another script -- invites the
    # record and the gate to disagree, which is worse. So the gate can write it,
    # but only when told to, by a person who has read the table it prints.
    if [ "${1:-}" = "--write-baseline" ]; then
        mkdir -p "$(dirname "$BASE")"
        cp "$W/table.tsv" "$BASE"
        echo "  baseline WRITTEN (deliberate): $BASE"
        echo
        echo "  Read it before committing. A baseline written without being read is"
        echo "  just a snapshot of whatever the code happened to do."
        echo
        echo "M47 gate: PASS (baseline written; re-run WITHOUT the flag to check it)"
        exit 0
    fi
    bad "the baseline exists" "missing: $BASE"
    echo "  It must be committed -- this gate pins a measured oracle gap, so the"
    echo "  record has to exist before the check can mean anything."
    echo
    echo "  To create it deliberately, after reading the table this gate prints:"
    echo "      bash tests/oracle/m47_oracle_link_coverage.sh --write-baseline"
    echo
    echo "M47 gate: FAIL (no baseline)"
    exit 1
fi

if diff -u "$BASE" "$W/table.tsv" > "$W/diff.txt"; then
    note "the oracle gap matches the baseline" "$ecount entr(ies) uncovered"
else
    bad "the oracle gap matches the baseline" "changed"
    echo
    grep -E '^[+-][^+-]' "$W/diff.txt" | head -12 | sed 's/^/      /'
    echo
    echo "  Either this reader gained or lost coverage, or an oracle changed. Both"
    echo "  are worth knowing; neither should happen silently."
    echo
    echo "  If intended:   cp $W/table.tsv $BASE"
    echo
fi

echo
echo "  What this means for the suite, stated plainly. On THIS volume:"
echo "    - the $ecount entr(ies) above have no external oracle at all;"
echo "    - amitools sees $amitools_state"
echo "    - so ADFlib is the only oracle that reaches this volume, and ADFlib does"
echo "      not expose links."
echo
echo "  That makes m25_fuse_link and m32_link_unlink's green internally consistent"
echo "  and externally unbacked -- a known, measured limitation, not an oversight."
echo "  This gate is what keeps it from being forgotten, and what will notice if"
echo "  either oracle ever gains link support."
echo
if [ "$fails" -eq 0 ]; then
    echo "M47 gate: PASS ($ecount entr(ies) with no external oracle, pinned)"
    exit 0
fi
echo "M47 gate: FAIL ($fails)"
exit 1