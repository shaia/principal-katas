#!/usr/bin/env bash
# Build and measure the C++ track on a Linux machine, and bundle what it saw.
#
# Written for the follow-up's hardware: a 64-core, two-socket server. Run it from
# the kata directory (the one holding cpp/):
#
#   cpp/remote/run.sh [--smt-off] [--phase6-reps N] [--pin-ab N] [--skip-install] [--quick]
#
#   --smt-off        turn SMT off first, so that one logical CPU is one core. On a
#                    bare-metal c6i.metal that leaves 64 cores across 2 sockets.
#   --phase6-reps N  extra runs of phases 0 and 6 after the two full runs (default 6).
#                    Tails here need repetition: one run can rank the arms either way.
#   --pin-ab N       instead of the above: N pairs of phase 6, workers placed by the OS
#                    and pinned (KEYSTONE_PIN=1), alternating which goes first, after one
#                    discarded warm-up run.
#   --skip-install   do not apt-get the toolchain (it is already there).
#   --quick          build, then phases 0 and 2 only: a trial of the script, not a measurement.
#
# Writes everything to ./remote-out/ and bundles it as ./remote-out.tgz. The exit
# code is nonzero if any run's checks failed; the logs say which.
set -uo pipefail

smt_off=0 reps=6 install=1 quick=0 pin_ab=0
while [ $# -gt 0 ]; do
    case "$1" in
    --smt-off) smt_off=1 ;;
    --phase6-reps) reps="$2"; shift ;;
    --pin-ab) pin_ab="$2"; shift ;;
    --skip-install) install=0 ;;
    --quick) quick=1 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

[ -d cpp ] || { echo "run this from the kata directory, the one holding cpp/" >&2; exit 2; }
out="$PWD/remote-out"
rm -rf "$out" && mkdir -p "$out"
say() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$out/progress.log"; }

if [ "$install" = 1 ]; then
    say "installing the toolchain"
    sudo apt-get update -qq >/dev/null
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq clang cmake ninja-build numactl >/dev/null
fi

if [ "$smt_off" = 1 ]; then
    if [ -w /sys/devices/system/cpu/smt/control ] || sudo test -e /sys/devices/system/cpu/smt/control; then
        say "turning SMT off"
        echo off | sudo tee /sys/devices/system/cpu/smt/control >/dev/null
    else
        say "SMT control not available here; leaving it as it is"
    fi
fi
# The fastest steady clock the cores will hold, where the OS lets us ask for it.
if ls /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor >/dev/null 2>&1; then
    echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null 2>&1 || true
fi

say "recording the machine"
{
    echo "== uname";      uname -a
    echo "== lscpu";      lscpu
    echo "== numactl -H"; numactl -H 2>&1 || echo "numactl unavailable"
    echo "== online";     cat /sys/devices/system/cpu/online
    echo "== smt";        cat /sys/devices/system/cpu/smt/control 2>/dev/null || echo "n/a"
    echo "== clocksource"; cat /sys/devices/system/clocksource/clocksource0/current_clocksource
    echo "== governor";   cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "n/a"
    echo "== tsc flags";  grep -o -w -E 'constant_tsc|nonstop_tsc|tsc_reliable' /proc/cpuinfo | sort | uniq -c
    echo "== compiler";   (clang++ --version || g++ --version) 2>&1 | head -1
    echo "== instance";   cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null || echo "n/a"
} >"$out/machine.txt" 2>&1

cxx=$(command -v clang++ || command -v g++)
say "building with $cxx"
cmake -S cpp -B cpp/build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$cxx" >"$out/build.log" 2>&1 &&
    cmake --build cpp/build-linux >>"$out/build.log" 2>&1 || { say "build failed"; tail -20 "$out/build.log"; exit 1; }
bin="$PWD/cpp/bin/solution"

failed=0
run() {  # run <log name> [args...]
    local name=$1; shift
    say "run $name: $*"
    local t0=$SECONDS
    "$bin" "$@" >"$out/$name.log" 2>&1
    local rc=$?
    say "  $name: exit $rc in $((SECONDS - t0)) s"
    [ $rc -eq 0 ] || failed=1
}

if [ "$quick" = 1 ]; then
    run quick-0-2 --phase 0,2
elif [ "$pin_ab" -gt 0 ]; then
    # Placement: the same binary, OS-placed (A) and pinned (B), in pairs whose
    # order alternates, so neither side is always the first of its pair.
    run warmup --phase 0
    for i in $(seq 1 "$pin_ab"); do
        if [ $((i % 2)) = 1 ]; then
            KEYSTONE_PIN=0 run "placed-$i" --phase 0,6
            KEYSTONE_PIN=1 run "pinned-$i" --phase 0,6
        else
            KEYSTONE_PIN=1 run "pinned-$i" --phase 0,6
            KEYSTONE_PIN=0 run "placed-$i" --phase 0,6
        fi
    done
else
    # Run twice and read the second: a freshly linked binary's first run is slower.
    run full-1
    run full-2
    for i in $(seq 1 "$reps"); do run "phase6-$i" --phase 0,6; done
    # The tracing's own check: phase 6 with stall tracing off, interleaved with nothing else.
    KEYSTONE_STALL_TRACE=0 run phase6-untraced-1 --phase 6
    KEYSTONE_STALL_TRACE=0 run phase6-untraced-2 --phase 6
fi

tar -czf "$PWD/remote-out.tgz" -C "$PWD" remote-out
say "done; failed=$failed; bundle at $PWD/remote-out.tgz"
touch "$out/DONE"
exit $failed
