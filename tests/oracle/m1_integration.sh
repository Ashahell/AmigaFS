#!/usr/bin/env bash
# M1 integration check: our be/checksum/blockio stack must agree with the REAL
# bytes of real fixtures. This is the first time our code touches actual Amiga
# disk images, so it is where an endianness or checksum misunderstanding would
# show up as a hard failure rather than a subtle one.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures"
PROG="$ROOT/build/m1_integration"

fails=0
ok()  { echo "  PASS  $*"; }
bad() { echo "  FAIL  $*" >&2; fails=$((fails+1)); }

echo "=== M1 integration ==="
[[ -x "$PROG" ]] || { echo "  (build m1_integration first)"; exit 2; }

# Every floppy fixture is 880 KiB = 1760 blocks of 512. The rootblock sits at
# block 880; the bootblock is block 0. We assert: DOS type read, bootblock
# checksum reported honestly, rootblock validated.
for f in testofs.adf testffs.adf testhd.adf links.adf arccsh.adf g1a30c.adf blank.adf ffdisk0049.adf; do
    [[ -f "$FIX/$f" ]] || { bad "$f missing"; continue; }
    out=$("$PROG" "$FIX/$f" 2>&1)
    rc=$?
    if [[ $rc -ne 0 ]]; then
        bad "$f: integration tool exited $rc: $out"
        continue
    fi
    # the tool prints "dostype=N rootblock=R rootvalid=1|0 bootvalid=1|0"
    dostype=$(sed -n 's/.*dostype=\([0-9]*\).*/\1/p' <<< "$out")
    rootvalid=$(sed -n 's/.*rootvalid=\([01]\).*/\1/p' <<< "$out")
    if [[ -z "$dostype" ]]; then bad "$f: no dostype parsed ($out)"; continue; fi
    if [[ "$rootvalid" != "1" ]]; then
        bad "$f: our rootblock checksum did NOT validate (got rootvalid=$rootvalid)"
    else
        ok "$(printf '%-20s DOS\\%s rootblock valid' "$f" "$dostype")"
    fi
done

echo
if [[ $fails -eq 0 ]]; then echo "M1 integration: PASS"; exit 0; fi
echo "M1 integration: $fails failure(s)" >&2
exit 1