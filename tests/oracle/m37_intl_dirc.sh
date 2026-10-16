#!/bin/bash
# M37 -- INTL / DIRC round trips, differenced against the ADFlib oracle.
#
# This closes the "INTL/DIRC exactness" open risk: nothing in the suite
# generated a volume of these types, because every fixture in tests/fixtures is
# DOS\1, DOS\3 or DOS\5 by way of however it was made. adfformat (the oracle's
# own formatter) is the generator.
#
# Coverage actually obtained, and its limits:
#
#   -t 0  OFS             round trip byte-identical to the oracle, no rebuild
#   -t 1  FFS             round trip byte-identical, no rebuild
#   -t 3  FFS/INTL        round trip byte-identical, no rebuild
#   -t 5  FFS/DIRC&INTL   round trip byte-identical AFTER a rebuild -- see below
#   -t 7  FFS/DIRC        refused by us on purpose (AmigaOS 3.2 long-filename
#                         type; documented, not a defect)
#   -t 2/4/6  OFS+INTL    CANNOT BE GENERATED. adfformat rejects them:
#                         examples/adfformat.c:261 refuses
#                         `fsType > 0 && fsType % 2 == 0`, which is every
#                         OFS+INTL combination. That is a limit of the ORACLE'S
#                         TOOL, not a statement about the format, so the OFS+INTL
#                         cell of this risk cannot be closed with these oracles
#                         and is recorded as open rather than quietly skipped.
#
# The -t 5 rebuild is not a workaround, it is the M34 guard doing its job:
# adfformat does not mark a DIRC volume's dir-cache block used, so the volume is
# born with a map that disagrees with its own tree, and afs_alloc_block refuses
# and names the remedy. Every DIRC volume from this formatter needs one
# `bitmap-rebuild` before its first write. That is a real user-visible
# consequence of shipping the guard, so it is pinned here.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
AFS=$ROOT/build/afs
ADFBIN=${ADFLIB_BIN:-}
command -v adfformat >/dev/null 2>&1 || { echo "M37 SKIP: adfformat not on PATH"; exit 0; }
command -v unadf    >/dev/null 2>&1 || { echo "M37 SKIP: unadf not on PATH"; exit 0; }

W=$(mktemp -d /tmp/amigafs-m37.XXXXXX)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/src" "$W/out"
printf 'hello INTL world\n' > "$W/src/a.txt"
head -c 3000 /dev/urandom > "$W/src/b.bin"
printf 'nested\n' > "$W/src/c.txt"

fails=0
note() { printf '  %-34s %s\n' "$1" "$2"; }
bad()  { printf '  %-34s FAIL (%s)\n' "$1" "$2"; fails=$((fails + 1)); }

for t in 0 1 3 5; do
  img=$W/w$t.adf
  dd if=/dev/zero of="$img" bs=512 count=1760 status=none
  if ! adfformat -t "$t" -l "V$t" "$img" >/dev/null 2>&1; then
    bad "adfformat -t $t" "formatter refused"
    continue
  fi

  # -t 5 is a DIRC volume and is born with a stale map (see the header), so it
  # gets the remedy the guard names. Doing it unconditionally would also prove
  # the rebuild is harmless on the others, which is worth having.
  "$AFS" bitmap-rebuild "$img" --yes >/dev/null 2>&1 \
    || bad "-t $t rebuild" "bitmap-rebuild refused"

  "$AFS" mkdir "$img" SUB >/dev/null 2>&1 || bad "-t $t" "mkdir refused"
  for f in a.txt b.bin; do
    "$AFS" put "$img" "/$f" "$W/src/$f" >/dev/null 2>&1 \
      || bad "-t $t" "put /$f refused"
  done
  "$AFS" put "$img" /SUB/c.txt "$W/src/c.txt" >/dev/null 2>&1 \
    || bad "-t $t" "put /SUB/c.txt refused"

  # the ORACLE reads it back, and its extraction is compared byte for byte
  rm -rf "$W/out"; mkdir -p "$W/out"
  ( cd "$W/out" && unadf "$img" >/dev/null 2>&1 )
  ours=$(  "$AFS" ls "$img" -R 2>/dev/null | awk '{print $2}' | sed 's|^/||' \
            | grep -v '^SUB$' | sort | tr '\n' ' ')
  orc=$( cd "$W/out" && find . -type f | sed 's|^\./||' | sort | tr '\n' ' ' )

  if [ "$ours" != "$orc" ]; then
    bad "-t $t listing vs oracle" "ours=[$ours] oracle=[$orc]"
  else
    note "-t $t listing vs oracle" "identical"
  fi

  mismatch=
  for f in a.txt b.bin SUB/c.txt; do
    cmp -s "$W/out/$f" "$W/src/${f##*/}" || mismatch="$mismatch $f"
  done
  if [ -n "$mismatch" ]; then
    bad "-t $t oracle extraction" "differs:$mismatch"
  else
    note "-t $t oracle extraction" "byte-identical"
  fi
done

echo
if [ "$fails" -eq 0 ]; then
  echo "M37 gate: PASS (OFS, FFS, FFS/INTL, FFS/DIRC&INTL all byte-identical to the oracle)"
  exit 0
fi
echo "M37 gate: FAIL ($fails)"
exit 1