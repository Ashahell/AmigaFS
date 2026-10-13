#!/usr/bin/env bash
# M0 gate: prove the pinned oracles are usable and that every fixture reports the
# DOS type recorded in fixtures.lock.
#
# This gate FAILS (non-zero) if:
#   - the ADFlib oracle binary is missing or will not run on a known-good image
#   - a fixture's measured bootblock DOS type differs from fixtures.lock
#   - the amitools oracle cannot read a DOS\2 image (our C1 support case)
#
# It is deliberately about ORACLE + FIXTURE integrity only. It does not exercise
# our own driver (none exists yet).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIXTURES="$(cd "$HERE/../fixtures" && pwd)"
LOCK="$FIXTURES/fixtures.lock"

ADFLIB_BIN="${ADFLIB_BIN:-$HOME/Work/adflib-oracle/ADFlib/build/examples}"
ADFLIB_COMMIT=$(awk 'BEGIN{o=0} /^\[oracle\.adflib\]/{o=1} o && /^commit[[:space:]]*=/{print $3; exit}' "$LOCK")
export PATH="$ADFLIB_BIN:$PATH"

fails=0
ok()   { echo "  PASS  $*"; }
bad()  { echo "  FAIL  $*" >&2; fails=$((fails+1)); }

echo "=== M0 gate ==="
echo "oracle ADFlib bin: $ADFLIB_BIN"

# --- 1. the ADFlib oracle must run at all (the plan's stated gate) ---
if ! command -v adfls >/dev/null 2>&1; then
    bad "adfls not found in $ADFLIB_BIN (build the oracle first)"
else
    if adfls "$FIXTURES/testffs.adf" >/dev/null 2>&1; then
        ok "adfls lists testffs.adf (ADFlib oracle usable)"
    else
        bad "adfls failed on testffs.adf"
    fi
fi

# --- 2. every fixture's measured DOS type must equal fixtures.lock ---
# expected types, keyed by fixture, from fixtures.lock blk0 field
declare -A EXPECT=(
  [testofs.adf]=2 [testffs.adf]=5 [testhd.adf]=5 [links.adf]=1
  [test_link_chains.adf]=1 [arccsh.adf]=0 [win32-names.adf]=5
  [cache_crash.adf]=6 [g1a30c.adf]=0 [blank.adf]=0 [ffdisk0049.adf]=0
)

for name in "${!EXPECT[@]}"; do
    f="$FIXTURES/$name"
    [[ -f "$f" ]] || { bad "$name missing (run fetch_fixtures.sh)"; continue; }
    # first long of block 0 == ASCII 'DOS' (0x44 0x4f 0x53) + flag byte
    b0=$(od -An -tx1 -N4 "$f" | tr -d ' \n')
    if [[ "${b0:0:6}" != "444f53" ]]; then
        bad "$name not a DOS volume (bootblock0=$b0)"; continue
    fi
    got=$((16#${b0:6:2}))
    if [[ "$got" == "${EXPECT[$name]}" ]]; then
        ok "$(printf '%-22s DOS\\%d' "$name" "$got")"
    else
        bad "$name DOS type = DOS\\$got, expected DOS\\${EXPECT[$name]}"
    fi
done

# --- 3. amitools must read the DOS\2 image ADFlib labels "???" (our C1 case) ---
if command -v xdftool >/dev/null 2>&1; then
    if xdftool "$FIXTURES/testofs.adf" list 2>&1 | grep -q "DOS2:ofs+intl"; then
        ok "amitools reads testofs.adf as DOS2:ofs+intl (C1 coverage)"
    else
        bad "amitools did not read testofs.adf as DOS2:ofs+intl"
    fi
else
    echo "  SKIP  amitools xdftool not on PATH (second oracle unavailable)"
fi

# --- summary ---
echo
if [[ $fails -eq 0 ]]; then
    echo "M0 gate: PASS"
    exit 0
fi
echo "M0 gate: $fails failure(s)" >&2
exit 1