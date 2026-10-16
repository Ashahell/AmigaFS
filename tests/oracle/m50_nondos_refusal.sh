#!/usr/bin/env bash
# m50_nondos_refusal.sh -- pin WHICH FILESYSTEMS ARE REFUSED, AND THAT THEY ARE NAMED.
#
# WHY THIS EXISTS. The plan's last open item on this axis read "Large volumes: a
# 32-bit block count with BSIZE up to 32 KiB stays in range. Refuse ffstd64
# explicitly." Refusing ffstd64 was already implemented -- `read_dos_id` requires
# the descriptor's first three bytes to be exactly "DOS" -- but NOTHING tested it,
# and a note of mine claimed the refusal was "an accident of the encoding". That
# claim was wrong, and reading the code is what showed it: the check is on the
# NAME, deliberately, with a comment saying so.
#
# THE FORMAT FACT THAT IS EASY TO GET WRONG, and that I got wrong twice writing
# this gate. The AmigaDOS filesystem id is `D` `O` `S` followed by ONE BINARY BYTE,
# not the ASCII string "DOS3". The fixtures make it unambiguous:
#
#     testffs.adf   44 4f 53 05     -> DOS\5
#     testofs.adf   44 4f 53 02     -> DOS\2
#     links.adf     44 4f 53 01     -> DOS\1
#
# My first draft wrote the ASCII bytes "DOS6"/"DOS7" to exercise the long-name
# branch. That produced byte 0x36, whose low three bits happen to be 6, so the
# branch was reached and the gate passed -- for the wrong reason, testing an id
# that does not exist. Hence the format fact is ASSERTED FIRST below, before
# anything else in the gate depends on it.
#
# WHY THE MASK IS NOT THE GUARD. `dostype = flags & 0x07` takes the low three bits
# of the descriptor's fourth byte, so a fourth byte in 0..7 masks to a DOS type in
# range. SFS, PFS, IFFS, FS64, DF64 and LNX1 all carry ASCII fourth bytes that mask
# into that range -- all six would be ACCEPTED by the mask alone. They are refused
# because the "DOS" name test runs FIRST. The mask would have been a genuine hole
# without it, so this gate pins both halves rather than one.
#
# AND IT PINS THE NAMING, which is the actual code change. The old message was "no
# DOS filesystem id on block 0 or block 1" -- true, and useless to a user holding an
# SFS volume, because it tells them the disk has no filesystem id when it has one
# and it is simply not an AmigaDOS. That is the shape M44 fixed for an unknown `-o`
# option: do not blame the input, name the cause. Here the two diagnoses send the
# user to entirely different places.
#
# A NOTE ON THE TWO WRITERS below. Three earlier versions of this gate wrote the
# 4-byte tag by formatting hex as text, or by passing it through printf escapes, and
# produced ids like "53 4" and "\xIF" -- which the gate then reported on with total
# confidence, because a probe that writes the wrong bytes does not fail, it
# cheerfully tests the wrong thing. Hence two separate, minimal writers: one takes a
# literal ASCII tag, the other takes a printf OCTAL escape for a single binary byte,
# which bash's printf handles without ambiguity.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures/testffs.adf"
W=$(mktemp -d /tmp/amigafs-m50.XXXXXX)
trap 'rm -rf "$W"' EXIT

fails=0
bad() { printf '  %-52s FAIL (%s)\n' "$1" "$2"; fails=$((fails + 1)); }
note() { printf '  %-52s %s\n' "$1" "$2"; }

[ -x "$ROOT/build/afs" ] || { echo "M50 gate: SKIP -- build/afs not built"; exit 77; }
[ -f "$FIX" ]            || { echo "M50 gate: FAIL -- fixture missing: $FIX"; exit 1; }

echo "=== M50 gate (non-AmigaDOS descriptors are refused, by name) ==="

# --- 0. the format fact, asserted before anything depends on it -------------
id4=$(head -c 4 "$FIX" | od -An -tx1 | tr -s ' ' | sed 's/^ //;s/ $//')
if [ "$id4" = "44 4f 53 05" ]; then
    note "the fixture's id is 'DOS' + binary byte 5" "44 4f 53 05 = DOS\\5, confirmed"
