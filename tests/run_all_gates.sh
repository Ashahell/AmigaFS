#!/usr/bin/env bash
# End-to-end harness: run every milestone gate in sequence and summarise.
#
# The unit tests cover the format logic; the gates compare that logic against
# two independent oracles (ADFlib CLI, amitools) and, for M8, against a REAL
# FUSE mount. This script is the single "is the project healthy" entry point
# and is what `make test` / ctest runs.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

echo "############################################################"
echo "# amigafs end-to-end harness"
echo "############################################################"

# 1. unit tests ONLY (exclude the gate_* and all_gates entries, which this
#    script runs itself -- running full ctest here would recurse into all_gates)
echo
echo "--- unit tests (ctest) ---"
if ( cd "$ROOT/build" && ctest -R '^test_' --output-on-failure ); then
    UNIT="PASS"
else
    UNIT="FAIL"
fi

# 2. oracle + integration gates
#
# The source-integrity fingerprint below is taken BEFORE any gate runs and
# compared after ALL of them. It exists because of what happened on 2026-10-16:
# this harness reported ALL GREEN, and the very next command found its own
# working directory GONE -- every tracked source file, plus the worktree's .git,
# with only build/ left behind.
#
# m42_no_repo_writes.sh is the gate built to catch a gate damaging the repository,
# and it did NOT catch this, for a structural reason rather than a logic one: it is
# gate #14 of 38, so its observation window covers only the first third of the run.
# A guard that watches a window cannot see damage done after the window closes.
# So the watch moves up to the harness, which spans everything.
#
# What it compares is the COUNT OF TRACKED FILES plus the source tree's digest --
# cheap, and it distinguishes "a gate removed something" from "nothing happened".
# Deletions are the shape that motivated it; the digest catches modifications too.
src_fingerprint() {
    ( cd "$ROOT" && git ls-files | wc -l; \
      cd "$ROOT" && git ls-files -z | xargs -0 sha256sum 2>/dev/null | sha256sum ) | tr '\n' ' '
}
SRC_BEFORE=$(src_fingerprint)

declare -a GATES=(m23_hash_unreachable m24_unadf_link_blindness m25_link_write m25_fuse_link m26_root_dircache_survives m28_strict_sees_bitmap_mismatch m29_no_raw_page_bitmap_writes m30_fixtures_valid_to_the_oracle m31_symlink_write m32_link_unlink m33_orphan_and_enospc m37_intl_dirc m39_blockio_fuzz m42_no_repo_writes m43_amiga_ids m46_errno_table m47_oracle_link_coverage m48_link_structure m49_softlink_structure m50_nondos_refusal m18_bitmap_rebuild m0_gate m2_gate m3_gate m4_gate m5_gate m6_gate m7_gate m8_gate m10_gate m10_mount_gate m11_gate m12_cli_rdb m13_write m14_delete m15_fuse_write m16_write_fuzz fuzz_gate mount_fuzz)
declare -a RESULTS=()
for g in "${GATES[@]}"; do
    echo
    echo "--- gate: $g ---"
    if bash "$HERE/oracle/$g.sh"; then
        RESULTS+=("PASS $g")
    else
        RESULTS+=("FAIL $g")
    fi
done

SRC_AFTER=$(src_fingerprint)

# 3. summary
echo
echo "############################################################"
echo "# SUMMARY"
echo "############################################################"
echo "unit tests: $UNIT"
for r in "${RESULTS[@]}"; do echo "  $r"; done

fail=0
[[ "$UNIT" == "PASS" ]] || fail=1
for r in "${RESULTS[@]}"; do [[ "$r" == FAIL* ]] && fail=1; done

if [[ "$SRC_BEFORE" != "$SRC_AFTER" ]]; then
    echo
    echo "SOURCE INTEGRITY: FAIL -- the tracked source tree changed during the run"
    echo "  before: $SRC_BEFORE"
    echo "  after:  $SRC_AFTER"
    echo
    echo "  Every gate above reported its own verdict, and none of them is the"
    echo "  thing that changed the repository. That is precisely why m42 -- which"
    echo "  watches for exactly this -- is not sufficient on its own: it is one"
    echo "  gate inside the run it is guarding."
    fail=1
else
    echo "source integrity: PASS (tracked tree unchanged across all 38 gates)"
fi

echo
if [[ $fail -eq 0 ]]; then
    echo "ALL GREEN"
    exit 0
fi
echo "SOMETHING FAILED"
exit 1