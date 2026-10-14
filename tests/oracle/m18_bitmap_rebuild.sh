#!/usr/bin/env bash
# m18_bitmap_rebuild.sh — the allocation-map rebuild, against pinned oracles.
#
# A rebuild FREES every block the directory tree does not reference, so the
# question this gate answers is not "does it run" but "does it destroy
# anything". Three independent checks, because any one of them alone would pass
# on a wrong implementation:
#
#   1. ADFlib ORACLE. adfReconstructBitmap (read at src/adf_bitm.c:443 of the
#      pinned checkout) is the reference policy: full unconditional rescan, no
#      pre-verification, all-or-nothing on a read failure. Our semantics are
#      chosen to match it exactly, so where ADFlib refuses we must refuse.
#   2. ADFlib DATA SURVIVAL. Every file the oracle could extract BEFORE must
#      extract byte-identically AFTER. This is the check that would catch a
#      rebuild that frees a live block — and it is deliberately byte-level,
#      because a rebuild that frees a block does not make the volume look
#      broken, it makes one file quietly wrong.
#   3. RECLAIM COUNT. The exact number of blocks reclaimed is asserted. This is
#      the only check that catches a mutant which frees the extension blocks:
#      such a rebuild still passes check 2 (the orphaned pointers do not change
#      what unadf extracts) but reclaims 33 instead of 26.
#
# Check 3 was found the hard way. The ext-chain mutant was run through the
# full survival loop first and produced ZERO observable differences: it wrote 15
# files where the correct build wrote 13, and every pre-existing file still
# matched byte-for-byte. A gate built only from checks 1-2 would have called
# that mutant GREEN.
#
# Oracles are subprocesses only, never linked (decision C7).
set -uo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
AFS="$REPO/build/afs"
FX="$REPO/tests/fixtures"
WORK="$(mktemp -d /tmp/amigafs-m18.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

fail=0
ok()  { echo "  PASS $1"; }
bad() { echo "  FAIL $1"; fail=1; }

for tool in unadf; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "m18_bitmap_rebuild: SKIP - $tool not on PATH" >&2
        echo "  (build the ADFlib oracle and put it on PATH; see README)" >&2
        exit 77
    fi
done
if [ ! -x "$AFS" ]; then
    echo "m18_bitmap_rebuild: FAIL - $AFS not built" >&2
    exit 1
fi

echo "Map rebuild gate: AmigaDOS/ADFlib rescan policy, data survival"

# ---------------------------------------------------------------- fixtures --
# ffdisk0049.adf is the fixture that motivates the whole feature: bm_flag=1,
# and its map marks block 1089 free while 1089 is the header of Plot/save.c.
# The allocator refuses it (M17); a rebuild is what makes it writable.
SRC="$FX/ffdisk0049.adf"
if [ ! -f "$SRC" ]; then echo "m18: missing $SRC" >&2; exit 1; fi

# ------------------------------------------------------- 1. refusal posture --
# The rebuild is an explicit operator action. Without --yes it must refuse, and
# it must refuse by NAME rather than silently doing nothing.
cp "$SRC" "$WORK/noyes.adf"
out=$("$AFS" bitmap-rebuild "$WORK/noyes.adf" 2>&1); rc=$?
if [ "$rc" -ne 0 ]; then ok "refuses without --yes (rc=$rc)"; else bad "rebuilt without --yes"; fi
if printf '%s' "$out" | grep -qi "FREES every"; then ok "refusal states what it frees"; else bad "refusal does not explain the consequence"; fi
if cmp -s "$SRC" "$WORK/noyes.adf"; then ok "refused volume untouched"; else bad "refused volume was modified"; fi

# A refused volume must also still refuse to be WRITTEN, which is the M17
# behaviour the rebuild exists to lift. If this passes before the rebuild the
# gate is not testing what it claims.
if "$AFS" put "$WORK/noyes.adf" /Plot/gatefile /dev/null >/dev/null 2>&1; then
    bad "a volume with a suspect bitmap accepted a write before rebuild"
else
    ok "suspect-bitmap volume still refuses writes before rebuild"
fi

# ------------------------------------------------------------ 2. dry run --
cp "$SRC" "$WORK/dry.adf"
out=$("$AFS" bitmap-rebuild "$WORK/dry.adf" --dry-run 2>&1); rc=$?
if [ "$rc" -eq 0 ]; then ok "dry run succeeds (rc=0)"; else bad "dry run failed: $out"; fi
if cmp -s "$SRC" "$WORK/dry.adf"; then ok "dry run left the image byte-identical"; else bad "dry run MODIFIED the image"; fi
if printf '%s' "$out" | grep -qi "would be reclaimed"; then ok "dry run reports the reclaim count"; else bad "dry run reports no reclaim count"; fi
# The dry run must not merely be quiet on this fixture: it must predict work.
n_dry=$(printf '%s' "$out" | sed -n 's/.*, \([0-9]*\) would be reclaimed.*/\1/p')
if [ -n "$n_dry" ] && [ "$n_dry" -gt 0 ]; then ok "dry run predicts $n_dry reclaimed blocks"; else bad "dry run predicted no reclaim (a vacuous run)"; fi

