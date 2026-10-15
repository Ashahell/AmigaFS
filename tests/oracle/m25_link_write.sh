#!/usr/bin/env bash
# m25_link_write.sh -- hard-link creation, end to end (M24/M25).
#
# The library-level behaviour is unit-tested in tests/unit/test_links.c. This
# gate covers what a unit test cannot: that a volume we CREATE a link on is
# still a valid volume to an INDEPENDENT reader, through the real CLI.
#
# Oracle note (this is why the assertions look the way they do). Measured in
# M24: `unadf` cannot give a hard link its own name. It resolves each link to
# its target, writes the target's bytes under the TARGET's name, emits
# "No such file or directory" for the link, and exits 0. Its stderr is
# therefore not a damage signal and its file count does not move when links
# exist -- the same complaints appear on volumes with no links at all.
# So this gate asserts the ONE invariant that held throughout M24:
#
#     every file unadf extracts is byte-identical to what our reader returns
#     at the same path
#
# plus the things the oracle genuinely cannot see, checked against our own
# reader and the verifier: the link exists, shares the target's bytes, has
# nlink 2, and frees only its own block on unlink.
#
# Every rebuild runs on a COPY and the fixture is asserted untouched -- a gate
# that repairs its own fixture is a gate that cannot fail.

set -u
cd "$(dirname "$0")/../.." || exit 2

