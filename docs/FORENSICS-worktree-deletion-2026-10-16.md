# The worktree deletions of 2026-10-16: what was excluded, and what was not

A bounded forensic record. The purpose is to save the next session from repeating the
search, to be honest that it never reached a cause, and — after the second occurrence —
to record that the first version's conclusion ("does not reproduce") was wrong.

## What happened

`tests/run_all_gates.sh` reported **ALL GREEN**. The next command in the same shell
invocation failed with:

```
fatal: Unable to read current working directory: No such file or directory
```

The working directory was `/home/miller/Work/projects/Vulkan4Aros/.worktrees/amigafs/amigafs`.
Every tracked source file and the worktree's `.git` were gone. `git worktree list` showed
only the main repository — the worktree had been properly de-registered, not merely
`rm -rf`'d.

## It happened three times

| | 1st ~17:06 | 2nd ~20:02 | 3rd ~20:09 |
|---|---|---|---|
| What survived | `build/` only | **nothing** | `build/Testing` only |
| ctest logs survived | **yes** (found later) | no | yes |
| Registry entry | gone | gone | never created; dir exists with no `.git` |
| Co-occurring signal | `src/abi/v1/AROS/.git/worktrees/**wt_PRE**/` | `src/abi/v1/AROS/.git/worktrees/**wt_PRE3**/` | as 2nd |
| Uncommitted work lost | none | none | **`CMakeLists.txt` + this file** |

The differing signatures matter: the first left `build/` behind, the later ones removed
the directory entirely. That is not one mechanism observed three times.

### The first occurrence's own logs — found later, and they do NOT match the third

When the third occurrence happened I preserved three `ctest` logs from it. Only afterwards,
clearing the directory that survived the **first** occurrence, did I find that it had a
`build/Testing/Temporary/` too — logs from **17:06**, an incident I had recorded without
any artefact. Compared:

```
                        incident 1 (17:06)              incident 3 (20:09)
LastTest.log            123 B, Start/End only           123 B, Start/End only
LastTestsFailed.log     '53:all_gates'                  gates 28, 53, 54
CTestCostData.txt       header only, 0 of 54 timings    (file did not survive)
```

The shared 123-byte `LastTest.log` made these look like one story. They are not, and the
discriminator is `CTestCostData.txt`. Controls run to establish that:

- A healthy `CTestCostData.txt` is **55 lines / 1729 bytes**, and it is **cumulative** —
  it does not shrink between runs.
- During a run it is already at full size, so ctest does not truncate it as it goes.
- Incident 1's file is exactly `---\n`: a header written once, with **no test ever
  recorded a timing line**.

So in incident 1, ctest wrote its header and then recorded **no completed test at all** —
the directory went while the very first test was in flight. In incident 3 the failure list
is timestamped *before* `LastTest.log` and names the trailing gates, which is why that one
can be placed at the end of a completed run.

**Two occurrences that share an artefact's byte size are not thereby the same event.** The
lesson is the one I had to learn twice this session and is worth stating once: a
coincidentally identical signature (`123 bytes`) is not corroboration, and treating it as
such would have produced a confident, tidy, wrong account of incident 1 — which is close
to what I wrote in the wiki log about incident 3 before running the control at all.

### WHEN, exactly, in the third occurrence — and a claim I had to retract

Three `ctest` logs survived the third deletion inside `build/Testing/Temporary/`. Their
**mtimes are the evidence**, and they corrected an inference I published before checking
them:

```
LastTestsFailed.log   20:09:42   names gates 28, 53, 54   <- the last three of 54
LastTest.log          20:09:49   123 bytes, Start/End only
```

I had written that ctest "died on its **first** test, before recording a result." **That
was wrong.** The failure list is timestamped 20:09:42 and names the *final* three gates,
so ctest ran **all 54 tests to completion** and recorded three failures. The deletion hit
seven seconds later, at 20:09:49, **while ctest was writing its final log** — which is
why `LastTest.log` survived as a Start/End pair with no per-test entries. The directory
was removed during the run's last write, not at its start.

Two things about how that mistake happened are worth more than the mistake:

- **A control would have caught it.** A healthy run's `LastTest.log` is 3389 lines with
  per-test `N/54 Testing:` entries; the incident copy is 4. But I read only the incident
  copy and inferred from its shape. The control is one command, and it is the whole
  difference between "died at the start" and "died at the end".
- **`LastTestsFailed.log` is stale-prone and I nearly leaned on it.** In a surviving
  clone, a run at 100% passed, 54/54, on a clean tree, and the file still listed four
  failures with an mtime from an **earlier** run — ctest does not rewrite it on success.
  So "this file lists failures" does not mean "this file describes this run". The mtime
  is what makes it current here. **A log file is evidence of the run that wrote it, and
  you have to check which run that was.**

## Recovery (nothing committed was ever lost, three times)

