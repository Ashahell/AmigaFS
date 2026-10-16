#!/bin/bash
# M46 -- a golden test of the message -> errno mapping.
#
# WHY. afs_err_to_errno classifies by scanning for substrings, in a fixed order,
# first match wins (M45). That makes THE WORDING OF EVERY LIBRARY MESSAGE
# LOAD-BEARING: a message that happens to contain two keywords gets whichever rule
# comes first, and the other becomes unreachable for it. M45's own test was wrong
# twice for exactly this reason -- "cannot find name" classified as EINVAL because
# it contains "name".
#
# So nothing in the suite noticed, and nothing would: rewording "dir-cache chain
# corrupt" to mention "full" would silently turn EIO into ENOSPC, and the mount
# would start reporting a full volume for a corrupt one. That is the kind of change
# that looks like a typo fix and ships as a behaviour change.
#
# WHAT THIS DOES. It enumerates the messages the library can actually produce,
# asks the REAL function what each one maps to, and diffs the whole table against a
# committed baseline. Any of three things fails the gate:
#
#   - a message is reworded, added or removed
#   - a rule is reordered, added or removed
#   - a new call site introduces a message the baseline has never seen
#
# Note what this is NOT: it does not reimplement the substring rules. An earlier
# enumeration script carried its own copy of them and reported the OLD
# classification the moment err_to_errno changed -- a second copy of a library
# invariant, going stale silently. Here the classification is obtained BY CALLING
# the function, so the table cannot disagree with the code; the baseline is a
# record of observed behaviour, not a second implementation of it.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BASE="$ROOT/tests/data/errno_baseline.tsv"
W=$(mktemp -d /tmp/amigafs-m46.XXXXXX)
trap 'rm -rf "$W"' EXIT

fails=0
bad() { printf '  %-52s FAIL (%s)\n' "$1" "$2"; fails=$((fails + 1)); }
note() { printf '  %-52s %s\n' "$1" "$2"; }

# 1. what messages can the library produce? Collected from the source, because the
#    library does not expose a registry -- which is itself worth knowing.
python3 - "$ROOT" > "$W/msgs.txt" <<'PY'
import re, glob, os, sys
root = sys.argv[1]
seen = {}
for path in sorted(glob.glob(os.path.join(root, "src/lib/*.c"))):
    src = open(path).read()
    for m in re.finditer(r'set_err\(\s*\w+\s*,\s*\w+\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', src):
        text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
        text = text.replace('\\n', ' ').strip()
        # a format string's %s would be substituted at runtime; pin the shape
        text = re.sub(r'%[-0-9.*lhzu]*[a-zA-Z]', '{arg}', text)
        if text and text not in seen:
            seen[text] = os.path.basename(path)
for text in sorted(seen):
    print(f"{seen[text]}\t{text}")
PY
n=$(wc -l < "$W/msgs.txt" | tr -d ' ')
[ "$n" -gt 0 ] || { echo "M46 gate: FAIL -- no messages extracted"; exit 1; }

# 2. ask the real function. Linking the library rather than reimplementing the
#    rules is the whole point: the table cannot drift from the code.
cat > "$W/ask.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "amigafs/translate.h"
int main(void) {
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        printf("%s\t%s\t%d\n", line, tab + 1, afs_err_to_errno(tab + 1));
    }
    return 0;
}
EOF
if ! cc -O1 -o "$W/ask" "$W/ask.c" "$ROOT/src/lib/translate.c" -I"$ROOT/include" \
        -D_POSIX_C_SOURCE=200809L 2>"$W/build.log"; then
    # translate.c pulls in the rest of the vocabulary; link the whole library
    if ! cc -O1 -o "$W/ask" "$W/ask.c" "$ROOT"/src/lib/*.c -I"$ROOT/include" \
            2>>"$W/build.log"; then
        echo "M46 gate: FAIL -- could not build the probe"; head -8 "$W/build.log" | sed 's/^/    /'
        exit 1
    fi
fi

"$W/ask" < "$W/msgs.txt" 2>/dev/null > "$W/table.tsv"
got=$(wc -l < "$W/table.tsv" | tr -d ' ')
if [ "$got" != "$n" ]; then
    bad "every extracted message was classified" "extracted $n, classified $got"
else
    note "every extracted message was classified" "$got of $n"
fi

# 3. diff against the baseline
# A MISSING BASELINE IS A FAILURE, not a first-run convenience. The first version
# wrote one and passed -- which means a fresh clone, or a checkout where the
# baseline was lost, gets a gate that reports green on its FIRST run and has
# checked nothing. That is the same vacuous-green trap the write fuzzer's
# coverage floors were added to close, in a different costume: a check whose
# expectation it writes for itself.
if [ ! -f "$BASE" ]; then
    bad "the baseline exists" "missing: $BASE"
    echo
    echo "  It must be COMMITTED -- this gate pins observed behaviour, so the record"
    echo "  has to exist before the check can mean anything. Generating it on a"
    echo "  missing baseline makes every fresh clone green on its first run."
    echo
    echo "  To create it deliberately, on a tree you have reviewed:"
    echo "      mkdir -p tests/data && cp <table> $BASE"
    echo
    echo "M46 gate: FAIL (no baseline)"
    exit 1
fi

if diff -u "$BASE" "$W/table.tsv" > "$W/diff.txt"; then
    note "message -> errno table matches the baseline" "no change"
else
    changed=$(grep -cE '^[+-][^+-]' "$W/diff.txt")
    bad "message -> errno table matches the baseline" "$changed line(s) changed"
    echo
    echo "  A message was reworded, added or removed, or a rule changed. That is"
    echo "  the silent-behaviour-change this gate exists for: rewording a message"
    echo "  so it contains another keyword reclassifies it with no other signal."
    echo
    grep -E '^[+-][^+-]' "$W/diff.txt" | head -20 | sed 's/^/    /'
    echo
    echo "  If the change is intended:"
    echo "      cp $W/table.tsv $BASE"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "M46 gate: PASS ($got messages pinned)"
    exit 0
fi
echo "M46 gate: FAIL ($fails)"
exit 1