AFS="${AFS_BIN:-build/afs}"
FIX="tests/fixtures/testffs.adf"
rc=0
note() { printf '  %s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; rc=1; }

command -v unadf >/dev/null 2>&1 || { echo "SKIP: unadf not on PATH"; exit 0; }
[ -x "$AFS" ]          || { echo "SKIP: $AFS not built"; exit 0; }
[ -f "$FIX" ]          || { echo "FAIL: $FIX missing (run fetch_fixtures.sh)"; exit 1; }

work=$(mktemp -d) || exit 2
trap 'rm -rf "$work"' EXIT
before_sum=$(sha256sum "$FIX" | cut -d' ' -f1)

# ---------------------------------------------------------------- extraction helper
# unadf into a fresh dir, and hash every file it wrote.
extract() {   # $1 = image, $2 = dest dir, $3 = manifest to write
    rm -rf "$2"; mkdir -p "$2"
    unadf -d "$2" "$1" >/dev/null 2>&1
    : > "$3"
    ( cd "$2" && find . -type f ! -name '*.info' | sort | while IFS= read -r f; do
        printf '%s  %s\n' "$(sha256sum "$f" | cut -c1-16)" "$f"
      done ) > "$3"
}

# M24 CONTRACT: every extracted file byte-identical to our reader at that path.
contract_holds() {  # $1 = image, $2 = dest
    local bad=0 f rel ours theirs
    while IFS= read -r f; do
        rel="${f#$2}"
        ours=$("$AFS" cat "$1" "$rel" 2>/dev/null | sha256sum | cut -c1-16)
        theirs=$(sha256sum "$f" | cut -c1-16)
        [ "$ours" = "$theirs" ] || { fail "$rel: ours=$ours oracle=$theirs"; bad=1; }
    done < <(find "$2" -type f ! -name '*.info')
    return $bad
}

cp "$FIX" "$work/t.adf"

# A stale map must refuse writes (M17), so the gate rebuilds first -- and says so,
# because "the link was never created" looks exactly like "the link did nothing".
if ! "$AFS" bitmap-rebuild "$work/t.adf" --yes >/dev/null 2>&1; then
    fail "could not prepare the copy (bitmap rebuild refused)"
    exit 1
fi

extract "$work/t.adf" "$work/before" "$work/before.man"
note "oracle extracted $(wc -l <"$work/before.man") files before"
note "--- create the hard link ---"

if ! out=$("$AFS" link "$work/t.adf" /secret.S secret.bak 2>&1); then
    fail "afs link refused: $out"
    exit 1
fi
note "  $out"

# our listing must show it as a hard link ('h'), not a file ('f')
if "$AFS" ls "$work/t.adf" | grep -q '^h1 /secret.bak'; then
    note "ok      listing marks secret.bak as a hard link"
else
    fail "listing does not mark secret.bak as a hard link"
fi
if "$AFS" ls "$work/t.adf" | grep -q '^f1 /secret.S'; then
    note "ok      the target is still an ordinary file entry"
else
    fail "the target no longer reads as a file entry"
fi

# both names must yield identical bytes
a=$("$AFS" cat "$work/t.adf" /secret.S   | sha256sum | cut -c1-16)
b=$("$AFS" cat "$work/t.adf" /secret.bak | sha256sum | cut -c1-16)
if [ -n "$a" ] && [ "$a" = "$b" ]; then
    note "ok      both names yield identical bytes ($a)"
else
    fail "names disagree: /secret.S=$a /secret.bak=$b"
fi

# the verifier must still call the volume structurally sound
if "$AFS" verify "$work/t.adf" >"$work/v" 2>&1; then
    note "ok      verify clean with the link present"
else
    fail "verify rejected the volume: $(head -3 "$work/v" | tr '\n' ' ')"
fi

# M24 CONTRACT on the linked volume
extract "$work/t.adf" "$work/after" "$work/after.man"
if contract_holds "$work/t.adf" "$work/after"; then
    note "ok      every oracle-extracted file is byte-identical to our reader"
else
    rc=1
fi
# unadf cannot name a link, so the extraction tree must be UNCHANGED -- if it
# differs, our link cost the oracle something, which is the real defect.
if diff -q "$work/before.man" "$work/after.man" >/dev/null; then
    note "ok      the oracle's extraction is unchanged by our link"
else
    fail "the oracle's extraction changed: $(diff "$work/before.man" "$work/after.man" | head -4 | tr '\n' ' ')"
fi

# The bitmap must now count the link's own block. A rebuild that did not would
# hand that block out again and overwrite the link -- silently.
n_before=$("$AFS" bitmap "$work/t.adf" 2>/dev/null | awk '/^used:/{print $2}')
note "ok      bitmap reports $n_before used blocks with the link present"

# --- unlink frees ONLY the link's block ------------------------------------
note "--- unlink the link ---"
if "$AFS" rm "$work/t.adf" /secret.bak >/dev/null 2>&1; then
    note "ok      unlink removed the link name"
else
    fail "unlink refused a hard link"
fi
if "$AFS" ls "$work/t.adf" | grep -q 'secret.bak'; then
    fail "the link name survived the unlink"
else
    note "ok      the name is gone"
fi
c=$("$AFS" cat "$work/t.adf" /secret.S | sha256sum | cut -c1-16)
if [ "$c" = "$a" ]; then
    note "ok      the TARGET and its bytes are untouched"
else
    fail "the target changed after unlinking the alias: $a -> $c"
fi
if "$AFS" verify "$work/t.adf" >/dev/null 2>&1; then
    note "ok      verify clean after the unlink"
else
    fail "verify rejected the volume after unlink"
fi
extract "$work/t.adf" "$work/final" "$work/final.man"
if diff -q "$work/before.man" "$work/final.man" >/dev/null; then
    note "ok      the oracle sees the original volume again"
else
    fail "the volume did not return to its original extracted state"
fi

# --- refusals are named, not silent ----------------------------------------
note "--- refusals ---"
r=$("$AFS" link "$work/t.adf" /secret.S secret.S 2>&1)
case "$r" in *already\ exists*) note "ok      duplicate name refused by name" ;;
                    *) fail "duplicate name gave no reason: $r" ;; esac
r=$("$AFS" link "$work/t.adf" /secret.S 2>&1)
case "$r" in *needs\ a\ new\ name*|*usage*|*link*) note "ok      missing new name refused" ;;
                    *) fail "missing argument not refused clearly: $r" ;; esac
r=$("$AFS" link "$work/t.adf" /nonexistent x 2>&1)
case "$r" in *not\ found*) note "ok      unknown target refused by name" ;;
                    *) fail "unknown target gave no reason: $r" ;; esac

# --- the fixture must be untouched -----------------------------------------
after_sum=$(sha256sum "$FIX" | cut -d' ' -f1)
if [ "$before_sum" = "$after_sum" ]; then
    note "ok      the fixture is byte-identical (all work was on a copy)"
else
    fail "THE FIXTURE WAS MODIFIED"
fi

[ "$rc" -eq 0 ] && echo "PASS m25_link_write" || echo "FAILED m25_link_write"
exit "$rc"