- Worktrees share the object store, so a commit whose `.git` is gone still exists as an
  object and can be pushed back: `git push origin <sha>:refs/heads/amigafs`.
- **Worktrees share the object store** is the load-bearing fact, and it held every time.
- The fixtures are fetched rather than committed, so they were re-fetched. **The work
  lost, all three times, was exactly the work not yet committed.**

## THE DECISIVE EXPERIMENT, and what it exonerated

The suspicion fell on this project's own gates first, because both deletions happened
while gates were running. Settled by running the suite **outside the main repository**:

```
for i in 1 2 3; do ctest --test-dir build; done
```

in a plain clone at `/tmp`. Three consecutive full `ctest` runs — 54 tests each,
including `m42_no_repo_writes` which itself runs 24 gates from the repository root —
left the tree intact every time. **This project's gates do not delete it.**

That matters, because it moves the hypothesis from "something external" to something
specific and checkable: the worktree lives **inside the main repository's gitignored
`.worktrees/`**, and an AROS-lane worktree operation (`wt_PRE*`) was in progress in the
same window both times it was checked. A tool in that lane that enumerates and removes
worktrees would not have created `.worktrees/amigafs`, and `.worktrees/` being
**gitignored** means it is invisible to `git status` — and therefore invisible to any
tool that reasons from `git status`.

**This remains unproven.** Settling it needs to know what else was running, which this
repository cannot see. But it is now a named suspect with a 3/3 correlation, not a
vague worry.

## What was excluded, and how the exclusion was validated

Rule 285: a negative search is only evidence of absence if the search has been shown
to find a known-present case. The search was validated — it **did** return 40 `rm -rf`
occurrences across the gates, so it can match. It found no *destructive target*.

| Suspect | Finding |
|---|---|
| All 38 oracle gates | No destructive `git` anywhere; only a comment in `m42` documenting a past incident. All 40 `rm -rf` targets are `mktemp -d` paths. |
| `fuzz_gate` / `mount_fuzz` | `set -uo pipefail`; `WORK` from `mktemp -d`; `OUT` from `${TMPDIR:-/tmp}`. `fuzz_test.c` opens fixtures `"rb"` only. |
| 14 unit tests | `test_blockio.c` calls `remove(path)` on a file it created. No other destructive calls. |
| `run_all_gates.sh`, `CMakeLists.txt` | No destructive git or `rm` outside the per-gate scratch dirs. |
| `scripts/aros_audit.sh` (other lane) | All 11 `rm -rf` targets are `mktemp -d` / `mktemp`. |
| `scripts/aros_pin.py` (other lane, removes worktrees) | Operates on the **AROS** repository at `src/abi/v1/AROS` — a different repository. **Not the cause, but see the hypothesis above**: the sweep may be wider than its own repository. |
| `git clean -fd` / `git stash -u` | `.worktrees/` **is** gitignored, so neither reaches it without `-x`. |
| Disk exhaustion | 1.7 TB free; `/tmp` tmpfs at 23 %. |
| **This project's own gates** | **Exonerated by experiment** — 3 × full `ctest` in an isolated clone, tree intact. |

## A negative result that was wrong, and what it cost

The first version of this record said **"Does it reproduce? No."** That was an honest
reading of the evidence available at the time: the harness had run twice, `ctest` had
run, and every gate had run, all clean.

But **a clean run of the thing that was broken is not evidence that the thing is not
broken** — only that *running it* does not trigger it. Both early occurrences happened
while gates were running, and both were discovered when the *next* command failed,
never by anything watching.

The harness fingerprint added after the first incident **did not fire on the second or
third**, because it only observes during its own run — the limitation rule 292 records,
now confirmed three times rather than argued.

## Working practice adopted

- **Do the work in a clone outside the main repository's tree.** Proven stable over
  three full `ctest` runs; the worktree lane was destroyed three times in one evening.
- **Commit and push promptly.** Every commit survived; every uncommitted change did not.
- **When a clone is created with `--depth 1` and then `git checkout FETCH_HEAD`, it is
  on a DETACHED HEAD**, and `git push origin main` from there is a no-op. Use
  `git push origin HEAD:main`. This cost one push here.

## What is in place regardless

`run_all_gates.sh` fingerprints the tracked tree (file count **and** digest) before any
gate and after all of them, and fails if it moved. The count alone is insufficient:
`git ls-files` counts tracked paths whether or not the file is *present*, so a deletion
is invisible to a count. Both are taken because they fail differently.

## Why no fix was attempted

The cause is still not proven, and a speculative patch to a gate — or to a tool in
another lane — would be a fabrication in the shape of a code change. What changed is
that the hypothesis is no longer vague: it is a named suspect, observed 3/3, with an
experiment that exonerated this project. The honest state is: **detected, cause still
unproven, suspect named, and the working practice changed so the hazard cannot reach
the work.**