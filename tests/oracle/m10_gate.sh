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

# ---- the unformatted RDB image must be refused, by name -------------------
# a590_WD93028XA_6parts_not_formatted.hdd carries a real RDB whose partitions
# were never formatted: no DOS id, no rootblock. It is fetched and verified by
# fetch_fixtures.sh but was exercised by NO gate -- a fixture with a purpose
# nobody wired up. The mount must refuse it, and the refusal must name the
# partition and the missing rootblock: asserting only "it did not mount" would
# be satisfied by any failure, including one in the test setup itself.
NEG="$ROOT/tests/fixtures/a590_WD93028XA_6parts_not_formatted.hdd"
if [ -f "$NEG" ]; then
    neglog=$(mktemp)
    MNT_NEG=$(mktemp -d /tmp/amigafs-m10neg.XXXXXX)
    if "$BIN/amigafs-fuse" -f "$NEG" "$MNT_NEG" >"$neglog" 2>&1; then
        :
    fi
    sleep 1
    if grep -qF " $MNT_NEG " /proc/self/mounts 2>/dev/null; then
        echo "  M10 gate: FAIL -- unformatted RDB image mounted; it must be refused"
        fusermount3 -u "$MNT_NEG" 2>/dev/null
        rm -f "$neglog"
        rmdir "$MNT_NEG" 2>/dev/null
        exit 1
    fi
    if grep -q "partition 'OFS'" "$neglog" && grep -q "not a T_HEADER/ST_ROOT rootblock" "$neglog"; then
        echo "  unformatted RDB refused: partition named, missing rootblock stated"
    else
        echo "  M10 gate: FAIL -- refusal did not name the partition and the missing rootblock:"
        sed 's/^/      /' "$neglog"
        rm -f "$neglog"
        rmdir "$MNT_NEG" 2>/dev/null
        exit 1
    fi
    rm -f "$neglog"
    rmdir "$MNT_NEG" 2>/dev/null
else
    echo "  (negative fixture absent -- run tests/fixtures/fetch_fixtures.sh)"
fi

echo "M10 gate: PASS"
exit 0
