# Dustman Design

This document captures the core design decisions behind dustman — what it is, why the architecture is shaped the way it is, and what constraints the API has to honour. It is rationale-heavy by design; concrete signatures will live in `docs/api.md` once settled.

## Goals and non-goals

### Goals

- **Precise** garbage collection. The collector knows exactly which words are pointers. This is non-negotiable — it unlocks object movement (compaction, evacuation), eliminates false retention, and makes scanning cheap.
- **Moving** collection. Objects can be relocated during collection for defragmentation and cache locality. Consumer pointers are updated transparently.
- **Generational-capable.** Design must accommodate a nursery/tenured split without API churn.
- **General-purpose.** Usable by any C++ application willing to buy in to the tracing contract. Not specialised to any one language runtime or host.
- **Low allocation cost.** Bump-pointer allocation from thread-local buffers. The fast path is inlined into consumer code.
- **Incremental / concurrent-capable.** Design must not foreclose moving the mark phase (and eventually sweep) off the mutator thread.

### Non-goals

- **Conservative scanning.** No stack scanning by pattern matching; no `malloc`-compatible drop-in. Precision requires consumer participation.
- **Zero-overhead opt-out.** Consumers who want reference-counted or manual memory keep using what they have. Dustman is for code willing to commit to tracing.
- **Fully automatic for arbitrary C++ today.** Some deliberate tracer code is required per type. See [Precise tracing contract](#precise-tracing-contract).

## Target platform and language

Dustman targets **C++17** as its language floor, on **Linux x86-64** as the primary platform with Linux arm64 as a secondary target. macOS and Windows are explicitly deferred until a consumer needs them. 32-bit platforms and big-endian architectures are unsupported.

The full rationale — why C++17 specifically (rather than C++11 or C++20), which compiler versions, which architectures, and what assumptions about word size, alignment, and page size are baked into the design — is captured in [`docs/platform.md`](platform.md). That document is the reference when a platform or standard decision needs to be reopened.

## Library architecture

Dustman is a **hybrid**: header-only for the hot-path and templated surface, with a linked `.cpp` backend for global state and platform-specific code. Consumers `#include <dustman/dustman.hpp>` and link a compiled archive (`libdustman.a` or the shared equivalent).

**In headers:**

- `gc_ptr<T>` and all smart-pointer machinery
- `alloc<T>(...)` fast path (bump-pointer allocation from the thread-local buffer)
- Write barriers and safepoint polls
- `Tracer<T>` trait and `Visitor` interface
- Anything templated on consumer types

**In the compiled backend:**

- Global heap: block pool, line metadata, generational state
- Slow-path allocation: TLAB refill, block acquisition
- Mark / sweep / evacuate implementations
- Collector thread
- Platform-specific primitives: stack scanning, signal handling, `mprotect` / `madvise`, thread suspend

### Why hybrid, not pure header-only

The allocation fast path, write barrier, and safepoint poll are each a handful of instructions. A function-call boundary is a real fraction of their cost, so these must inline across translation units — they live in headers.

The reverse is equally true for global state and platform code. Shipping signal handlers, `mprotect` calls, and stack-scanning assembly to every consumer via headers is impractical: it forces the consumer's build to pick up platform-specific flags, multiplies compile time, and makes the heap and collector thread's ownership ambiguous.

The split is invisible to consumers — same `#include`, plus one linked archive.

### Why hybrid, not pure compiled library

A GC whose allocation path crosses a function-call boundary surrenders most of its performance advantage over `make_shared`. LTO can sometimes recover inlining, but relying on it is a bad bet. Templates for the public API (`gc_ptr<T>`, `alloc<T>`) end up in headers regardless, so a "pure" compiled library is not actually an option — it's just a worse version of the hybrid.

## Precise tracing contract

Precise collection means the GC knows exactly which words in an object are pointers, and which roots (stack slots, globals, and registers) point at GC-managed objects. Precision is what allows the GC to move objects, skip false retention, and generate tight mark loops.

C++ provides no portable way to discover an arbitrary type's pointer-typed members at compile time (today). Consequently **the consumer must describe its object graph to dustman**. This is the contract.

Per type, the consumer provides a **tracer**: a function that enumerates outgoing references by calling `visitor.visit(field)` for each. Dustman's marker and evacuator invoke the tracer whenever it needs to scan an object of that type.

### Why not auto-discovery today?

| Mechanism | Verdict |
|---|---|
| **Boost.PFR / aggregate decomposition** | Works only for trivial aggregates. No user constructors, no private fields, no non-trivial bases. Too restrictive for real types. |
| **Member-pointer registration** (`FieldList<T, &T::a, &T::b>`) | Yes — gives most of the ergonomic win of reflection, but still needs one deliberate line per type. Adopted as the ergonomic default. |
| **Construction-time self-registration** (each `gc_ptr` announces itself to its parent) | Expensive per-allocation. Breaks under object movement. Rejected. |
| **Runtime debug-info scanning** | Fragile, platform-specific, not fast-path-friendly. Rejected. |
| **C++26 static reflection** | Genuinely automatic. Not yet portably available. See [Future: C++26 reflection](#future-c26-static-reflection). |

So dustman's primary mechanism is an explicit tracer, with ergonomic shorthands provided, and a clean migration path to fully-automatic tracing via reflection when compilers catch up.

## API direction

The API surface is designed around four decisions. Concrete signatures will live in `docs/api.md` once settled; what follows is the shape.

### 1. `Tracer<T>` specialisation is primary

Tracing is described via a specialisation of a template trait:

```cpp
template<>
struct dustman::Tracer<MyClass> {
  static void trace(MyClass& obj, dustman::Visitor& v) {
    v.visit(obj.child);
    v.visit(obj.next);
  }
};
```

This works for any type — including ones the consumer doesn't own — because the specialisation lives in dustman's namespace and can be written in any translation unit.

### 2. `FieldList<T, ...>` as the ergonomic default

For the common case ("just scan these fields"), a helper generates the tracer from member pointers:

```cpp
template<>
struct dustman::Tracer<MyClass>
  : dustman::FieldList<MyClass, &MyClass::child, &MyClass::next> {};
```

No macros, no base class. Fall back to a hand-written tracer when needed (tagged unions, conditional fields, optional members, etc.).

### 3. `dustman::Object` as optional sugar

For consumers who prefer an OO flavour, `dustman::Object` is a base class whose auto-specialised tracer dispatches to a virtual `trace()`:

```cpp
class MyClass : public dustman::Object {
  void trace(dustman::Visitor& v) override { v.visit(child); }
};
```

Strictly optional. Present because it matches familiar idioms (Oilpan-style APIs). Inheritance is never required by dustman itself.

### 4. Object header holds a `const TypeInfo*`

Every GC-managed object carries a one-word header pointing at a per-type `TypeInfo` struct:

```cpp
struct TypeInfo {
  std::size_t size;
  void (*trace)(void*, Visitor&);
  void (*destroy)(void*);
  std::uint32_t flags;   // movability, immutability, has-finaliser, etc.
};
```

This is the "manual vtable" that non-virtual designs require in any case. It gives the collector what it needs to mark, evacuate, and finalise any object from a raw pointer alone, independent of whether the type uses inheritance.

### 5. `Root<T>` for externally-held references

Precise collection needs to know every live reference — every word that could currently be holding a GC-managed pointer and keeping an object alive. References stored inside GC-managed objects are found by tracing (via `Tracer<T>`). References held outside the heap — on the stack, in globals, in static storage, in host-owned containers — must be registered explicitly.

Dustman's mechanism is a smart-pointer template, `Root<T>`, that registers itself in a thread-local root list at construction and unregisters at destruction. The root list is a vector of pointers to `gc_ptr_base` sub-objects; the collector walks it during marking.

```cpp
dustman::Root<MyObj> r = dustman::alloc<MyObj>(args);   // registered, non-null
dustman::gc_ptr<MyObj> p = r;                            // implicit slice; no registration
parent->child = r;                                       // assigns to a field via gc_ptr<T>
```

`Root<T>` composes a `gc_ptr<T>` as its payload and a `std::size_t` slot index into the thread-local root vector. Move is supported via a handle-based protocol: on move, the destination takes over the slot, and the vector entry is updated to point at the destination's payload. This keeps `Root<T>` usable as a function return, a `std::vector` element, or any other movable context while guaranteeing the collector always finds a live address in every occupied slot.

Key properties:

- **`alloc<T>()` keeps returning `gc_ptr<T>`.** The consumer decides when to wrap in a `Root<T>`. Field-assignment paths (`parent->child = alloc<T>(...)`) pay zero root-registration cost.
- **Implicit conversion `Root<T> → gc_ptr<T>`.** `parent->child = root;` works without ceremony.
- **Copy is deleted, move is cheap.** A `Root<T>` is a unique registration; duplicating it would mean duplicating slot ownership.
- **Free-list slot reuse.** The root vector grows monotonically to the historical peak; destroyed roots leave `nullptr` entries that the collector skips and subsequent `Root` constructions re-occupy.

In later phases (incremental and concurrent collection), allocation will act as an implicit safepoint — the contract between `alloc<T>()` returning a `gc_ptr<T>` and the consumer wrapping it in a `Root<T>` needs to be tightened so that the brief intermediate value cannot be collected. That is a contract change, not an API change; `Root<T>`'s shape and the `alloc<T>()` return type do not move.

## Inheritance-optional rationale

A general-purpose GC library that cannot manage types the consumer doesn't own is a weaker proposition. Forcing `: public dustman::Object` locks out third-party types and makes dustman unsuitable for anything but green-field code.

The `Tracer<T>` specialisation mechanism decouples **how the GC dispatches tracing** from **how the consumer's types are declared**. A third-party type can be made GC-managed by writing a specialisation in the consumer's own translation unit, without touching the third-party header.

Green-field consumers who don't need this flexibility still get the ergonomic shortcut via `dustman::Object` — nothing is lost, and the inheritance-free path remains available.

## Mechanical sympathy with the memory hierarchy

Immix's structural choices — blocks of ~32 KB, lines of ~128 B, out-of-line mark bitmaps, per-thread bump allocation — are not arbitrary. Each is aligned to a concrete property of modern CPUs. Deviating from those sizes and layouts forfeits most of the performance argument for a tracing collector over refcounting or free-list allocation. This section captures the *why* behind the structural choices; the roadmap that follows describes when each one lands.

### Cache lines and line granularity

Lines are sized to a small multiple of the hardware cache line (typically 64 B on x86-64, 64 or 128 B on ARM). Writes into a freshly-allocated line are absorbed by the store buffer and can be flushed to a line the CPU never had to fetch first — no read-for-ownership, no coherency round-trip. This matters when bump-allocating tens of millions of small objects per second.

Reclamation is per-line for the same reason: a line is either fully live, fully dead, or mixed, and the coarsest safe granularity that still permits reuse is the cache line. Reclaiming at word granularity (free-list allocators) fragments lines and poisons the cache.

### Blocks, pages, and the TLB

Blocks are page-multiple (32 KB ≈ 8 × 4 KB pages, or a fraction of a 2 MB huge page), not page-sized. This matters in three ways:

- A thread's hot block stays resident in the TLB for the duration of many allocations — the allocation path never pays a TLB miss.
- Block-level OS interactions (acquire, release, `madvise(DONTNEED)` on empty blocks) operate on page-aligned, page-multiple regions, which the kernel handles cleanly.
- Huge-page backing for the block pool dramatically reduces TLB pressure on large heaps. One TLB entry covers 2 MB rather than 4 KB — decisive for pointer-chasing workloads during the mark phase.

### Bump allocation and the branch predictor

The fast path is:

```
if (cursor + size <= tlab_end) { cursor += size; return obj; }
```

The branch is taken ~99% of the time — TLAB exhaustion is rare by design. Modern branch predictors handle it perfectly, and the whole path costs a handful of cycles. Free-list allocators pay tens of cycles on size-class lookup and free-list traversal, with much less predictable control flow.

Writes into a freshly-allocated line are also contiguous and sequential, ideal for the store buffer and write-combining logic, and prefetch-friendly for the subsequent constructor reads.

### False sharing and per-thread TLABs

Thread-local allocation buffers are block-aligned: no two threads' hot allocation regions share a cache line, so no coherency traffic flows between cores during allocation. This is the single most important lock-free-allocation property to get right — a naïve shared bump pointer produces cache-line ping-pong that dominates allocation cost past two cores.

The same discipline extends to collector-internal state. Per-thread mark queues, remembered-set fragments, and collection counters are cache-line-padded. The inner loops of the marker and sweeper are exquisitely sensitive to false sharing.

### Out-of-line mark bitmaps

Mark bits live in a side table per block — a tightly packed array — not in the object header. Three consequences:

- Marking an object doesn't dirty the object's own cache line. Under concurrent marking, where the mutator may be actively reading the object, this avoids a coherency invalidation on every mark.
- The mark phase has high spatial locality: walking the bitmap is a sequential scan over a small, dense array — prefetch-friendly, L1-resident.
- Mark-bit clearing at the start of a cycle is a bulk `memset`-equivalent over contiguous memory, not scattered writes across the heap.

### Generational collection and cache residency

Most allocations die young. The nursery is sized to fit comfortably within the L2 or L3 cache of the mutator thread, so by the time it is collected, most of its contents are still hot. The mark phase then costs almost nothing: it scans memory the mutator touched seconds ago, still resident in the cache hierarchy.

Contrast with a single-heap collector that scans tenured and young generations together: it unavoidably touches cold memory and pulls it back through L3 → DRAM, displacing the mutator's working set on every collection.

### Why this beats the alternatives

- **`std::shared_ptr`.** Every copy performs an atomic increment on the control block — a separate cache line from the object, often in a different cache set. Atomic RMW stalls the pipeline. Every destructor does the reverse. Allocation-heavy workloads pay this on essentially every operation.
- **`malloc` / free-list allocators.** Size-class lookups, coalescing, and free-list chasing dirty many cache lines per allocation. Good general-purpose allocators amortise this well, but they cannot compete with bump allocation on pure throughput.
- **Conservative / non-moving collectors (Boehm).** Cannot compact. Over time the heap fragments into cache-unfriendly layouts; retained garbage further dilutes cache density. Mark bits in object headers (when used) add coherency cost under concurrent marking.

Immix's structure exists precisely because the hardware rewards it: linear writes, cached reads, no false sharing, predictable branches, TLB-friendly strides. The performance advantage over refcounting is not "less bookkeeping" — it is *fewer cache misses and fewer coherency round-trips per allocation*.

## Roadmap

Implementation follows a pragmatic migration path, but the API and header layout are designed for the endpoint from day one — concurrency, generations, and movement cannot be cleanly retro-fitted.

1. **Arena allocator** — bump allocation, leak-on-purpose. Establishes the allocation path and thread-local buffer structure. Proves zero-cost allocation.
2. **Mark-sweep on arena** — precise root walking via consumer-supplied tracers. Proves the `Tracer<T>` API and safepoint infrastructure.
3. **Immix** — line/block reclamation, opportunistic evacuation, generational (nursery + tenured). Proves moving and generational under the same API.
4. **Incremental / concurrent marking** — reduce pause time independent of heap size. The write barrier and safepoint poll are already in the hot path from phase 2.

The public API should not shift meaningfully across these phases.

### Status

Landed:
- **Phase 1–2** — arena allocator, mark-sweep, precise tracing via `Tracer<T>`, reentrancy guard.
- **Phase 3a** — line-aware small allocator, recycle list, medium tier, huge side-table, over-alignment routing.
- **Phase 3b** — opportunistic evacuation with forwarding pointers and `UpdateVisitor`; formal [`specs/collect.tla`](../specs/collect.tla).
- **Phase 3.5** — multi-mutator stop-the-world with formal [`specs/stw.tla`](../specs/stw.tla): safepoint protocol, attach/detach lifecycle, collector-identity serialisation, `enter_native` / `leave_native` for external blocks, fast-path safepoint in `alloc<T>`.

Remaining:
- **Phase 3c** — generational (nursery + tenured) under the existing moving infrastructure.
- **Phase 4** — concurrent / incremental marking with write barriers (own TLA+ spec planned).
- **Deferred topics** (below): parallel STW, thread-local young generation.

### Deferred: parallel STW collection

Between serial STW (phase 3.5) and concurrent (phase 4) sits **parallel STW**: the collector itself uses N threads during the pause. Mutators are still parked, so no write barrier or tri-color invariants — just work-stealing over mark, evacuation, classify, and finalize. Payoff order is mark ≫ evacuate > tail; mark dominates on real heaps.

The bottleneck is not atomic mark-bit CAS (uncontended atomic OR is ~10-20 cycles vs ~100-300 for the cache miss to load the object) but **cache-line bouncing on shared bitmaps**. Mitigation: work-steal at block granularity rather than object, so different threads touch different bitmaps. Expect ~0.7-0.9× per-thread efficiency, saturating at memory bandwidth (usually 4-8 cores desktop, more on NUMA).

Phase 3.5 (serial STW) is a prerequisite; parallel extends it phase-by-phase without changing heap layout — the existing `UniqueCollector` invariant still holds (one *logical* cycle), and workers would warrant their own phase-barrier sub-spec.

### Deferred: thread-local young generation

Per-thread nurseries are a clean win when the consumer commits to an isolation story (Ractor-style: no shared mutable state, or only via explicit channels); one thread's young-gen collects without touching others'. Under classical shared-memory semantics the remembered-set bookkeeping needed to handle cross-thread young-gen pointers tends to eat most of the locality benefit.

Middle ground, which the current TLAB design already supports structurally: **shared nursery with per-thread TLABs**. Allocation locality lands for free; collection is still global. Revisit the per-thread-nursery question once Frozone's concurrency model is pinned down.

## Testing and verification

Garbage collectors fail in asymmetric, hard-to-reproduce ways:

- **False retention** (the GC keeps an object it could have freed) is a performance bug. Detected by heap-size assertions. Embarrassing but harmless.
- **False reclamation** (the GC frees an object that is still reachable) is a silent data-corruption catastrophe, typically surfacing weeks after the offending allocation as a crash in unrelated consumer code. A tracer that forgets a single field can lurk for months.

Dustman's test strategy biases aggressively toward catching false reclamation. Several features exist specifically to make the collector testable and must be designed in from the start, not bolted on.

### Testability as a design property

The following hooks are built into dustman's core, not retrofitted into a separate testing layer:

- **Stress mode.** Every allocation triggers a full GC. Any tracer that forgets a field is caught the first time that field is read after allocation. The single highest-yield test mode.
- **Deterministic GC.** In test mode, collections trigger only at explicit points — no timers, no heuristics, no scheduler. Randomised tests are reproducible from a seed.
- **Heap verifier.** An O(heap) invariant sweep: every `gc_ptr` resolves to a live object with a valid `TypeInfo*`; mark bits are consistent; no cross-generation pointers exist outside the remembered set. Callable after any safepoint in debug builds.
- **Poison-on-reclaim.** Reclaimed memory is overwritten with a distinctive pattern in debug builds. Use-after-free surfaces as an immediate crash rather than a silent mis-read.
- **Safepoint injection.** Tests can force a safepoint at arbitrary mutator points.
- **Allocation-site tagging.** Each object records its allocation site in debug builds, for leak and retention analysis.

### Test categories

1. **Unit** — block pool, line metadata, TLAB, write barrier, mark bitmap, `FieldList` expansion. Fast, isolated, cover individual components.
2. **Integration** — hand-built object graphs, allocate + collect cycles, tracer correctness. End-to-end but deterministic.
3. **Property-based** — seeded generators produce random-but-structurally-valid object graphs and mutation sequences. After each GC, invariants are checked. The main defence against "the collector works on tests we wrote".
4. **Fuzz** — distinct from property-based. Coverage-guided mutation of API call sequences (allocate, assign, safepoint, trigger-GC, ...) via **libFuzzer**, with **libprotobuf-mutator** providing a structured grammar so mutations stay decodable. Reaches states no human would script. A specific sub-mode fuzzes *tracers themselves* — generates correct and deliberately-buggy tracers and asserts that stress mode + heap verifier detect the bad ones, testing the testing infrastructure.
5. **Stress / torture** — long-running, high allocation pressure, many GC cycles. Exercises TLAB refill, block recycling, evacuation, remembered set maintenance under load.
6. **Sanitisers** — AddressSanitizer and UndefinedBehaviorSanitizer throughout. ThreadSanitizer added once concurrent marking lands.
7. **Benchmarks** — allocation throughput, GC pause time, heap overhead. Tracked as a separate gate; not correctness tests, but essential for catching performance regressions. Each collection phase captures a baseline on landing (throughput + cache-miss rate) so later layout or algorithm changes have real numbers to justify themselves against.

Both positive and negative tests are written for every feature: the former verifies *the GC collects what it should*, the latter verifies *the GC does not collect what it should not*.

### Formal methods: TLA+ for the concurrent protocols

The hardest GC bugs hide in concurrent protocols — write barriers interacting with concurrent marking, safepoint synchronisation, remembered set updates under mutation, TLAB refill under contention. Testing fundamentally cannot exhaustively exercise the interleavings that matter.

The appropriate tool is **TLA+**: a specification language plus model checker, well-suited to concurrent protocols, with a strong track record on industrial systems. We use it not to verify the C++ code directly, but to verify the *algorithms* the C++ implements.

TLA+ specifications are written and maintained in the repo for each load-bearing concurrent protocol:

- The tri-colour invariant under concurrent marking
- The write barrier protocol
- The safepoint handshake (mutators yielding cooperatively)
- The remembered set protocol for generational collection
- TLAB refill and block recycling under concurrent allocation

Each spec is a first-class artifact. Protocol changes are made in the spec first, re-checked, and only then implemented in C++. A spec that drifts from the implementation creates false confidence, so discipline matters — but the alternative (testing alone) is strictly worse for protocols at this level of concurrency.

As a bonus, a TLA+ model of the write barrier is simultaneously the clearest explanation of the write barrier anyone will ever read.

### Lightweight executable specifications

Three complementary forms of lightweight verification fall out of the design:

- **Runtime assertion of model invariants.** Every invariant the TLA+ spec proves has a corresponding debug-build assertion in C++. The spec and the code converge at the assertion.
- **Heap verifier as executable spec.** The verifier's post-conditions *are* the specification of heap well-formedness. Runnable against any real heap at any safepoint.
- **Reference implementation for differential testing.** A deliberately slow, obviously-correct mark-sweep collector runs in debug mode alongside the real collector. The real collector is correct iff the two agree on reachability. Catches divergence without requiring a proof.

### Explicit non-goal: source-level proofs

Coq/Iris-level proofs of the C++ implementation itself are **out of scope**. Prior work exists (CertiCoq's verified GC, Iris separation-logic proofs of heap-manipulating programs), and the techniques are well-suited to the domain. But proof engineering at that level is a multi-year project in itself, and the gap between extracted Coq code and idiomatic C++ means some verification gap always remains. A research partnership could pick this up later; the design must not depend on it.

### Tooling

- **Tests:** [Catch2 v3](https://github.com/catchorg/Catch2). Minimal dependency, expressive, a small compiled helper library. Chosen over GoogleTest for lighter weight and cleaner ergonomics.
- **Benchmarks:** [Google Benchmark](https://github.com/google/benchmark). Handles warm-up, statistical analysis, and regression tracking properly.
- **Fuzzing:** libFuzzer (Clang built-in, ASan-integrated) + libprotobuf-mutator for structure-aware mutation of API call sequences.
- **Sanitisers:** AddressSanitizer and UndefinedBehaviorSanitizer in default CI builds; ThreadSanitizer added once concurrent marking lands.
- **Model checking:** the TLC model checker (TLA+ reference implementation) runs in CI on any change to a `.tla` spec file.

## Future: C++26 static reflection

C++26 adds compile-time reflection (P2996): templates can introspect a type's members — names, types, offsets, access specifiers. For dustman this means a `Tracer<T>` specialisation can be generated automatically by walking `T`'s members at compile time and emitting `v.visit(...)` calls for the ones that are `gc_ptr<U>`.

Once compiler support is ubiquitous (Clang is implementing it; others following), dustman will ship an auto-specialising `Tracer<T>` for types that opt in via a tag type or `requires`-expression. Hand-written and `FieldList`-based tracers remain available and unchanged.

This is a future evolution, not a current dependency. The API is designed so that auto-generated tracers slot in as additional specialisations without disturbing existing consumers.
