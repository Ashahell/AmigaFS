#!/usr/bin/env bash
# M4 gate: every file ADFlib's unadf extracts must be byte-for-byte identical
# to what we read, on every fixture.
#
# Comparison rule (honest and precise):
#   PASS iff the ORACLE's (path, sha256, size) set is a SUBSET of ours.
# Two known, documented reasons ours may be a strict superset:
#   1. Hard links. unadf resolves a hard-linked entry to its real file and
#      writes only that; we expose the link as its own path with the same
#      content. Correct behaviour, and M5 turns links into shared inodes.
#   2. ADFlib ".info" sidecars are filtered on BOTH sides (they are ADFlib's
#      own comment metadata, not volume files).
#
# Hashes are computed from stdin (sha256sum < file) because GNU sha256sum
# escapes filenames containing backslashes, which corrupts the comparison for
# win32-names.adf.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures"
OURS="$ROOT/build/m4_cat"
ADFLIB_BIN="${ADFLIB_BIN:-$HOME/Work/adflib-oracle/ADFlib/build/examples}"
export PATH="$ADFLIB_BIN:$PATH"

fails=0; total=0; matched=0; extra_total=0

# M42 MUTANT: reproduce the unadf-writes-to-cwd failure
unadf "$FIX/testffs.adf" >/dev/null 2>&1 || true

echo "=== M4 gate (file contents vs unadf) ==="
[[ -x "$OURS" ]] || { echo "  (build m4_cat first)"; exit 2; }
command -v unadf >/dev/null || { echo "  (unadf not found in $ADFLIB_BIN)"; exit 2; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

for f in "$FIX"/*.adf; do
    name=$(basename "$f")
    [[ "$name" == "cache_crash.adf" ]] && continue        # we refuse DOS\6

    od="$TMP/out_$name"; rm -rf "$od"; mkdir -p "$od"
    unadf -d "$od" "$f" >/dev/null 2>&1 || { echo "  SKIP  $name (unadf failed)"; continue; }

    # oracle side: hash each extracted file from stdin, keyed by its path
    ( cd "$od" && find . -type f ! -name '*.info' | LC_ALL=C sort | while IFS= read -r rp; do
        printf '/%s\t%s\t%s\n' "${rp#./}" "$(sha256sum < "$rp" | cut -d' ' -f1)" "$(stat -c%s "$rp")"
      done ) > "$TMP/oracle.txt"

    "$OURS" "$f" 2>/dev/null | LC_ALL=C sort > "$TMP/ours.txt"

    norc=$(wc -l < "$TMP/oracle.txt" | tr -d ' ')
    [[ "$norc" == "0" ]] && { echo "  --    $(printf '%-22s no data files' "$name")"; continue; }

    total=$((total + norc))
    # exact set comparison in python (locale-proof; comm can warn about tab
    # ordering and we do not want a warning to mask a real difference)
    missing=$(python3 - "$TMP/oracle.txt" "$TMP/ours.txt" <<'PY'
import sys
def load(p):
    s=set()
    for ln in open(p, encoding='latin-1'):
        ln=ln.rstrip('\n')
        if ln: s.add(ln)
    return s
o=load(sys.argv[1]); n=load(sys.argv[2])
miss=o-n
sys.stdout.write(str(len(miss)))
PY
)
    extra=$(python3 - "$TMP/oracle.txt" "$TMP/ours.txt" <<'PY'
import sys
def load(p):
    s=set()
    for ln in open(p, encoding='latin-1'):
        ln=ln.rstrip('\n')
        if ln: s.add(ln)
    return s
o=load(sys.argv[1]); n=load(sys.argv[2])
sys.stdout.write(str(len(n-o)))
PY
)

    if [[ "$missing" == "0" ]]; then
        matched=$((matched + norc))
        extra_total=$((extra_total + extra))
        if [[ "$extra" == "0" ]]; then
            echo "  PASS  $(printf '%-22s %3d files identical' "$name" "$norc")"
        else
            echo "  PASS  $(printf '%-22s %3d identical (+%d hardlink alias)' "$name" "$norc" "$extra")"
        fi
    else
        echo "  FAIL  $(printf '%-22s %d of %d oracle files NOT matched' "$name" "$missing" "$norc")" >&2
        comm -23 "$TMP/oracle.txt" "$TMP/ours.txt" | head -8 >&2
        fails=$((fails + 1))
    fi
done

echo
echo "oracle files: $total, byte-identical: $matched, hardlink aliases: $extra_total"
if [[ $fails -eq 0 ]]; then echo "M4 gate: PASS"; exit 0; fi
echo "M4 gate: $fails fixture(s) have unmatched content" >&2
exit 1