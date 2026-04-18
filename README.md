# Dustman

A standalone, general-purpose precise garbage collector library for C++.

## Goals

Dustman provides precise garbage collection for C++ applications willing to buy in to a minimal tracing contract. It targets workloads where reference counting (e.g. `std::shared_ptr`) is a correctness fit but a performance drag, or where allocation pressure demands near-free allocation and reclamation.

The long-term target is an Immix-style mark-region collector with:

- bump allocation via thread-local allocation buffers (TLABs)
- line/block-granularity reclamation
- opportunistic evacuation (compaction without full copying pauses)
- generational collection (nursery + tenured)
- precise stack maps and object tracing
- incremental / concurrent marking

## Precise, not conservative

Dustman is **precise**: the collector knows exactly which words are pointers. Consumers opt in by supplying a tracing mechanism — visitor pattern, trace method, or similar — that enumerates outgoing GC references for each object. The exact API is part of the ongoing design.

The payoff: objects can be moved (enabling compaction and evacuation), false retention is eliminated, and scanning is cheap. The cost: Dustman is not a drop-in `malloc` replacement — consumers must describe their object graph.

## Why not just use...

- **Boehm (libgc):** conservative, non-moving, mature. Works as a drop-in `malloc` replacement but cannot compact, cannot exploit consumer knowledge, and suffers false retention on large 64-bit heaps.
- **`std::shared_ptr`:** correct and deterministic but pays atomic refcount traffic on every copy and destroy. Prohibitive for allocation-heavy workloads.
- **Oilpan, SGCL:** both precise C++ GCs worth studying. Dustman's niche is being standalone, small, and opinionated about moving + generational from day one.

## Roadmap

Implementation follows a pragmatic migration path, but design is the hard part — concurrency, generations, and object movement cannot be cleanly retro-fitted, so they shape the API and data structures from the start.

1. **Arena allocator** — bump allocation, leak-on-purpose. Establishes the allocation path and TLAB structure.
2. **Mark-sweep on arena** — precise root walking via consumer-supplied tracers. Proves the visitor API and safepoint infrastructure.
3. **Immix** — line/block reclamation, opportunistic evacuation, generational nursery/tenured split.
4. **Incremental / concurrent marking** — reduce pause time independent of heap size.

## Design

See [docs/design.md](docs/design.md) for architecture, the precise-tracing contract, API direction, and rationale.

## Status

Early design phase. API, visitor mechanism, and core data structures are under active discussion.

## License

MIT.
