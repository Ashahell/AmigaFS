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
declare -a GATES=(m0_gate m2_gate m3_gate m4_gate m5_gate m6_gate m7_gate m8_gate m10_gate m10_mount_gate m11_gate m12_cli_rdb m13_write m14_delete fuzz_gate mount_fuzz)
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

echo
if [[ $fail -eq 0 ]]; then
    echo "ALL GREEN"
    exit 0
fi
echo "SOMETHING FAILED"
exit 1