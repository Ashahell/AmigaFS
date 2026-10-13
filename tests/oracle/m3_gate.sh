#!/usr/bin/env bash
# M3 gate: our directory listing must match ADFlib's adfls on every real
# fixture. This is the oracle diff that decides whether the hash-table walk is
# correct, including the same-hash collision chains and the accented name.
#
# Depth is limited to 1 so the comparison is root-only and deterministic;
# subdirectory recursion is exercised in M4/M9.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures"
OURS="$ROOT/build/m3_ls"
ADFLIB_BIN="${ADFLIB_BIN:-$HOME/Work/adflib-oracle/ADFlib/build/examples}"
export PATH="$ADFLIB_BIN:$PATH"

fails=0
ok()  { echo "  PASS  $*"; }
bad() { echo "  FAIL  $*" >&2; fails=$((fails+1)); }

echo "=== M3 gate (directory listing vs adfls) ==="
[[ -x "$OURS" ]] || { echo "  (build m3_ls first)"; exit 2; }
command -v adfls >/dev/null || { echo "  (adfls not found in $ADFLIB_BIN)"; exit 2; }

for f in "$FIX"/*.adf; do
    name=$(basename "$f")
    # negative fixture: we refuse it, so skip the oracle comparison
    if [[ "$name" == "cache_crash.adf" ]]; then continue; fi

    "$OURS" "$f" 1 2>/dev/null | sort > /tmp/m3_ours.$$
    # adfls prints bare names at the root, one per line
    adfls "$f" 2>/dev/null | sed 's#^#/#' | sort > /tmp/m3_adflib.$$

    if diff -q /tmp/m3_ours.$$ /tmp/m3_adflib.$$ >/dev/null 2>&1; then
        n=$(wc -l < /tmp/m3_ours.$$ | tr -d ' ')
        ok "$(printf '%-22s %2s entries match adfls' "$name" "$n")"
    else
        bad "$name: listing differs from adfls"
        diff /tmp/m3_ours.$$ /tmp/m3_adflib.$$ | head -12 >&2
    fi
    rm -f /tmp/m3_ours.$$ /tmp/m3_adflib.$$
done

echo
if [[ $fails -eq 0 ]]; then echo "M3 gate: PASS"; exit 0; fi
echo "M3 gate: $fails failure(s)" >&2
exit 1