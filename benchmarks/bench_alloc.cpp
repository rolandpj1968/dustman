#include <memory>

#include <benchmark/benchmark.h>

#include "dustman/alloc.hpp"
#include "dustman/tracer.hpp"

namespace {

struct Small {
  int a;
  int b;
};

} // namespace

template <>
struct dustman::Tracer<Small> : dustman::FieldList<Small> {};

static void BM_DustmanAlloc(benchmark::State& state) {
  for (auto _ : state) {
    auto p = dustman::alloc<Small>();
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_DustmanAlloc)->Iterations(1'000'000);

static void BM_NewOnly(benchmark::State& state) {
  for (auto _ : state) {
    auto* p = new Small {};
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_NewOnly)->Iterations(1'000'000);

static void BM_NewDelete(benchmark::State& state) {
  for (auto _ : state) {
    auto* p = new Small {};
    benchmark::DoNotOptimize(p);
    delete p;
  }
}
BENCHMARK(BM_NewDelete);

static void BM_MakeShared(benchmark::State& state) {
  for (auto _ : state) {
    auto p = std::make_shared<Small>();
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_MakeShared);
