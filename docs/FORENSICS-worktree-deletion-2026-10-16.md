# The 2026-10-16 worktree deletion: what was excluded, and what was not

A bounded forensic record. The purpose is to save the next session from repeating
this search, and to be honest that it did not reach a cause.

## What happened

`tests/run_all_gates.sh` reported **ALL GREEN**. The next command in the same shell
invocation failed with:

```
fatal: Unable to read current working directory: No such file or directory
```

The working directory was `/home/miller/Work/projects/Vulkan4Aros/.worktrees/amigafs/amigafs`.
Every tracked source file and the worktree's `.git` were gone. `build/` survived, and
`git worktree list` showed only the main repository — i.e. the worktree had been
properly de-registered, not merely `rm -rf`'d.

## Recovery (nothing was lost)

- Worktrees share the object store, so the last commit (`e5c60667`, M49) survived as
  an object even with its `.git` gone. It was pushed straight back to `origin/amigafs`.
- The worktree was re-created with `git worktree add`, and the fixtures re-fetched
  (they are fetched, not committed — that is exactly the distinction that decided how
  much work was lost: **the work lost was the work not yet committed**).

## Window, from filesystem timestamps

Deletion at **~17:06–17:07 local** (CEST, UTC+2). `build/Testing/Temporary/LastTest.log`
was written at 17:06, so **ctest was running and writing into `build/` at the moment the
source disappeared.** The deletion happened *during* the test run, not before it.

## What was excluded, and how the exclusion was validated

Rule 285: a negative search is only evidence of absence if the search has been shown
to find a known-present case. The search was validated — it **did** return 40 `rm -rf`
occurrences across the gates, so it can match. It found no *destructive target*.

| Suspect | Finding |
|---|---|
| All 38 oracle gates | No destructive `git` (`stash`/`checkout`/`clean`/`reset`/`worktree`) anywhere; only a comment in `m42` documenting a past incident. All 40 `rm -rf` targets are `mktemp -d` paths. |
| `fuzz_gate` / `mount_fuzz` | `set -uo pipefail`; `WORK` from `mktemp -d`; `OUT` from `${TMPDIR:-/tmp}`. `fuzz_test.c` opens fixtures `"rb"` only — no `remove`, `unlink`, `rename`, `system`, or write-mode `fopen`. |
| 14 unit tests | `test_blockio.c` calls `remove(path)` on a file it created. No other destructive calls. |
| `run_all_gates.sh`, `CMakeLists.txt` | No destructive git or `rm` outside the per-gate scratch dirs. |
| `scripts/aros_audit.sh` (other lane, ran 17:08:37 local, rc=0, 80 s) | All 11 `rm -rf` targets are `mktemp -d` / `mktemp`. |
| `scripts/aros_pin.py` (other lane, removes worktrees) | Operates on the **AROS** repository at `src/abi/v1/AROS`, a different repository from this one. |
| `git clean -fd` / `git stash -u` from the main checkout | `.worktrees/` **is** gitignored (`.gitignore:221`), so neither reaches it without `-x`. |
| Disk exhaustion | 1.7 TB free on `/home`, 10 % used. `/tmp` is tmpfs at 23 %. |

## Not excluded

- **Concurrent activity in the main checkout by another lane.** Confirmed in the same
  window: `src/abi/v1/AROS/.git/worktrees/wt_PRE/*`, `tests/drm_host/test_stdio_contract.{c,py}`
  and `src/abi-patches/v1/aros/0075-*.diff` were all being written between 17:00 and 17:10.
  This is the leading remaining hypothesis and it **cannot be tested from inside this
  worktree** — it requires knowing what else was running.
- Anything outside the repository and the gates.

## Does it reproduce?

**No.** Two full `run_all_gates.sh` runs, a full `ctest` run, and every gate individually
have run since, with the tree intact and `git status` clean.

## What is in place regardless

`run_all_gates.sh` now fingerprints the tracked tree (file count **and** digest) before
any gate and after all 38, and fails if it moved. The count alone is insufficient:
`git ls-files` counts tracked paths whether or not the file is *present*, so a deletion
is invisible to a count. Proven load-bearing by deleting a tracked source file during a
run.

## Why no fix was attempted

The cause is not known. A speculative patch to a gate would be a fabrication in the shape
of a code change. The honest state is: **detected, cause unknown, caught if it recurs.**