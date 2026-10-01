#!/usr/bin/env bash
# Pinned latency benchmark for a Linux machine. Builds the project, records the environment, runs
# ot_bench on one pinned core several times and writes a summary with medians across runs.
#
#   scripts/linux_benchmark.sh [--cpu N] [--runs N] [--messages N] [--itch FILE]
#
# It changes no system settings. For trustworthy tails, set the machine up first (see
# docs/BENCHMARKING.md): performance governor, an isolated core (isolcpus=N nohz_full=N rcu_nocbs=N)
# and nothing else running. The script prints what it finds so the results record the conditions.
set -euo pipefail

cpu=2
runs=5
messages=5000000
itch=""
while [ $# -gt 0 ]; do
    case "$1" in
        --cpu) cpu="$2"; shift 2 ;;
        --runs) runs="$2"; shift 2 ;;
        --messages) messages="$2"; shift 2 ;;
        --itch) itch="$2"; shift 2 ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ "$(uname -s)" = "Linux" ] || { echo "this script is for Linux" >&2; exit 1; }
command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 1; }
command -v taskset >/dev/null || { echo "taskset (util-linux) is required" >&2; exit 1; }

root="$(cd "$(dirname "$0")/.." && pwd)"
host="$(hostname -s)"
out="$root/results/linux_${host}_cpu${cpu}"
mkdir -p "$out"
report="$out/summary.txt"

{
    echo "OptiTrade benchmark, $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host: $host   pinned core: $cpu   runs: $runs   messages per run: $messages"
    echo
    echo "== environment"
    echo "kernel:   $(uname -r)"
    echo "arch:     $(uname -m)"
    lscpu 2>/dev/null | grep -E "Model name|^CPU\(s\)|Thread\(s\) per core|Socket|MHz|L1d|L2|L3|Virtualization|Hypervisor" || true
    echo "governor: $(cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    echo "no_turbo: $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo n/a)"
    echo "cmdline:  $(cat /proc/cmdline 2>/dev/null || echo unknown)"
    if grep -q -E "isolcpus=[^ ]*\b${cpu}\b|isolcpus=.*${cpu}" /proc/cmdline 2>/dev/null; then
        echo "isolated: yes (core $cpu appears in isolcpus)"
    else
        echo "isolated: NO - core $cpu is not isolated; tail latencies will include scheduler noise"
    fi
    echo "load:     $(cut -d' ' -f1-3 /proc/loadavg)"
    echo "compiler: $(${CXX:-c++} --version | head -1)"
    echo
} | tee "$report"

cmake -S "$root" -B "$root/build/bench" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
    || cmake -S "$root" -B "$root/build/bench" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$root/build/bench" --target ot_bench ot_replay_bench -j"$(nproc)" >/dev/null

bench="$root/build/bench/ot_bench"
median() { sort -g | awk '{a[NR]=$1} END{ if (NR==0) {print "n/a"} else if (NR%2) {print a[(NR+1)/2]} else {print (a[NR/2]+a[NR/2+1])/2} }'; }

{
    echo "== ot_bench, closed loop, medians over $runs runs (ns)"
    printf "%-18s %10s %8s %8s %8s %9s %10s\n" strategy ns/msg p50 p99 p99.9 p99.99 max
} | tee -a "$report"
for s in imbalance maker ema; do
    for i in $(seq 1 "$runs"); do
        taskset -c "$cpu" "$bench" --strategy "$s" --messages "$messages" --cpu "$cpu" \
            --csv "$out/${s}_run${i}.csv" > "$out/${s}_run${i}.txt"
    done
    col() { awk -F, -v m="$1" -v c="$2" '$2=="closed" && $3=="service"{print $c}' "$out"/${s}_run*.csv | median; }
    thr() { awk -F, '$2=="throughput"{print $5}' "$out"/${s}_run*.csv | median; }
    printf "%-18s %10s %8s %8s %8s %9s %10s\n" "$s" "$(thr)" "$(col x 6)" "$(col x 8)" "$(col x 9)" "$(col x 10)" "$(col x 11)" | tee -a "$report"
done

{
    echo
    echo "== ot_bench, open loop (service and response time), imbalance strategy, one run per rate"
} | tee -a "$report"
for rate in 1000000 2000000 5000000; do
    taskset -c "$cpu" "$bench" --strategy imbalance --messages "$messages" --rate "$rate" --cpu "$cpu" \
        > "$out/open_${rate}.txt"
    echo "-- offered $rate msg/s" | tee -a "$report"
    grep -E "open-loop (service|response)|starts more than|WARNING|NOTE" "$out/open_${rate}.txt" | tee -a "$report" || true
done

if [ -n "$itch" ]; then
    {
        echo
        echo "== ot_replay_bench on $itch"
    } | tee -a "$report"
    taskset -c "$cpu" "$root/build/bench/ot_replay_bench" --ahead 0,16,32 "$itch" | tee -a "$report"
fi

echo
echo "Done. Everything is in: $out"
echo "Send the folder (or just summary.txt) back to be added to the repository."