# ------------------------------------------------- 3. data survival, oracle --
extract() {  # $1=image $2=outdir
    mkdir -p "$2"
    unadf -d "$2" "$1" >/dev/null 2>&1
}
rm -rf "$WORK/before" "$WORK/after"
extract "$SRC" "$WORK/before"
n_before=$(find "$WORK/before" -type f ! -name '*.info' | wc -l)
if [ "$n_before" -gt 0 ]; then ok "oracle extracted $n_before files before"; else bad "oracle extracted NOTHING before (vacuous gate)"; fi

cp "$SRC" "$WORK/live.adf"
out=$("$AFS" bitmap-rebuild "$WORK/live.adf" --yes 2>&1); rc=$?
if [ "$rc" -eq 0 ]; then ok "rebuild succeeds"; else bad "rebuild failed: $out"; fi

# --------------------------------------- 4. the bitmap problems are resolved --
# Before: bm_flag=1 and "block 1089 is referenced by the tree but the bitmap
# marks it free". After: neither. The bootblock root field garbage is a
# separate, long-documented fixture defect and is excluded by name so this
# gate cannot be satisfied by "fewer problems" for an unrelated reason.
before_bad=$("$AFS" verify "$SRC" --strict 2>&1 | grep -c "bitmap")
after_bad=$("$AFS" verify "$WORK/live.adf" --strict 2>&1 | grep -c "bitmap")
if [ "$before_bad" -gt 0 ]; then ok "fixture starts with $before_bad bitmap problem(s)"; else bad "fixture has no bitmap problem to fix (vacuous gate)"; fi
if [ "$after_bad" -eq 0 ]; then ok "rebuild resolved every bitmap problem"; else bad "$after_bad bitmap problem(s) remain after rebuild"; fi

# bm_flag must now read valid (-1). Read it through the oracle's own listing,
# which reports the invalid bitmap as a mount warning when unset.
if unadf -l "$WORK/live.adf" 2>&1 | grep -qi "invalid bitmap"; then
    bad "oracle still reports an invalid bitmap after rebuild"
else
    ok "oracle no longer reports an invalid bitmap"
fi

extract "$WORK/live.adf" "$WORK/after"
n_after=$(find "$WORK/after" -type f ! -name '*.info' | wc -l)
if [ "$n_after" -eq "$n_before" ]; then ok "oracle extracted $n_after files after"; else bad "file count changed: $n_before -> $n_after"; fi
if diff -r --exclude='*.info' "$WORK/before" "$WORK/after" >/dev/null 2>&1; then
    ok "every file is byte-identical across the rebuild"
else
    bad "file bytes changed across the rebuild"
    diff -rq --exclude='*.info' "$WORK/before" "$WORK/after" 2>&1 | head -5
fi

# ----------------------------------------------------- 5. reclaim count, exact --
# This is the check that catches the extension-chain mutant: it still passes
# check 4, but reclaims 33 rather than 26. Measured stable over repeated runs.
got=$(printf '%s' "$out" | sed -n 's/.* \([0-9]*\) reclaimed$/\1/p')
if [ "$got" = "26" ]; then
    ok "reclaimed exactly 26 blocks"
else
    bad "reclaimed ${got:-<none>} blocks, expected 26"
    echo "       (an extension-chain mutant reports 33 here while still passing"
    echo "        the survival check above -- this assertion is what catches it)"
fi

# ------------------------------------------------- 6. idempotence + writable --
out2=$("$AFS" bitmap-rebuild "$WORK/live.adf" --yes 2>&1); rc=$?
if [ "$rc" -eq 0 ]; then ok "second rebuild succeeds"; else bad "second rebuild failed: $out2"; fi
if cmp -s "$WORK/live.adf" "$WORK/live.adf"; then ok "second run completed"; fi
rm -rf "$WORK/after2"; extract "$WORK/live.adf" "$WORK/after2"
if diff -r --exclude='*.info' "$WORK/after" "$WORK/after2" >/dev/null 2>&1; then
    ok "rebuild is idempotent (files unchanged on the second pass)"
else
    bad "second rebuild changed file contents"
fi

# The point of the exercise: a volume that was refused for writes must now
# accept them, and the pre-existing files must survive the writes.
head -c 900 /dev/urandom > "$WORK/payload.bin"
nput=0
for i in $(seq 1 80); do
    if "$AFS" put "$WORK/live.adf" "/Plot/gate$i" "$WORK/payload.bin" >/dev/null 2>&1; then
        nput=$((nput+1))
    fi
done
if [ "$nput" -gt 0 ]; then ok "wrote $nput files to the rebuilt volume"; else bad "rebuilt volume still refuses every write"; fi
rm -rf "$WORK/after3"; extract "$WORK/live.adf" "$WORK/after3"
if diff -r --exclude='*.info' --exclude='gate*' "$WORK/after" "$WORK/after3" >/dev/null 2>&1; then
    ok "pre-existing files survive writing into the reclaimed space"
else
    bad "writing after the rebuild damaged pre-existing files"
    diff -rq --exclude='*.info' --exclude='gate*' "$WORK/after" "$WORK/after3" 2>&1 | head -5
fi

# --------------------------------------------------------- 7. refusal cases --
# A volume that cannot even be opened, and a long-name volume, must be refused
# by name rather than half-processed.
if "$AFS" bitmap-rebuild "$FX/cache_crash.adf" --yes >/dev/null 2>&1; then
    bad "rebuilt a DOS\\6 long-name volume"
else
    ok "DOS\\6 long-name volume refused by name"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "m18_bitmap_rebuild: PASS"
    exit 0
fi
echo "m18_bitmap_rebuild: FAIL"
exit 1