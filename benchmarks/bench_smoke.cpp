#include <benchmark/benchmark.h>

#include "dustman/dustman.hpp"

static void BM_EmptyLoop(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(dustman::version_major);
  }
}
BENCHMARK(BM_EmptyLoop);

BENCHMARK_MAIN();
