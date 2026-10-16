#!/bin/bash
# M42 -- no gate may write into the repository.
#
# Rule 280 in the wiki says a rule violated by its own author needs a MECHANICAL
# guard rather than a reminder. This is that guard, for the failure that has
# actually happened here twice:
#
#   1. `unadf` extracts into the CURRENT DIRECTORY. Running the oracle against a
#      fixture from the repo root dropped 67 untracked paths into it -- Amiga
#      files like `Trashcan.info`, `Cycloids/`, and a `win32-names.sh` that is a
#      shell script living INSIDE the Amiga volume and not a repo script.
#      `git clean -fd` removed them; nothing warned.
#   2. libFuzzer writes `crash-<hash>` to the current directory by default. The
#      first mutant run of m39 therefore left a 400-byte artifact in the repo
#      root, and `git add amigafs/` committed it.
#
# Both are silent: no error, no warning, just files where nobody was looking. And
# both were caught by running `git status` afterwards, which is a habit, not a
# control. This turns the habit into a gate.
#
# It runs a sample of the artifact-producing gates and asserts the working tree is
# byte-identical afterwards. A SAMPLE, deliberately: run_all_gates.sh invokes
# this gate, so running all of them here would recurse. The sample is every gate
# that shells out to `unadf`, plus m37, which constructs images and reads them
# back through the oracle.
#
# The precondition matters as much as the check: if the tree is already dirty when
# this starts, the check cannot attribute a change to a gate, and reporting
# "clean" or "dirty" would both be meaningless. So a dirty start FAILS, loudly.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT" || exit 2

SAMPLE=(m4_gate m8_gate m12_cli_rdb m24_unadf_link_blindness
        m26_root_dircache_survives m30_fixtures_valid_to_the_oracle
        m37_intl_dirc)

digest() {
    # one value covering both things that matter: tracked content (including any
    # modification) and the set of untracked paths
    { git ls-files -s | sha256sum | cut -d' ' -f1
      git status --porcelain | sha256sum | cut -d' ' -f1; } | tr '\n' ' '
}

echo "=== M42 gate (no gate writes into the repository) ==="

before=$(digest)
if [ -n "$(git status --porcelain)" ]; then
    echo "  FAIL  the working tree is already dirty; this gate cannot attribute a"
    echo "        change to a gate, so it refuses to run. Commit or stash first."
    git status --porcelain | head -10 | sed 's/^/        /'
    exit 1
fi

ran=0; skipped=0
for g in "${SAMPLE[@]}"; do
    script="tests/oracle/$g.sh"
    if [ ! -f "$script" ]; then
        echo "  SKIP  $g (no such gate)"
        skipped=$((skipped + 1))
        continue
    fi
    if ! bash -n "$script" 2>/dev/null; then
        echo "  FAIL  $g does not parse"
        exit 1
    fi
    # Run from a scratch cwd as well as checking afterwards: a gate that uses a
    # RELATIVE path would then fail loudly instead of quietly writing into the
    # repo. This is the half of the guard that prevents the problem rather than
    # reporting it afterwards.
    scratch=$(mktemp -d /tmp/amigafs-m42.XXXXXX)
    ( cd "$scratch" && timeout 900 bash "$ROOT/$script" ) >/dev/null 2>&1
    rc=$?
    rm -rf "$scratch"
    if [ $rc -ne 0 ]; then
        echo "  note  $g exited $rc when run from a scratch cwd"
        echo "        (its own runner re-runs it correctly; this gate only cares"
        echo "         about the repository, and the check below is the verdict)"
    fi
    ran=$((ran + 1))
done

after=$(digest)
echo "  ran $ran gate(s), skipped $skipped"

if [ "$before" != "$after" ]; then
    echo "  FAIL  a gate modified the repository"
    git status --porcelain | head -20 | sed 's/^/        /'
    echo
    echo "  A gate that writes into the tree is a gate that will eventually be"
    echo "  committed with its debris. Run such tools from a scratch directory,"
    echo "  or redirect their output (unadf extracts into the cwd; libFuzzer wants"
    echo "  -artifact_prefix)."
    exit 1
fi

echo "  PASS  repository byte-identical after $ran gate(s)"
echo
echo "M42 gate: PASS"
exit 0