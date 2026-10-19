#!/usr/bin/env bash
# Benchmark one build of fathom on one ITCH file, pinned to a single core.
#
#   scripts/bench.sh data/raw/01302019.NASDAQ_ITCH50
#
# Prints the median wall-clock seconds of three timer-free full-day runs (after one
# throwaway run to warm the page cache), then the sampled apply-latency percentiles
# from one --bench run. CORE picks the CPU (default 2, a P-core thread on the
# i9-14900HX under WSL2, where CPUs 0-15 are the P-cores' SMT siblings); BIN the binary.
set -euo pipefail
file=${1:?usage: scripts/bench.sh <itch file>}
core=${CORE:-2}
bin=${BIN:-build/fathom}

taskset -c "$core" "$bin" "$file" > /dev/null  # warm the page cache
for _ in 1 2 3; do
    taskset -c "$core" "$bin" "$file" | sed -n 's/.* in \([0-9.]*\) s .*/\1/p'
done | sort -n | sed -n '2s/.*/median wall: & s/p'
taskset -c "$core" "$bin" "$file" --bench | grep '^apply latency'
