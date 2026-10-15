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

    # Compare ROOT ONLY: adfls lists just the root directory, so depth 0 on
    # our side. (Depth>0 would recurse and legitimately show more.)
    "$OURS" "$f" 0 2>/dev/null | sort > /tmp/m3_ours.$$
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

# --- CLI lookup is case-INsensitive (decision D5) -------------------------
#
# D5 says "exact-match in the mount, case-insensitive only through the CLI".
# The mount half is the kernel's job and is untestable here. The CLI half was
# for years described as living behind an `afs ls -i` flag that never existed:
# the lookup was ALREADY case-insensitive, because afs_dir_lookup compares
# with the same capitalch the hash uses. Rather than add a flag that does
# nothing, this pins the behaviour the decision record actually promised.
#
# If this check is ever removed, `-i` will be "added" as a no-op and the record
# will look right again while nothing is tested.
if [[ -x "$ROOT/build/afs" ]]; then
    CS="$FIX/win32-names.adf"
    if [[ -f "$CS" ]]; then
        "$ROOT/build/afs" ls "$CS" /forbidden-files  > /tmp/m3_ci_exact.$$ 2>&1; rc_exact=$?
        "$ROOT/build/afs" ls "$CS" /FORBIDDEN-FILES  > /tmp/m3_ci_upper.$$ 2>&1; rc_upper=$?
        "$ROOT/build/afs" ls "$CS" /ForBiddEn-FiLeS  > /tmp/m3_ci_mixed.$$ 2>&1; rc_mixed=$?
        if [[ $rc_exact -eq 0 && $rc_upper -eq 0 && $rc_mixed -eq 0 ]] \
           && cmp -s /tmp/m3_ci_exact.$$ /tmp/m3_ci_upper.$$ \
           && cmp -s /tmp/m3_ci_exact.$$ /tmp/m3_ci_mixed.$$; then
            ok "CLI path lookup is case-insensitive (exact/UPPER/MiXeD identical, $(wc -l < /tmp/m3_ci_exact.$$ | tr -d ' ') lines)"
        else
            bad "CLI lookup is not case-insensitive (rc $rc_exact/$rc_upper/$rc_mixed); D5's promise is unmet"
            diff /tmp/m3_ci_exact.$$ /tmp/m3_ci_upper.$$ 2>&1 | head -5 >&2
        fi
        rm -f /tmp/m3_ci_exact.$$ /tmp/m3_ci_upper.$$ /tmp/m3_ci_mixed.$$
    fi
fi

# --- `afs ls` on a FILE must not report success and print nothing ---------
#
# Found by accident while checking the case-insensitivity claim above:
# `afs ls win32-names.adf /forbidden-files/CON` resolved the path, then listed
# a FILE block as though it were a directory. Result: rc 0 and ZERO bytes of
# output.
#
# That is the worst shape a CLI failure can take. An error at least stops
# `afs ls ... && ...`; silence with a success code lets a script proceed as
# though it had received a listing. GNU `ls` prints the file's own name, so
# that is what we do, in the existing `f1 /name` format and with the STORED
# spelling (the entry may differ in case from what was typed).
LSF="$FIX/win32-names.adf"
if [[ -x "$ROOT/build/afs" && -f "$LSF" ]]; then
    got=$("$ROOT/build/afs" ls "$LSF" /forbidden-files/CON 2>/dev/null); rc_file=$?
    if [[ $rc_file -eq 0 && "$got" == "f1 /CON" ]]; then
        ok "ls of a file prints its own entry (rc=0, 'f1 /CON')"
    else
        bad "ls of a file: rc=$rc_file, output='${got}' (expected rc=0, 'f1 /CON')"
    fi

    # Typed in lower case: lookup is case-insensitive, so this must still
    # succeed and must print the STORED spelling, not what was typed. Pins the
    # stored-name behaviour and cross-checks D5 at the same time.
    gotl=$("$ROOT/build/afs" ls "$LSF" /forbidden-files/con 2>/dev/null); rc_lower=$?
    if [[ $rc_lower -eq 0 && "$gotl" == "f1 /CON" ]]; then
        ok "ls of a file prints the STORED spelling, not the typed one"
    else
        bad "ls of a file typed '/con': rc=$rc_lower, output='${gotl}' (expected rc=0, 'f1 /CON')"
    fi

    # Control: the parent DIRECTORY must be unaffected. Without this, a fix
    # that special-cased the wrong path would still pass the line above.
    ndir=$("$ROOT/build/afs" ls "$LSF" /forbidden-files 2>/dev/null | wc -l | tr -d ' ')
    if [[ "$ndir" == "44" ]]; then
        ok "ls of the containing directory still lists 44 entries"
    else
        bad "directory listing changed: ${ndir} entries, expected 44"
    fi
fi

echo
if [[ $fails -eq 0 ]]; then echo "M3 gate: PASS"; exit 0; fi
echo "M3 gate: $fails failure(s)" >&2
exit 1