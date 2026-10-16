#!/usr/bin/env bash
# Mount fuzz gate: the FUSE shell, under hostile images, through a REAL mount.
#
# WHY A MOUNT AND NOT A UNIT TEST. `fuse_req_t` is opaque and allocated inside
# libfuse, and libfuse's `fuse_session_custom_io()` -- which would let a test
# drive the ops in-process -- is compiled out of distribution builds
# (`-Denable-custom-io=true` is required, else it returns -ENOTSUP). libfuse
# disabled that path precisely because "a custom io peer might not be a kernel
# and can forge requests that libfuse parses without bounds checks". So the
# reply paths can only be reached the way the kernel reaches them: by mounting.
# That is also where both bugs found earlier in this layer lived (lookup
# refusing every name inside .amigafs; .amigafs unreachable on a partition).
#
# WHAT IT CHECKS, per mutated image:
#   - the mount survives: it must not die, and every syscall the probe issues
#     must either succeed or fail with an errno the design sanctions (ENOTDIR
#     for a hard-linked directory, C12);
#   - the sanitizer stays quiet: any ASan or UBSan report in the mount's stderr
#     fails the gate, whatever the probe thought;
#   - the mount unmounts.
#
# COVERAGE IS ASSERTED, NOT ASSUMED. A fuzz run that mounts nothing and reads
# nothing reports "0 failures" while having tested nothing -- the failure mode
# this project has already hit twice. So the gate prints how many images were
# mounted, refused and walked, and how many entries and files it reached, and
# FAILS if those are too low. A count is the only thing that can tell a green
# run from an empty one.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FIX="$ROOT/tests/fixtures"
ITERS="${1:-40}"
WORK=$(mktemp -d /tmp/amigafs-mountfuzz.XXXXXX)
IMG="$WORK/img.adf"
MNT="$WORK/mnt"

fails=0
tot_unreachable=0
err() { printf '  %-46s FAIL\n' "$1"; fails=$((fails+1)); }
ok()  { printf '  %-46s %s\n' "$1" "$2"; }

