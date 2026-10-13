#!/usr/bin/env bash
# Fetch and verify amigafs test fixtures against fixtures.lock.
#
# Images are downloaded at test time and are gitignored; we never commit or
# redistribute them. Every image is verified by sha256 and a mismatch is a hard
# error (a corrupted or swapped fixture silently invalidates every oracle diff).
#
# fixtures.lock is parsed in ONE awk pass into a TSV of: name, sha, gz(0|1), role.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCK="$HERE/fixtures.lock"

SRC=$(awk 'BEGIN{s=0} /^\[source\.adflib-dumps\]/{s=1} s && /^url[[:space:]]*=/{print $3; exit}' "$LOCK")
ADFLIB_COMMIT=$(awk 'BEGIN{o=0} /^\[oracle\.adflib\]/{o=1} o && /^commit[[:space:]]*=/{print $3; exit}' "$LOCK")
[[ -n "$SRC" && -n "$ADFLIB_COMMIT" ]] || { echo "error: could not parse source url/commit from $LOCK" >&2; exit 2; }

# TSV: name<TAB>sha<TAB>gz<TAB>role
MANIFEST=$(
  awk '
    function flush() { if (name != "") printf "%s\t%s\t%d\t%s\n", name, sha, gz, role }
    /^\[name = / { flush(); name=$0; sub(/^\[name = /,"",name); sub(/\]$/,"",name); sha=""; gz=0; role="" }
    /^sha256 = /  { sha=$3 }
    /^gz = true/   { gz=1 }
    /^gz *= *true/ { gz=1 }
    /^role *=/     { role=$0; sub(/^role *= */,"",role); gsub(/[ \t].*/,"",role) }
    END { flush() }
  ' "$LOCK"
)

n=$(printf '%s\n' "$MANIFEST" | grep -c .)
echo "source:  $SRC"
echo "oracle:  ADFlib @ $ADFLIB_COMMIT"
echo "fixtures: $n"
[[ "$n" -gt 0 ]] || { echo "error: no fixtures parsed from $LOCK" >&2; exit 2; }

fail=0
while IFS=$'\t' read -r name want gz role; do
    [[ -n "$name" ]] || continue
    if [[ ! -f "$HERE/$name" ]]; then
        remote="$name"; [[ "$gz" == "1" ]] && remote="$name.gz"
        echo "  fetching $remote"
        if ! curl -sSLf -o "$HERE/.dl.tmp" "$SRC/$remote"; then
            echo "  ERROR: download failed: $remote" >&2
            rm -f "$HERE/.dl.tmp"; fail=1; continue
        fi
        if [[ "$gz" == "1" ]]; then
            got=$(gunzip -c "$HERE/.dl.tmp" | sha256sum | cut -d' ' -f1)
            if [[ "$got" != "$want" ]]; then
                echo "  ERROR: decompressed sha mismatch for $name" >&2
                echo "         expected $want" >&2
                echo "         actual   $got" >&2
                rm -f "$HERE/.dl.tmp"; fail=1; continue
            fi
            gunzip -c "$HERE/.dl.tmp" > "$HERE/$name"
            rm -f "$HERE/.dl.tmp"
            echo "  expanded $name"
        else
            mv "$HERE/.dl.tmp" "$HERE/$name"
        fi
    fi
    got=$(sha256sum "$HERE/$name" | cut -d' ' -f1)
    if [[ "$got" == "$want" ]]; then
        printf '  OK   %-40s [%s]\n' "$name" "${role:-?}"
    else
        echo "  FAIL $name" >&2
        echo "       expected $want" >&2
        echo "       actual   $got" >&2
        fail=1
    fi
done <<< "$MANIFEST"

if [[ $fail -ne 0 ]]; then
    echo "fixture verification FAILED" >&2
    exit 1
fi
echo "all fixtures verified"