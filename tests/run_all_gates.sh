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
# A SKIP IS NOT A PASS. Measured on a clone with the oracles absent: eleven gates
# printed "SKIP: <tool> not on PATH", exited 0, and this harness recorded every one
# of them as PASS -- including `m30_fixtures_valid_to_the_oracle`, whose entire job
# is to cross-check the fixtures against ADFlib, and `m47_oracle_link_coverage`,
# which pins the oracle gap. A summary that says PASS for gates which did not run is
# the "a green run was not testing what it claimed" shape, in the one script every
# reader runs first.
#
# So the gate's OUTPUT decides the verdict class, not just its exit code. The exit
# code cannot, because skipping is implemented as `exit 0` -- which is correct for a
# gate (an absent optional tool is not a failure) and wrong for a summary.
n_pass=0; n_skip=0; n_fail=0; n_part=0
# Two distinct shapes of "did not test everything", and they are not the same thing:
#   - the WHOLE gate skipped (an optional tool absent, so it asserted nothing);
#   - the gate RAN but skipped part of itself -- e.g. m31/m32/m33 print "SKIP oracle
#     checks (adfls not on PATH)" and then still print ALL GREEN and exit 0. Those
#     gates did real work; they just checked it against one oracle instead of two,
#     which is exactly the cross-check the second oracle exists for. Recording that
#     as a plain PASS is how a two-oracle suite degrades into a one-oracle suite
#     without anybody noticing.
# Anchored, because the first version matched the bare word and misfired on a
# HEALTHY run: m42 prints its own summary "ran 24 gate(s), skipped 0", so a gate
# that fully passed and merely *reported on skipping* was classified PARTIAL. That
# is the M46 shape exactly -- a classifier whose result depends on wording -- and it
# would have made this signal something people switch off.
#
# Both real conventions are covered: gates that print "SKIP: ..." or "SKIP ..." at
# the start of a line, and the newer "M<n> gate: SKIP -- ...". A sentence that
# merely mentions skipping is not a skip.
skip_markers='^[[:space:]]*SKIP([[:space:]]|:)|gate: SKIP'
pass_markers='ALL GREEN|gate: PASS|gate: PASSED'
for g in "${GATES[@]}"; do
    echo
    echo "--- gate: $g ---"
    bash "$HERE/oracle/$g.sh" >"$ROOT/build/.gate.$g.out" 2>&1
    rc=$?
    cat "$ROOT/build/.gate.$g.out"
    skipped=$(grep -cEi "$skip_markers" "$ROOT/build/.gate.$g.out")
    passed=$(grep -cEi "$pass_markers" "$ROOT/build/.gate.$g.out")
    if [ $rc -ne 0 ]; then
        RESULTS+=("FAIL $g"); n_fail=$((n_fail + 1))
    elif [ "$skipped" -gt 0 ] && [ "$passed" -eq 0 ]; then
        RESULTS+=("SKIP $g  <- asserted NOTHING"); n_skip=$((n_skip + 1))
    elif [ "$skipped" -gt 0 ]; then
        RESULTS+=("PARTIAL $g  <- ran, but $skipped sub-check(s) skipped")
        n_part=$((n_part + 1))
    else
        RESULTS+=("PASS $g"); n_pass=$((n_pass + 1))
    fi
    rm -f "$ROOT/build/.gate.$g.out"
done

SRC_AFTER=$(src_fingerprint)

# 3. summary
echo
echo "############################################################"
echo "# SUMMARY"
echo "############################################################"
echo "unit tests: $UNIT"
for r in "${RESULTS[@]}"; do echo "  $r"; done
# Counted, never typed -- a hardcoded expectation is a claim that goes stale, which
# is why the gate count is derived too.
echo "ran $n_pass, partial $n_part, skipped $n_skip, failed $n_fail  (of ${#GATES[@]} gates)"

fail=0
[[ "$UNIT" == "PASS" ]] || fail=1
for r in "${RESULTS[@]}"; do [[ "$r" == FAIL* ]] && fail=1; done

# Skipping is legitimate when an optional tool is genuinely absent -- that is the
# documented design, and a missing local ADFlib should not fail the run. But a skip
# means the gate asserted NOTHING, so it is reported on its own line and the verdict
# says so. `AMIGAFS_REQUIRE_NO_SKIP=1` turns any skip into a failure, which is the
# form CI wants: there, a skipped gate means the environment is wrong, not that the
# tool is optional.
if [[ $((n_skip + n_part)) -gt 0 ]]; then
    echo
    echo "NOTE: $n_skip gate(s) asserted NOTHING and $n_part ran only partially."
    echo "      A PARTIAL gate is the quiet one: it did real work but skipped part of"
    echo "      itself -- typically the SECOND oracle -- and still reported green. That"
    echo "      is how a two-oracle suite becomes a one-oracle suite unnoticed."
    echo "      'ALL GREEN' below covers only the $n_pass that fully ran."
    if [[ "${AMIGAFS_REQUIRE_NO_SKIP:-0}" == "1" ]]; then
        echo "      AMIGAFS_REQUIRE_NO_SKIP=1 is set, so this is a FAILURE."
        fail=1
    else
        echo "      Set AMIGAFS_REQUIRE_NO_SKIP=1 to make that a hard failure."
    fi
fi

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
    # counted, never typed: a hardcoded count is a claim that goes stale the next
    # time a gate is added, which is rule 286's shape in a single word.
    echo "source integrity: PASS (tracked tree unchanged across all ${#GATES[@]} gates)"
fi

echo
if [[ $fail -eq 0 ]]; then
    echo "ALL GREEN"
    exit 0
fi
echo "SOMETHING FAILED"
exit 1