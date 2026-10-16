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
# this gate, so running all of them here would recurse. The sample is COMPUTED
# from the tree -- every gate mentioning unadf, clang, fsanitize=fuzzer,
# amigafs-fuse or git -- because a hand-list inside a gate goes stale silently.
#
# The precondition matters as much as the check: if the tree is already dirty when
# this starts, the check cannot attribute a change to a gate, and reporting
# "clean" or "dirty" would both be meaningless. So a dirty start FAILS, loudly.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
W=$(mktemp -d /tmp/amigafs-m42.XXXXXX)
trap 'rm -rf "$W"' EXIT
incomplete=0
incomplete_list=""
cd "$ROOT" || exit 2

# The sample is COMPUTED from the tree, never hand-listed.
#
# The first version named seven gates by hand, when the suite had 33. By the time
# the suite reached 35 it was missing m39_blockio_fuzz -- the libFuzzer gate that
# writes crash-<hash> into the CURRENT DIRECTORY, which is the exact failure this
# gate exists to catch, discovered here because m39 had written one into the repo
# root and `git add` had committed it. A hand-list goes stale silently: nothing
# fails when a new gate is added, it just quietly escapes the check.
#
# So the sample is every gate that could plausibly touch the tree, found by
# looking for the tools that do it. Adding a gate that uses unadf, clang, FUSE or
# git therefore joins the sample automatically, and one that does none of those is
# cheap to omit because it cannot produce debris.
#
# This gate itself is excluded: it runs git, so including itself would recurse.
SELF=m42_no_repo_writes
mapfile -t SAMPLE < <(
    grep -lE 'unadf|clang|fsanitize=fuzzer|amigafs-fuse|git ' tests/oracle/*.sh \
      | xargs -n1 basename | sed 's/\.sh$//' | grep -v "^${SELF}$" | sort -u
)

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
    # Run from the REPOSITORY ROOT, deliberately. run_all_gates.sh invokes gates
    # as `bash "$HERE/oracle/$g.sh"` with no cd, so they inherit the caller's cwd,
    # which in practice is the repo root -- and that is exactly where `unadf`
    # drops its extraction. An earlier version of this gate ran each gate from a
    # scratch directory instead, which PREVENTED the debris rather than detecting
    # it and left the digest check nearly vacuous: a gate that writes via an
    # absolute or repo-relative path would still have been caught, but the common
    # case (extract into cwd) could never trip it.
    #
    # The gate's VERDICT is not what is under test -- what is under test is its side
    # effect on the repository -- so a sampled gate that fails its own assertions
    # must not be reported here as if it had dirtied the tree. Those are two
    # different verdicts and they are kept apart below.
    #
    # But the exit status is NOT discarded, because "no debris" from a gate that
    # never ran is not evidence of anything. This gate used to pipe every gate's
    # output to /dev/null and append || true, on the stated grounds that "several
    # of these exit non-zero when run out of band". Measured: on a clean tree
    # 0 of 21 exit non-zero. The comment asserted a fact about the suite that was
    # never true, and it happened to be load-bearing -- it licensed throwing away
    # the only evidence that a gate had actually run.
    #
    # That gap was demonstrated in this session's own mutant: the fuzz target
    # failed to link, so no crash was found, so the run produced no debris, and
    # this gate reported "repository byte-identical after 21 gate(s)" for a gate
    # that had never executed a single input.
    ( cd "$ROOT" && timeout 900 bash "$script" ) >"$W/$g.out" 2>&1
    rc=$?
    ran=$((ran + 1))
    if [ $rc -ne 0 ];   then
        incomplete=$((incomplete + 1))
        incomplete_list="$incomplete_list $g(rc=$rc)"
    fi
done

after=$(digest)
echo "  ran $ran gate(s), skipped $skipped"

# Checked BEFORE the digest verdict, and deliberately as a separate failure. A
# gate that aborted early produced no debris -- so on the digest alone this gate
# would pass, and "no gate writes into the repository" would be a claim about
# gates that never ran. Ordering it first also means a suite with a real
# regression reads as one failure here rather than two that look independent.
if [ "$incomplete" -gt 0 ]; then
    echo "  FAIL  $incomplete of $ran sampled gate(s) did not complete:"
    for g in $incomplete_list; do
        echo "        $g"
        tail -4 "$W/${g%%(*}.out" 2>/dev/null | sed 's/^/            /'
    done
    echo
    echo "  This is NOT a statement that a gate dirtied the tree. It is that a gate"
    echo "  did not finish, so its side effects were never exercised and their"
    echo "  absence proves nothing. Look above: usually the real cause, which"
    echo "  run_all_gates.sh reports in full."
    echo
    echo "M42 gate: FAIL ($incomplete sampled gate(s) incomplete)"
    exit 1
fi

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