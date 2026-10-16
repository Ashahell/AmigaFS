#!/usr/bin/env bash
# m24_unadf_link_blindness.sh -- pin the ORACLE'S ACTUAL CONTRACT.
#
# Why this gate exists
# -------------------
# Implementing hard-link creation (M24), I "proved" it broken: `unadf`
# extracted 3 of 9 files after `afs link`, versus 9 of 9 before, and I reported
# that as the oracle catching a regression. It was not. Investigating took four
# wrong turns before the truth, and each wrong turn is a trap worth closing:
#
#   1. I blamed the next_link chain append. It was exonerated: with the append
#      the oracle extracts 9/9 byte-identical.
#   2. I blamed the stale bitmap on testffs.adf. The blocks it handed out were
#      genuinely free.
#   3. I blamed unadf being nondeterministic. Five runs on the pristine fixture
#      all gave 9.
#   4. I concluded "unadf drops hard links". It does not: on links.adf it
#      extracts both real files CORRECTLY, resolving each link to its target.
#
# The actual behaviour: unadf emits a per-entry "No such file or directory" for
# whatever it cannot materialise -- links (it cannot give a link its own name),
# and some ordinary files it walks oddly -- then carries on, extracting every
# real file with correct bytes and exiting 0. Its stderr is therefore NOT a
# damage signal, and its exit code is NOT a link signal. Only the extracted
# bytes are.
#
# The generalisable lesson, and the reason this is a gate rather than a note: an
# oracle that SILENTLY SKIPS a construct is worse than no oracle, because its
# output still looks like evidence. A gate that asserted "unadf agrees" would
# have passed on a broken volume and failed on a correct one.
#
# What it asserts, on every link-bearing and link-free fixture:
#   A. Every file unadf extracts is BYTE-IDENTICAL to what our reader returns
#      at the same path. This is the only comparison that is actually about
#      data, and it is direction-checked in both tools.
#   B. No real file goes missing: unadf's extraction count equals the number of
#      ordinary (non-link) files our reader lists.
#   C. Our own reader verifies each fixture structurally clean.
#   D. Load-bearing: the gate is proven to fail when the contract is violated
#      (see the mutant at the end), so a green run cannot be vacuous.

set -u
cd "$(dirname "$0")/../.." || exit 2

AFS="${AFS_BIN:-build/afs}"
rc=0
note() { printf '  %s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; rc=1; }

command -v unadf >/dev/null 2>&1 || { echo "SKIP: unadf not on PATH"; exit 77; }
[ -x "$AFS" ] || { echo "SKIP: $AFS not built"; exit 77; }

tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT

# Prefer a link-bearing fixture; fall back so the gate still runs without it.
pick=()
for fx in links testffs testodf testffs2; do
    [ -f "tests/fixtures/$fx.adf" ] && pick+=("tests/fixtures/$fx.adf")
done
[ "${#pick[@]}" -gt 0 ] || { echo "FAIL: no fixtures present (run fetch_fixtures.sh)"; exit 1; }

for fx in "${pick[@]}"; do
    note "--- $fx ---"
    work="$tmp/$(basename "$fx" .adf)"
    mkdir -p "$work"
    cp "$fx" "$work/t.adf"
    unadf -d "$work/out" "$work/t.adf" >/dev/null 2>"$work/err"
    urc=$?

    # A. every extracted file byte-identical to our reader at the same path
    mism=0; n_ex=0
    while IFS= read -r f; do
        n_ex=$((n_ex + 1))
        rel="${f#"$work/out"}"
        ours=$(./build/afs cat "$work/t.adf" "$rel" 2>/dev/null | sha256sum | cut -c1-16)
        theirs=$(sha256sum "$f" | cut -c1-16)
        if [ "$ours" != "$theirs" ]; then
            fail "$fx: $rel differs (oracle=$theirs ours=${ours:-none})"
            mism=$((mism + 1))
        fi
    done < <(find "$work/out" -type f ! -name '*.info' 2>/dev/null)
    [ "$mism" -eq 0 ] && note "ok      all $n_ex extracted files byte-identical to our reader"

    # B. no real file went missing. Count ordinary files via our own listing.
    real_files=$(./build/afs ls "$work/t.adf" / -R 2>/dev/null | awk '$1 ~ /^f/ {n++} END {print n+0}')
    if [ "$n_ex" -eq "$real_files" ]; then
        note "ok      no data lost: $n_ex extracted == $real_files real files"
    else
        fail "$fx: unadf extracted $n_ex but the volume holds $real_files ordinary files"
    fi

    # C. our reader calls the fixture structurally clean
    if ./build/afs verify "$work/t.adf" >"$work/v" 2>&1; then
        note "ok      our reader verifies it clean (unadf exit was $urc, stderr not a damage signal)"
    else
        fail "$fx: our verify rejected it: $(head -3 "$work/v" | tr '\n' ' ')"
    fi
done

# D. Load-bearing: the byte-comparison must actually be able to fail. Corrupt
#    one extracted file and confirm the comparison catches it. Without this, a
#    broken comparison (empty both sides, wrong path) would report green.
cp "${pick[0]}" "$tmp/mut.adf"
unadf -d "$tmp/mutout" "$tmp/mut.adf" >/dev/null 2>&1
victim=$(find "$tmp/mutout" -type f ! -name '*.info' 2>/dev/null | head -1)
if [ -n "$victim" ]; then
    printf 'X' >>"$victim"                       # flip one byte
    rel="${victim#"$tmp/mutout"}"
    ours=$(./build/afs cat "$tmp/mut.adf" "$rel" 2>/dev/null | sha256sum | cut -c1-16)
    theirs=$(sha256sum "$victim" | cut -c1-16)
    if [ "$ours" != "$theirs" ]; then
        note "ok      load-bearing: a 1-byte change is detected (${ours} vs ${theirs})"
    else
        fail "the byte comparison cannot detect a change: the gate is vacuous"
    fi
else
    fail "no file extracted, cannot prove the comparison is load-bearing"
fi

[ "$rc" -eq 0 ] && echo "PASS m24_unadf_link_blindness" || echo "FAILED m24_unadf_link_blindness"
exit "$rc"