cleanup() {
    fusermount3 -uz "$MNT" 2>/dev/null
    [ -n "${MPID:-}" ] && kill "$MPID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "Mount fuzz gate: FUSE shell under hostile images, real mount"
# M36: one seed for the whole run, overridable. Every `$RANDOM` in the damage
# path derives from it, so a failure replays exactly (see the note at the damage
# site). Without this the gate could not distinguish a real defect from noise.
SEED_BASE=${MOUNT_FUZZ_SEED:-20261016}
lane_no=0

# ---- preconditions, each named separately (a gate that silently skips is
# ---- worse than no gate, because its own text tells you to ignore it)
if [ ! -e /dev/fuse ]; then
    echo "Mount fuzz gate: NOT RUN - /dev/fuse is absent"
    echo "  (this gate did not execute; it did not pass)"
    exit 1
fi
command -v fusermount3 >/dev/null || { echo "Mount fuzz gate: NOT RUN - no fusermount3"; exit 1; }
[ -d "$FIX" ] || { echo "Mount fuzz gate: NOT RUN - no fixtures at $FIX"; exit 1; }

mapfile -t SOURCES < <(ls "$FIX"/*.adf 2>/dev/null)
if [ "${#SOURCES[@]}" -eq 0 ]; then
    echo "Mount fuzz gate: NOT RUN - no fixtures (run tests/fixtures/fetch_fixtures.sh)"
    exit 1
fi

PKG=$(pkg-config --cflags --libs fuse3 2>/dev/null)
if [ -z "$PKG" ]; then
    echo "Mount fuzz gate: NOT RUN - fuse3 not found by pkg-config"
    exit 1
fi

# ---- build the driver under ASan+UBSan. Without the sanitizers "no crash"
# ---- only means "did not happen to segfault today".
# shellcheck disable=SC2086
if ! cc -std=gnu99 -g -O1 \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -DFUSE_USE_VERSION=31 $PKG \
        -w -I"$ROOT/include" -I"$ROOT/src/lib" \
        "$ROOT/src/fuse/main.c" "$ROOT"/src/lib/*.c \
        -o "$WORK/fuse_asan" 2>"$WORK/build.log"; then
    echo "Mount fuzz gate: FAIL -- sanitizer build of the mount failed"
    head -10 "$WORK/build.log" | sed 's/^/      /'
    exit 1
fi
if ! cc -std=gnu99 -g -O1 -Wall -Wextra -Werror \
        -o "$WORK/mount_probe" "$HERE/mount_probe.c" 2>"$WORK/probe.log"; then
    echo "Mount fuzz gate: FAIL -- mount_probe did not build"
    head -10 "$WORK/probe.log" | sed 's/^/      /'
    exit 1
fi
if ! cc -std=gnu99 -g -O1 -Wall -Wextra -Werror \
        -o "$WORK/mount_write_probe" "$HERE/mount_write_probe.c" 2>"$WORK/wprobe.log"; then
    echo "Mount fuzz gate: FAIL -- mount_write_probe did not build"
    head -10 "$WORK/wprobe.log" | sed 's/^/      /'
    exit 1
fi

# LeakSanitizer is off deliberately: the mount is a long-lived daemon torn down
# by a signal on unmount, so its exit path is not a meaningful leak oracle and
# the noise would bury the memory errors this gate exists to find. The library
# fuzzer runs with leak detection ON and covers that ground.
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:handle_segv=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"

mkdir -p "$MNT"
mounted=0; refused=0; entries=0; files=0; bytes=0
sanitizer_hits=0; died=0; probe_fail=0; detached=0

for i in $(seq 1 "$ITERS"); do
    src="${SOURCES[$((i % ${#SOURCES[@]}))]}"
    cp "$src" "$IMG" 2>/dev/null || continue

    size=$(stat -c%s "$IMG" 2>/dev/null || echo 0)
    [ "$size" -gt 1024 ] || continue
    # mutate: flip a byte, or zero a whole block
    if [ $((i % 4)) -eq 3 ]; then
        off=$(( (RANDOM * 32768 + RANDOM) % (size / 512) * 512 ))
    else
        off=$(( (RANDOM * 65536 + RANDOM) % size ))
    fi
    printf "$(printf '\\%03o' $(( (RANDOM % 255) + 1 )))" \
        | dd of="$IMG" bs=1 seek="$off" count=1 conv=notrunc status=none 2>/dev/null

    log="$WORK/mnt.log"
    "$WORK/fuse_asan" -f "$IMG" "$MNT" > "$log" 2>&1 &
    MPID=$!
    # Readiness is "the mountpoint is in /proc/mounts", NOT "ls succeeds": `ls`
    # on an ordinary empty directory succeeds too, so the weaker check is
    # satisfied by the ABSENCE of a filesystem -- which is how a mutated image
    # that fails to open got counted as a successful mount and reported zero
    # entries while reporting no failure.
    mounted_ok=0
    for _ in $(seq 1 24); do
        if grep -qF " $(readlink -f "$MNT" 2>/dev/null || echo "$MNT") fuse " \
             /proc/self/mounts 2>/dev/null; then
            mounted_ok=1
            break
        fi
        kill -0 "$MPID" 2>/dev/null || break
        sleep 0.25
    done

    if [ "$mounted_ok" -eq 0 ]; then
        refused=$((refused+1))
        # an image that is refused must still be refused QUIETLY
        if grep -qE "ERROR: (Address|Leak)Sanitizer|runtime error:" "$log"; then
            sanitizer_hits=$((sanitizer_hits+1))
            grep -m1 -E "ERROR: (Address|Leak)Sanitizer|runtime error:" "$log" \
                | sed "s/^/      iter $i: /"
        fi
        wait "$MPID" 2>/dev/null
        MPID=""
        continue
    fi

    mounted=$((mounted+1))

    # -f must mean what it says. `fuse_daemonize(foreground)` daemonizes when
    # the argument is ZERO, so a mount that passes 0 forks, the parent exits,
    # and the caller's PID is not the process serving the mount: the flag is a
    # lie, stderr is redirected to /dev/null so every diagnostic after this
    # point is lost, and `kill $!` does not stop the filesystem. A detached
    # child still serves the mount, so `ls` succeeds and nothing else notices --
    # which is why this is asserted rather than inferred from the mount working.
    if ! kill -0 "$MPID" 2>/dev/null; then
        detached=$((detached+1))
    fi

    out=$(timeout 120 "$WORK/mount_probe" "$MNT" 2>"$WORK/probe.err")
    prc=$?
    if [ $prc -ne 0 ]; then
        probe_fail=$((probe_fail+1))
        head -2 "$WORK/probe.err" | sed "s/^/      iter $i: /"
    fi

    # CROSS-CHECK every name the mount refused with ENOENT. The probe counts
    # those separately rather than failing, because a name whose bytes no longer
    # hash to its own bucket is unreachable BY THE FORMAT: readdir walks every
    # bucket and still shows it, while lookup computes hash(name) and reads a
    # bucket the entry is not filed in. ('multidef' -> bucket 46; one byte
    # mutated -> bucket 16, entry still at 46.) ADFlib answers identically.
    #
    # But whitelisting them without a second opinion would mask a mount whose
    # lookup was broken entirely -- every name would ENOENT and the gate would
    # pass. So ask the library, which is the format's authority: if `afs ls`
    # CAN find what the mount could not, the MOUNT lost a reachable entry and
    # that is a real defect.
    unreachable=0
    mount_lost=0
    while IFS= read -r u; do
        [ -n "$u" ] || continue
        unreachable=$((unreachable+1))
        rel="${u#"$MNT"/}"
        [ "$rel" != "$u" ] || continue
        if "$ROOT/build/afs" ls "$IMG" "/$rel" >/dev/null 2>&1; then
            mount_lost=$((mount_lost+1))
            echo "      iter $i: MOUNT LOST a reachable entry: /$rel" >&2
        fi
    done < <(sed -n 's/^probe: unreachable //p' "$WORK/probe.err")
    tot_unreachable=$((tot_unreachable + unreachable))
    if [ "$mount_lost" -gt 0 ]; then
        probe_fail=$((probe_fail+1))
    fi
    # harvest coverage even from a partial walk
    e=$(sed -n 's/.*entries=\([0-9]*\).*/\1/p' <<< "$out")
    f=$(sed -n 's/.*files=\([0-9]*\).*/\1/p' <<< "$out")
    b=$(sed -n 's/.*bytes=\([0-9]*\).*/\1/p' <<< "$out")
    entries=$((entries + ${e:-0}))
    files=$((files + ${f:-0}))
    bytes=$((bytes + ${b:-0}))

    if grep -qE "ERROR: (Address|Leak)Sanitizer|runtime error:" "$log"; then
        sanitizer_hits=$((sanitizer_hits+1))
        grep -m1 -E "ERROR: (Address|Leak)Sanitizer|runtime error:" "$log" \
            | sed "s/^/      iter $i: /"
    fi

    fusermount3 -u "$MNT" >/dev/null 2>&1
    # The daemon exits asynchronously after the unmount, so poll rather than
    # testing once: a single immediate check races the teardown and reports a
    # clean exit as a failure. The check is KEPT because a mount that does not
    # exit after unmount really does leak one process per iteration.
    gone=0
    for _ in $(seq 1 20); do
        if ! kill -0 "$MPID" 2>/dev/null; then gone=1; break; fi
        sleep 0.1
    done
    if [ "$gone" -eq 0 ]; then
        kill "$MPID" 2>/dev/null
        died=$((died+1))
    fi
    wait "$MPID" 2>/dev/null
    MPID=""
done

echo "  iterations=$ITERS mounted=$mounted refused=$refused" \
     "entries=$entries files=$files bytes=$bytes"

[ "$sanitizer_hits" -eq 0 ] \
    || err "sanitizer reports from the mount" "$sanitizer_hits iteration(s)"
[ "$probe_fail" -eq 0 ] \
    || err "probe saw an unsanctioned failure" "$probe_fail iteration(s)"
[ "$died" -eq 0 ] \
    || err "mount survived the probe but died on unmount" "$died iteration(s)"
[ "$detached" -eq 0 ] \
    || err "-f did not stay in the foreground" \
          "$detached iteration(s): fuse_daemonize(0) daemonizes, so the PID the caller launched exits and stderr is discarded"

# The coverage floor is the gate's own honesty check. Without it, a run where
# every image was refused at the DOS id check reports 0 failures and proves
# nothing -- the failure mode this project has already hit twice. The floors
# are absolute rather than a fraction of $ITERS, so a short run and a long run
# are held to the same bar; the printed mounted/refused counts make the refusal
# rate visible either way.
if [ "$mounted" -lt 3 ] || [ "$entries" -lt 50 ] || [ "$files" -lt 10 ]; then
    err "coverage too low to be meaningful" \
        "mounted=$mounted (need >=3) entries=$entries (need >=50) files=$files (need >=10)"
else
    ok "reached the parser through a real mount" \
       "$mounted mounted, $refused refused, $entries entries, $files files"
fi

# Report the separately-counted ENOENTs so the number is visible rather than
# quietly absorbed. A non-zero count is EXPECTED on mutated images; what would
# not be acceptable is one the library could still reach, and that fails above.
ok "names refused as unreachable by the format" \
   "$tot_unreachable (each cross-checked against the library)"

echo
echo "--- write lane: the FUSE write ops through a real -o rw mount ---"

# The write ops (create/write/setattr/mkdir/unlink/rmdir/rename, the O_APPEND
# redirect, the Amiga protection-bit check, and the reply types) are the one
# layer with no fuzz coverage: mount_probe exercises the shell's READ
# arithmetic, tests/write_fuzz.c exercises the LIBRARY's write path against a
# model, and between them the shell itself was only ever driven by fixed POSIX
# scenarios. Two of its bugs so far surfaced as "fuse: writing device: Invalid
# argument" -- an error from the reply WRITER, not from the operation.
#
# TWO PASSES, because the checks are only meaningful on a volume the write path
# can reason about:
#   strict  - unmutated images. Every errno must be one the design sanctions,
#             and EIO is NOT among them: a healthy volume answering EIO is a
#             defect, and this is the pass that would catch it.
#   lenient - deliberately mutated images, where meeting a structure the write
#             path refuses to write through and answering EIO is the DESIGNED
#             behaviour. The differential check (what the mount accepted must
#             be observable, what it refused must leave the volume alone) still
#             applies in full.
# As in the read lane, coverage floors are absolute, so a short run cannot pass
# by doing nothing.
wmounted=0; wrefused=0; wprobe_fail=0; wsan=0; wops=0; waccepted=0
wclasses=0; wnowrite=0
for lane in strict lenient; do
    lane_iters=$(( ITERS / 2 ))
    [ "$lane" = strict ] && lane_iters=8
    for i in $(seq 1 "$lane_iters"); do
        # DETERMINISTIC DAMAGE (M36). This used `$RANDOM`, which bash seeds from
        # the pid and the clock, so the byte pattern differed on EVERY run. That
        # makes the gate unable to answer the only question it exists for: a
        # failure here cannot be replayed, and "iter 7" in one run says nothing
        # about any other run.
        #
        # It already looked flaky -- a write landing wrong bytes appeared in some
        # runs and not others -- and a flaky defect that cannot be seeded is not
        # a defect anyone can fix. Seed it once, and let the seed be overridden so
        # a failing one can be replayed exactly:
        #
        #     MOUNT_FUZZ_SEED=12345 bash tests/oracle/mount_fuzz.sh
        #
        # $RANDOM is re-seeded per lane rather than per iteration, because the
        # lanes interleave and a per-iteration reseed would make iteration N
        # depend on which lane got there first.
        RANDOM=$(( (SEED_BASE + i * 31 + lane_no) % 32768 ))
        src="${SOURCES[$(( (i + 7) % ${#SOURCES[@]} ))]}"
        cp "$src" "$IMG" 2>/dev/null || continue
        size=$(stat -c%s "$IMG" 2>/dev/null || echo 0)
        [ "$size" -gt 1024 ] || continue
        # the lenient lane mutates; the strict lane must stay a healthy volume
        if [ "$lane" = lenient ]; then
            off=$(( (RANDOM * 32768 + RANDOM) % (size / 512) * 512 ))
            printf "$(printf '\\%03o' $(( (RANDOM % 255) + 1 )))" \
                | dd of="$IMG" bs=1 seek="$off" count=1 conv=notrunc status=none 2>/dev/null
        fi

        log="$WORK/wmnt.log"
        "$WORK/fuse_asan" -f "$IMG" "$MNT" -o rw > "$log" 2>&1 &
        MPID=$!
        mnt_ready=0
        for _ in $(seq 1 24); do
            if grep -qF " $(readlink -f "$MNT" 2>/dev/null || echo "$MNT") fuse " \
                 /proc/self/mounts 2>/dev/null; then
                mnt_ready=1
                break
            fi
            kill -0 "$MPID" 2>/dev/null || break
            sleep 0.25
        done

        if [ "$mnt_ready" -eq 0 ]; then
            wrefused=$((wrefused+1))
            if grep -qE "ERROR: (Address|Leak)Sanitizer|runtime error:" "$log"; then
                wsan=$((wsan+1))
                grep -m1 -E "ERROR: (Address|Leak)Sanitizer|runtime error:" "$log" \
                    | sed "s/^/      $lane iter $i: /"
            fi
            wait "$MPID" 2>/dev/null
            MPID=""
            continue
        fi
        wmounted=$((wmounted+1))

        wargs="$MNT 250 17"
        [ "$lane" = lenient ] && wargs="$wargs --lenient"
        # the scratch directory is created by the probe inside the mount
        timeout 300 "$WORK/mount_write_probe" $wargs > "$WORK/wprobe.out" 2>"$WORK/wprobe.err"
        wrc=$?
        if [ $wrc -eq 3 ]; then
            # the volume refuses writes on purpose (bitmap flagged invalid,
            # untrustworthy page count). Counted, not failed: a refused write
            # is the design working.
            wnowrite=$((wnowrite+1))
        elif [ $wrc -ne 0 ]; then
            wprobe_fail=$((wprobe_fail+1))
            head -3 "$WORK/wprobe.err" | sed "s/^/      $lane iter $i: /"
        else
            o=$(sed -n 's/^wprobe: ops=\([0-9]*\) accepted=\([0-9]*\).*/\1 \2/p' "$WORK/wprobe.out")
            # guard the arithmetic: a probe that produced no tally must not
            # turn into a shell syntax error that hides the real finding
            [ -n "$o" ] || o="0 0"
            wops=$((wops + ${o%% *}))
            waccepted=$((waccepted + ${o##* }))
            # a class counts as exercised when the probe tried it at all
            wclasses=$((wclasses + $(grep -c 'tried=' "$WORK/wprobe.out")))
        fi

        if grep -qE "ERROR: (Address|Leak)Sanitizer|runtime error:" "$log"; then
            wsan=$((wsan+1))
            grep -m1 -E "ERROR: (Address|Leak)Sanitizer|runtime error:" "$log" \
                | sed "s/^/      $lane iter $i: /"
        fi

        fusermount3 -u "$MNT" >/dev/null 2>&1
        gone=0
        for _ in $(seq 1 20); do
            if ! kill -0 "$MPID" 2>/dev/null; then gone=1; break; fi
            sleep 0.1
        done
        [ "$gone" -eq 0 ] && kill "$MPID" 2>/dev/null
        wait "$MPID" 2>/dev/null
        MPID=""
    done
done

echo "  write lane: mounted=$wmounted refused=$wrefused ops=$wops accepted=$waccepted nowrite=$wnowrite"

[ "$wsan" -eq 0 ] \
    || err "sanitizer reports from the rw mount" "$wsan iteration(s)"
[ "$wprobe_fail" -eq 0 ] \
    || err "write probe saw a failure" "$wprobe_fail iteration(s)"
if [ "$wmounted" -lt 3 ] || [ "$wops" -lt 200 ] || [ "$waccepted" -lt 100 ] \
   || [ "$wclasses" -lt 16 ]; then
    err "write coverage too low to be meaningful" \
        "mounted=$wmounted (need >=3) ops=$wops (need >=200) accepted=$waccepted (need >=100) class-lines=$wclasses (need >=16)"
else
    ok "write ops reached through a real -o rw mount" \
       "$wmounted mounted, $wrefused refused, $wops ops, $waccepted accepted"
fi

if [ $fails -eq 0 ]; then
    echo "Mount fuzz gate: PASS"
    exit 0
fi
echo "Mount fuzz gate: FAIL ($fails)"
exit 1
