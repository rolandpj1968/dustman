#!/usr/bin/env bash
# Run dustman benchmarks with CPU pinning and Google Benchmark aggregation.
#
# Environment overrides:
#   BUILD=<dir>           build directory (default: build-release)
#   CPU_LIST=<list>       taskset CPU list (default: 0-5)
#   GOVERNOR=performance  set performance governor for the run (requires sudo)

set -euo pipefail

BUILD=${BUILD:-build-release}
BENCH="$BUILD/benchmarks/dustman_bench"

if [[ ! -x "$BENCH" ]]; then
  cat >&2 <<EOF
benchmark not built at $BENCH; configure and build first:
  cmake -S . -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build $BUILD
EOF
  exit 1
fi

# AMD Ryzen 5 6600H default: 0-5 = 3 physical cores (+ HT siblings), leaving
# 6-11 free for browser / system. Override via CPU_LIST=... for other boxes.
CPU_LIST=${CPU_LIST:-0-5}

if [[ "${GOVERNOR:-}" == "performance" ]]; then
  echo ">> setting performance governor on cpus $CPU_LIST (sudo)" >&2
  sudo cpupower -c "$CPU_LIST" frequency-set -g performance >/dev/null
  trap 'sudo cpupower -c "$CPU_LIST" frequency-set -g powersave >/dev/null || true' EXIT
fi

echo ">> taskset -c $CPU_LIST $BENCH" >&2
taskset -c "$CPU_LIST" "$BENCH" \
  --benchmark_min_time=0.5s \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true \
  "$@"
