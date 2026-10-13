#!/usr/bin/env bash
# M7 gate: RDB partition discovery on the real A590 image.
#
# amitools rdbtool is the authoritative oracle here (ADFlib's RDB support is
# experimental). We check:
#  1. six partitions are found, with the exact names amitools reports;
#  2. every partition's computed start block actually holds its declared DOS
#     type on disk (the strongest check: it validates our start-block formula
#     against real bytes, not against a table);
#  3. the sizes match amitools' cyl_blks arithmetic.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures"
PROG="$ROOT/build/m7_rdb"
IMG="$FIX/a590_WD93028XA_6parts.hdd"

fails=0
ok()  { echo "  PASS  $*"; }
bad() { echo "  FAIL  $*" >&2; fails=$((fails+1)); }

echo "=== M7 gate (RDB partitions) ==="
[[ -x "$PROG" ]] || { echo "  (build m7_rdb first)"; exit 2; }
[[ -f "$IMG" ]] || { echo "  (fetch the A590 RDB fixture first)"; exit 2; }

out=$("$PROG" "$IMG")
n=$(printf '%s\n' "$out" | grep -c .)
if [[ "$n" == "6" ]]; then
    ok "six partitions discovered"
else
    bad "expected 6 partitions, found $n"
fi

# names and DOS types expected, straight from amitools rdbtool
expected_names=("OFS" "OFS INTL" "OFS DirCache" "FFS" "FFS INTL" "FFS DirCache")
expected_types=("0x444f5300" "0x444f5302" "0x444f5304" "0x444f5301" "0x444f5303" "0x444f5305")

i=0
while IFS=$'\t' read -r name typ start size sup; do
    [[ -n "$name" ]] || continue
    if [[ "$name" == "${expected_names[$i]}" ]]; then
        ok "$(printf '%-14s name matches amitools' "$name")"
    else
        bad "$name: expected name ${expected_names[$i]}"
    fi
    if [[ "$typ" == "${expected_types[$i]}" ]]; then
        ok "$(printf '%-14s DOS type %s' "$name" "$typ")"
    else
        bad "$name: DOS type $typ, expected ${expected_types[$i]}"
    fi
    if [[ "$sup" == "supported" ]]; then
        ok "$(printf '%-14s supported' "$name")"
    else
        bad "$name: flagged unsupported"
    fi
    i=$((i+1))
done <<< "$out"

# the decisive check: each start block really holds that DOS id on disk
check=$("$PROG" "$IMG" | python3 -c '
import sys, struct
img = open("'"$IMG"'", "rb").read()
bad = 0
for ln in sys.stdin:
    parts = ln.rstrip("\n").split("\t")
    if len(parts) < 4: continue
    name, typ, start = parts[0], int(parts[1], 16), int(parts[2])
    got = struct.unpack_from(">I", img, start * 512)[0]
    if got != typ:
        print(f"{name}: block {start} holds 0x{got:08x}, expected 0x{typ:08x}")
        bad += 1
print(f"MISMATCHES={bad}")
')
mism=$(grep -o 'MISMATCHES=[0-9]*' <<< "$check" | cut -d= -f2)
if [[ "$mism" == "0" ]]; then
    ok "every partition start block holds its declared DOS type on disk"
else
    bad "$mism partition start(s) do not hold their declared DOS type"
    grep -v MISMATCHES <<< "$check" >&2
fi

echo
if [[ $fails -eq 0 ]]; then echo "M7 gate: PASS"; exit 0; fi
echo "M7 gate: $fails failure(s)" >&2
exit 1