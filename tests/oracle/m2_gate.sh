#!/usr/bin/env bash
# M2 gate: volume open + variant detection against the REAL fixtures.
#
# The pass condition is that our reader reports EXACTLY the DOS types measured
# in fixtures.lock, locates the rootblock by the single formula, and REFUSES
# the DOS\6 fixture with a named reason (C1: DOS\6/DOS\7 are AmigaOS 3.2
# long-filename types, out of scope). ADFlib reads cache_crash.adf as DOS\6;
# we deliberately refuse it, so that one is an intentional divergence.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures"
PROG="$ROOT/build/m2_gate"

fails=0
ok()  { echo "  PASS  $*"; }
bad() { echo "  FAIL  $*" >&2; fails=$((fails+1)); }

echo "=== M2 gate (variant detection) ==="
[[ -x "$PROG" ]] || { echo "  (build m2_gate first)"; exit 2; }

# expected dostype : 0 0 6 0 1 5 5 1 2 5 0   (name order must match the calls)
declare -A EXPECT=(
  [arccsh.adf]=0 [blank.adf]=0 [cache_crash.adf]=6 [ffdisk0049.adf]=0
  [links.adf]=1 [testffs.adf]=5 [testhd.adf]=5 [test_link_chains.adf]=1
  [testofs.adf]=2 [win32-names.adf]=5 [g1a30c.adf]=0
)

for name in "${!EXPECT[@]}"; do
    f="$FIX/$name"
    [[ -f "$f" ]] || { bad "$name missing (run fetch_fixtures.sh)"; continue; }
    want="${EXPECT[$name]}"
    out=$("$PROG" "$f" 2>/dev/null)

    if [[ "$want" == "6" ]]; then
        # must be refused, with a reason naming the type
        if grep -q "refused=1" <<< "$out" && grep -q "DOS.6" <<< "$out"; then
            ok "$(printf '%-22s refused as DOS\\6 (out of scope)' "$name")"
        else
            bad "$name: expected refusal as DOS\\6, got: $out"
        fi
        continue
    fi

    got=$(sed -n 's/.*dostype=\([0-9]*\).*/\1/p' <<< "$out")
    if [[ "$got" == "$want" ]]; then
        ok "$(printf '%-22s DOS\\%s' "$name" "$got")"
    else
        bad "$name: DOS type $got, expected $want"
    fi
done

echo
if [[ $fails -eq 0 ]]; then echo "M2 gate: PASS"; exit 0; fi
echo "M2 gate: $fails failure(s)" >&2
exit 1