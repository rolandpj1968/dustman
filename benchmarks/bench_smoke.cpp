#include <benchmark/benchmark.h>

#include "dustman/dustman.hpp"

static void BM_EmptyLoop(benchmark::State& state) {
  int x = dustman::version_major;
  for (auto _ : state) {
    benchmark::DoNotOptimize(x = x + 1);
  }
}
BENCHMARK(BM_EmptyLoop);

BENCHMARK_MAIN();
