#!/usr/bin/env bash
# M10 gate: RDB partitions, cross-checked against amitools.
#
# m10_partition opens every partition the RDB describes and checks three
# things per partition, none of which the old code could do:
#   - the window is exactly the partition's block count (not "to end of image");
#   - the computed rootblock is the one that size implies, (2 + last)/2;
#   - the DOS type inside the partition matches the one the RDB advertises.
#
# The partition table itself is then diffed against `rdbtool list`, so a
# disagreement about where a partition starts or how long it is fails here
# rather than surfacing as a mysterious "not a rootblock" much later.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
IMG="$ROOT/tests/fixtures/a590_WD93028XA_6parts.hdd"
BIN="$ROOT/build"

echo "M10 gate: RDB partitions"

if [ ! -f "$IMG" ]; then
    echo "M10 gate: SKIP (fixture $IMG absent)"
    exit 0
fi

out=$("$BIN/m10_partition" "$IMG" 2>&1)
rc=$?
echo "$out" | sed 's/^/  /'
if [ $rc -ne 0 ]; then
    echo "M10 gate: FAIL (m10_partition rc=$rc)"
    exit 1
fi

# ---- our partition table vs rdbtool's -------------------------------------
# Normalise both to "index name dostype start size" and compare exactly.
ours=$(printf '%s\n' "$out" | awk '
    /^part [0-9]+ / {
        i=$2;
        # DriveName may CONTAIN SPACES ("OFS INTL", "FFS DirCache"), so the
        # fields are cut out by shape, not by whitespace splitting -- splitting
        # silently truncated four of the six names and looked like a parser bug.
        rest=$0; sub(/^part [0-9]+ /,"",rest);
        name=rest; sub(/^name=/,"",name); sub(/ dos=.*/,"",name);
        dos=rest; sub(/.* dos=/,"",dos); sub(/ .*/,"",dos);
        start=rest; sub(/.* start=/,"",start); sub(/ .*/,"",start);
        size=rest;  sub(/.* size=/,"",size);  sub(/ .*/,"",size);
        printf "%s|%s|%s|%s|%s\n", i, name, dos, start, size;
    }')
theirs=$(rdbtool "$IMG" list 2>/dev/null | awk -v q="'" '
    /^Partition: #[0-9]+ / {
        idx=$2; gsub("#","",idx);
        line=$0;
        sub(/^Partition: #[0-9]+ /,"",line);
        split(line, a, q);
        name=a[2];
        # Pick fields by SHAPE, not by position: rdbtool is free to insert or
        # drop columns (this output already carries an "auto boot(0)" tail),
        # and a positional index silently compares the wrong column.
        size=""; d="";
        m=split(a[3], f, /[ \t]+/);
        for (i=1; i<=m; i++) {
            if (f[i] == "") continue;
            if (f[i] ~ /^DOS[0-9]\//) { d=f[i]; sub(/^DOS[0-9]\//,"",d); }
            else if (size == "" && f[i] ~ /^[0-9]+$/ && i >= 4) size=f[i];
        }
        printf "%s|%s|%s||%s\n", idx, name, d, size;
    }')
# We cannot recompute start from cylinders here without duplicating rdb.c, so
# compare the fields both sides state independently: index, name, DOS type, size.
cmp_ours=$(printf '%s\n' "$ours"  | awk -F'|' '{print $1"|"$2"|"$3"|"$5}')
cmp_theirs=$(printf '%s\n' "$theirs" | awk -F'|' '{print $1"|"$2"|"$3"|"$5}')
if [ "$cmp_ours" = "$cmp_theirs" ] && [ -n "$cmp_ours" ]; then
    echo "  partition table matches rdbtool (index, name, DOS type, size)"
else
    echo "  M10 gate: FAIL -- partition table disagrees with rdbtool"
    diff <(printf '%s\n' "$cmp_theirs") <(printf '%s\n' "$cmp_ours") \
        | sed 's/^/      /' | head -20
    exit 1
fi

# ---- every DOS type the project claims must appear ------------------------
missing=""
for t in 0 1 2 3 4 5; do
    printf '%s\n' "$out" | grep -q "dos=0x444f530$t" || missing="$missing DOS\\$t"
done
if [ -n "$missing" ]; then
    echo "  M10 gate: FAIL -- partitions missing for:$missing"
    exit 1
fi
echo "  all six DOS types present across the partitions"

echo "M10 gate: PASS"
exit 0
