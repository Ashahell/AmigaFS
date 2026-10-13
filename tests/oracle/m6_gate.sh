#!/usr/bin/env bash
# M6 gate: the structure verifier, checked three ways.
#
#   1. AGREEMENT, clean disks -- every fixture with no known defect must verify
#      clean. A verifier that reported nothing anywhere would pass this half,
#      which is why half 3 exists.
#
#   2. AGREEMENT, damaged disks -- for each fixture with a KNOWN defect, the
#      verifier must report that specific defect AND ADFlib must independently
#      report it too. Known defects are never "fixed" by going quiet.
#
#   3. NEGATIVE CONTROL -- m6_damage_probe damages each block the hash table
#      can reach and requires the verifier to name that block. This is the half
#      that catches a verifier which never descends past the rootblock: such a
#      verifier agrees with ADFlib on both damaged disks above (their defects
#      are rootblock- and bitmap-level, both checked at open) while being
#      unable to see ANY damage below the root.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures"
BIN="$ROOT/build"
export PATH="/home/miller/Work/adflib-oracle/ADFlib/build/examples:$PATH"

fail=0
note() { printf '  %-42s %s\n' "$1" "$2"; }
bad()  { printf '  %-42s FAIL\n' "$1"; fail=1; }

# problem lines only -- m6_verify also prints a "<path> N problems" summary
# line, which contains tabs and would otherwise be counted as a finding.
problems() { grep -E '^  ' || true; }

echo "M6 gate: structure verifier"

# ---- 1. clean disks must verify clean ------------------------------------
for fx in testffs.adf testofs.adf arccsh.adf links.adf testhd.adf \
          test_link_chains.adf win32-names.adf dirhash.adf; do
    p="$FIX/$fx"
    [ -f "$p" ] || continue
    if out=$("$BIN/afs" verify "$p" 2>&1); then
        note "$fx" "clean (as expected)"
    else
        bad "$fx"; echo "$out" | sed 's/^/      /'
    fi
done

# ---- 2. damaged disks must match ADFlib ----------------------------------
# g1a30c.adf is NOT clean: its bootblock root field says 879 while the computed
# rootblock is 880. ADFlib reports the same disagreement (verified), so this is
# a true finding and the fixture was simply mis-filed as clean until now.
check_known() {
    fx="$1"; shift
    p="$FIX/$fx"
    [ -f "$p" ] || return
    out=$("$BIN/m6_verify" "$p" 2>&1)
    body=$(printf '%s\n' "$out" | problems)
    n=$(printf '%s\n' "$body" | grep -c . || true)
    want=$#
    ok=1
    [ "$n" -eq "$want" ] || ok=0
    for pat in "$@"; do
        printf '%s\n' "$body" | grep -q "$pat" || ok=0
    done
    if [ $ok -eq 1 ]; then
        note "$fx" "$n problem(s), all expected"
    else
        bad "$fx (got $n, want $want)"; printf '%s\n' "$body" | sed 's/^/      /'
    fi
}
check_known ffdisk0049.adf 'bootblock root field' 'bitmap flagged invalid'
check_known g1a30c.adf   'bootblock root field 879 disagrees'

echo "  -- ADFlib corroboration of each known defect --"
adflog=$(mktemp)
if adfinfo "$FIX/ffdisk0049.adf" > "$adflog" 2>&1; then :; fi
if grep -q 'different than calculated 880' "$adflog"; then
    note "ffdisk0049.adf" "ADFlib: rootblock disagreement confirmed"
else
    bad "ffdisk0049.adf ADFlib corroboration"
fi
if adfinfo "$FIX/g1a30c.adf" > "$adflog" 2>&1; then :; fi
if grep -q 'rootBlock sector set in bootblock 879, different than calculated 880' "$adflog"; then
    note "g1a30c.adf" "ADFlib: same 879-vs-880 disagreement confirmed"
else
    bad "g1a30c.adf ADFlib corroboration"
fi
rm -f "$adflog"

# ---- 3. negative control: injected damage must be detected ---------------
if [ -x "$BIN/m6_damage_probe" ]; then
    probe_fx=""
    for fx in testffs.adf arccsh.adf links.adf testofs.adf; do
        [ -f "$FIX/$fx" ] && probe_fx="$probe_fx $FIX/$fx"
    done
    # shellcheck disable=SC2086
    if out=$("$BIN/m6_damage_probe" $probe_fx 2>&1); then
        printf '%s\n' "$out" | grep -E 'reachable blocks|OK' | sed 's/^/  /'
    else
        printf '%s\n' "$out" | sed 's/^/  /'
        fail=1
    fi
else
    bad "m6_damage_probe binary missing"
fi

# ---- 4. the verifier must refuse the DOS\6 disk BY NAME ------------------
if [ -f "$FIX/cache_crash.adf" ]; then
    out=$("$BIN/afs" verify "$FIX/cache_crash.adf" 2>&1); rc=$?
    if [ $rc -ne 0 ] && printf '%s\n' "$out" | grep -q 'DOS.6'; then
        note "cache_crash.adf" "refused by name (DOS\\6), rc=$rc"
    else
        bad "cache_crash.adf"; echo "$out" | sed 's/^/      /'
    fi
fi

echo
if [ $fail -eq 0 ]; then
    echo "M6 gate: PASS"
    exit 0
fi
echo "M6 gate: FAIL"
exit 1