else
    bad "the fixture's id is 'DOS' + one binary byte" "got: $id4"
    echo "M50 gate: FAIL (the format fact this gate rests on is unconfirmed)"
    exit 1
fi

# The control: a real volume must still be READ. If this breaks, every refusal
# below is vacuous -- which is exactly how the mask-only mutant surfaced.
if "$ROOT/build/afs" ls "$FIX" >"$W/ok.txt" 2>&1; then
    note "a real DOS\\5 volume is still READ" "$(wc -l < "$W/ok.txt" | tr -d ' ') entries"
else
    bad "a real DOS\\5 volume is still READ" "$(head -1 "$W/ok.txt")"
    echo "M50 gate: FAIL (control broken -- the refusals below would be vacuous)"
    exit 1
fi

# $1 = literal 4-character ASCII tag, written verbatim
probe_tag() {
    cp "$FIX" "$W/p.adf"
    printf '%s' "$1" | dd of="$W/p.adf" bs=1 seek=0 count=4 conv=notrunc status=none
    "$ROOT/build/afs" ls "$W/p.adf" 2>&1 | head -1
}
# $1 = a printf format producing ONE binary byte, e.g. '\006'
probe_dos() {
    cp "$FIX" "$W/p.adf"
    printf 'DOS' | dd of="$W/p.adf" bs=1 seek=0 count=3 conv=notrunc status=none
    printf "$1" | dd of="$W/p.adf" bs=1 seek=3 count=1 conv=notrunc status=none
    "$ROOT/build/afs" ls "$W/p.adf" 2>&1 | head -1
}

# --- 1. non-AmigaDOS tags, each of which masks to an ACCEPTED DOS type --------
for d in SFS1 PFS1 IFFS FS64 DF64 LNX1 FFS3; do
    masked=$(python3 -c "print(ord('$d'[3]) & 0x07)")
    out=$(probe_tag "$d")
    if printf '%s' "$out" | grep -q "is not an AmigaDOS filesystem"; then
        if printf '%s' "$out" | grep -q "'$d'"; then
            note "'$d' refused AND named (4th byte masks to DOS\\$masked)" "'$d' is not an AmigaDOS filesystem"
        else
            bad "'$d' refused AND named" "refused but the message omits the id: $out"
        fi
    else
        bad "'$d' is refused" "$out"
    fi
done

# --- 2. DOS\6 / DOS\7 reach a DIFFERENT branch, with their own message ---------
# These are genuine AmigaDOS types we do not support, and the message that says WHY
# is the long-name one. If the branches ever merge, a user with a long-name volume
# would be told they have the wrong FILESYSTEM, which is wrong.
for t in 6 7; do
    case $t in 6) esc='\006' ;; 7) esc='\007' ;; esac
    out=$(probe_dos "$esc")
    if printf '%s' "$out" | grep -q "long-filename types"; then
        note "DOS\\$t refused with the LONG-NAME reason" \
             "$(printf '%s' "$out" | sed 's/afs: //' | cut -c1-36)"
    else
        bad "DOS\\$t refused with the long-name reason" "$out"
    fi
done

# --- 3. a descriptor that is NOT printable keeps the original wording ---------
# Naming the id must never print four control characters or a NUL into a message.
cp "$FIX" "$W/z.adf"
dd if=/dev/zero of="$W/z.adf" bs=512 count=2 conv=notrunc status=none
zout=$("$ROOT/build/afs" ls "$W/z.adf" 2>&1 | head -1)
if printf '%s' "$zout" | grep -q "no DOS filesystem id on block 0 or block 1"; then
    note "a zeroed bootblock keeps the original wording" "no id, so no id is invented"
else
    bad "a zeroed bootblock keeps the original wording" "$zout"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "M50 gate: PASS (7 non-AmigaDOS tags named, 2 long-name types kept, control reads)"
    exit 0
fi
echo "M50 gate: FAIL ($fails)"
exit 1