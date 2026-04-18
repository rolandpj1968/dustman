# Target Platform and Language Standard

This document records the platform, compiler, and language-standard decisions for dustman, and the rationale behind each. It exists so future contributors can re-open a decision knowing what was considered and why — not to relitigate, but to evaluate new evidence against the original reasoning.

## C++ standard: C++17

Dustman commits to a **C++17 floor** for all public headers and the compiled library. Consumers must be able to compile under C++17 or later.

### What C++17 gives us that we need

- **`if constexpr`** — compile-time branching inside templated tracer expansion.
- **Fold expressions** — `FieldList<T, Ms...>::trace` becomes `(v.visit(obj.*Ms), ...)`: one line, no recursion.
- **`inline` variables** — header-only global state (type registries, singletons) without Meyers-singleton gymnastics and their initialisation-order edge cases.
- **Incidental conveniences** — `std::optional`, `std::variant`, `std::string_view`, structured bindings, `std::byte`. Nice to have without depending on.

### What we lose by not requiring C++20

| C++20 feature | Our workaround |
|---|---|
| Concepts (`Tracer<T>`, `Traceable<T>`) | SFINAE + `std::void_t` helpers; slightly uglier compile errors, same constraint semantics. |
| `<bit>` (popcount, countl_zero) | `dustman::bitops` wrapping `__builtin_popcount` / `__builtin_clz` on GCC/Clang, portable fallback elsewhere. |
| `std::span` | `dustman::span<T>` — a ~20-line wrapper over pointer + length. |
| Ranges | Iterators and plain loops. Not load-bearing for us. |
| Coroutines | Speculatively interesting for incremental marking. Not needed yet. |

None are on the critical path. The ergonomic gap between C++17 and C++20 is narrow.

### Why C++11 was rejected despite being feasible

The C++11-vs-C++17 gap is substantially larger in practical terms than C++17-vs-C++20:

- `FieldList<T, Ms...>::trace` requires recursive template chains (`trace_impl<Head, Tail...>`) instead of a fold expression. Compile errors from the recursive path are significantly harder to read.
- Every `if constexpr` site becomes SFINAE + tag dispatch. The implementation roughly doubles in size and grows hostile to non-expert readers.
- Header-only global state requires function-local statics (Meyers singleton), with its own initialisation-order and thread-safety considerations even in C++11.
- No `inline` variables means every header-declared global needs a linkage trick.

The **consumer-side gain** of requiring C++11 instead of C++17 is small. Any C++ codebase actively maintained in 2026 able to consume a modern library is overwhelmingly already on C++14 or C++17.

### Why C++03 is infeasible

C++03 is not a viable target for dustman:

- **No `std::atomic`, `std::memory_order`.** A concurrent precise GC cannot be implemented without these. Hand-rolled platform atomics (inline assembly per architecture) would be the only substitute.
- **No `std::thread`.** The collector thread would require raw platform APIs (`pthread_create`, `CreateThread`, etc.) wrapped in a portability layer.
- **No rvalue references.** `gc_ptr<T>` could not have a move constructor. Every pass-by-value and return would incur redundant root-list operations.
- **No variadic templates.** `FieldList<T, &T::a, &T::b, &T::c>` would collapse into macro-generated overloads for N fields — a hard cap on N and a far worse API.
- **No `constexpr`.** Compile-time line/block arithmetic becomes runtime, or becomes macros.

This would not be "dustman written in C++03"; it would be a fundamentally different library with a materially worse API and substantially more hand-maintained platform code.

## Compiler targets

The C++17 floor implies the following minimum versions:

| Compiler | Minimum | Released |
|----------|---------|----------|
| GCC      | 7       | 2017-05  |
| Clang    | 5       | 2017-09  |
| MSVC     | 2017 15.7 (`/std:c++17`) | 2018-05 |

### CI matrix

Primary CI runs on **current Clang** and **current GCC** with the full sanitiser matrix (ASan, UBSan, and TSan once concurrent marking lands). The C++17 floor is verified periodically on the minimum compiler versions to prevent accidental uses of post-C++17 features slipping into headers.

MSVC support is added when Windows enters the platform matrix.

## Target platforms

### Currently supported

- **Linux x86-64** — primary development and test target (current dev box is AMD x86-64).
- **Linux arm64** — secondary target, brought online after the build/test matrix stabilises on x86-64.

### Explicitly deferred

- **macOS (x86-64 and arm64).** Signal handling, thread suspension, and memory-protection APIs differ enough from Linux that bringing up the platform-specific layer is a project in itself. Page size on arm64 Macs is 16 KB, which affects block sizing assumptions. Deferred until a consumer requires it.
- **Windows.** Same platform-layer argument as macOS, plus MSVC-specific toolchain differences (no `__builtin_*` intrinsics, different calling conventions, structured exception handling as a potential safepoint mechanism). Deferred.

### Explicitly unsupported

- **32-bit platforms.** Dustman assumes 64-bit pointers and 64-bit words throughout: block/line addressing, mark bitmap sizing, atomic operations, and `std::uintptr_t` arithmetic. Retrofitting 32-bit support would touch every core data structure.
- **Big-endian architectures.** Bit-twiddling in mark bitmaps and the object header encoding assumes little-endian layout. All currently-supported targets are little-endian (x86-64, arm64 in default mode).
- **Non-x86-64 / non-arm64 ISAs.** PowerPC, RISC-V, SPARC, etc. are not targeted. Stack scanning and safepoint implementation are ISA-specific and outside current scope.

## Word, alignment, and page assumptions

Baked into the design:

- **64-bit pointers** and 64-bit `uintptr_t`.
- **8-byte minimum object alignment**, with higher alignment available via allocator request.
- **64-byte cache lines.** Correct on x86-64 and arm64 (default). On platforms with 128-byte cache lines (some server ARM microarchitectures), padding structures will be oversized — correctness preserved, some memory wasted.
- **Page size of 4 KB or 16 KB.** Block sizing is computed at runtime via `sysconf(_SC_PAGESIZE)` or the platform equivalent; block dimensions are expressed as integer multiples of the page size, not as absolute byte counts.

## NUMA and multi-socket

The current design is **single-socket aware only**. Block allocation from the global pool does not attempt NUMA affinity. Multi-socket NUMA support is known future work; it will require:

- NUMA-aware block pool: per-node sub-pools, preferential refill from the local node.
- NUMA-aware TLAB refill: a thread's new blocks come from its current node.
- Possibly per-node collector threads for parallel marking.

Not blocking for current scope; called out so the design does not foreclose it.

## Re-opening these decisions

This document is versioned. A change to any of these choices (language standard, supported platforms, word/alignment assumptions) is a design event, not a commit message: update this document with the new decision and its rationale, and cross-link the commit or PR that implements the change.
