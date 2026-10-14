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
err() { printf '  %-46s FAIL\n' "$1"; fails=$((fails+1)); }
ok()  { printf '  %-46s %s\n' "$1" "$2"; }

cleanup() {
    fusermount3 -uz "$MNT" 2>/dev/null
    [ -n "${MPID:-}" ] && kill "$MPID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "Mount fuzz gate: FUSE shell under hostile images, real mount"

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

if [ $fails -eq 0 ]; then
    echo "Mount fuzz gate: PASS"
    exit 0
fi
echo "Mount fuzz gate: FAIL ($fails)"
exit 1
