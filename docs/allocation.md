# Allocation strategy

This document captures the allocation tier structure of dustman's heap, and the rationale behind each choice. Like `collection.md`, it grows with each phase; today it covers phase 3a's small-object path. Medium and huge paths are sketched and land in subsequent phases.

## Three size tiers

```
Small    (≤ 256 B / one line)   → line-aware bump in shared small blocks     [phase 3a-ii ✔]
Medium   (256 B .. 4 KiB)       → dedicated medium blocks, bump-only          [phase 3a-iii ✔]
Huge     (> 4 KiB)              → mmap per object, tracked externally          [phase 3a-iv]
```

Each tier is an independent allocator path. `alloc<T>()` routes **at compile time** via `if constexpr (object_bytes<T>() ≤ threshold)`; small allocations never touch medium or huge code, and vice versa. No runtime dispatch, no branch prediction concerns, no cross-contamination between the hot path and the cold paths.

## No cross-line allocations in the small path

**A key simplifying invariant: no object in a small block spans a line boundary.** Every small allocation fits entirely within a single 256 B line.

This gives the small allocator a trivial shape:

- Fast path: bump the cursor within the current line. One comparison.
- Slow path: find *any* free line (N = 1). Pick it, resume.

No "run of N contiguous free lines" logic. No medium-awareness anywhere in the small code. Structurally, the small allocator is the phase-1 arena allocator with "line" substituted for "block" — with the extra machinery that tracks which lines are free across sweeps.

The cost is that objects whose `object_bytes<T>()` exceeds `line_size` cannot use the small path. They go to medium. That's a deliberate split: each tier stays simple within its own scope.

## Line size: 256 B

Line size here is *not* the classic Immix reclamation granularity alone — our no-cross-line constraint also makes it the **cap on small-object size**. That changes the trade-off.

### Ruby object-size distribution (indicative)

| Size range | Approximate share | Examples |
|---|---:|---|
| 0 – 64 B | ~40 % | integers, symbols, small objects |
| 64 – 128 B | ~40 % | 3–5-field objects, inline small arrays |
| **128 – 256 B** | ~15 % | classes, method objects, compiled CallInfo |
| 256 B – 4 KiB | ~4 % | array/hash backing, medium strings |
| > 4 KiB | < 1 % | rare |

| Line size | Small-path coverage |
|---|---:|
| 128 B | ~80 % of allocations |
| **256 B** | **~95 % of allocations** |

Moving from 128 B (classic Immix) to 256 B pulls the 128 – 256 B band — ordinary classes with a handful of ivars, method objects, Compiled Call Info — into the fast path. That's a meaningful win for Ruby-shaped workloads and for any consumer whose "typical" objects are slightly bigger than minimal.

### What 256 B costs

- **Within-line fragmentation worst case** grows from 120 B (one survivor in a 15-slot, 128 B line) to 248 B (one survivor in a 31-slot, 256 B line). Partially-live lines retain more dead space per line.
- **Reclamation granularity halves**: 123 lines per block instead of 245.

Both matter less than the numbers suggest. Allocation correlation means partially-live lines are the exception rather than the rule (objects born together tend to die together — the generational hypothesis). And compaction (phase 3b, opportunistic evacuation) eventually cleans up line-level fragmentation regardless of line size. 123 lines per block remains plenty finer than whole-block sweep (the phase 2c granularity).

### Empirical follow-up

Once dustman hosts a real workload (Frozone), we can measure the actual object-size distribution and line-occupancy pattern. If the 128 → 256 intuition turns out wrong for a given consumer, the constant is one line of header to change. No API implications.

## The three tiers map cleanly onto Immix

Dustman's tiers are Immix's tiers, with one deliberate simplification:

- Dustman "small" ↔ Immix "small" (bump within line).
- Dustman "medium" ↔ Immix "medium" — but **our medium uses dedicated blocks** rather than multi-line runs inside shared blocks. A block is flagged `small` or `medium` at acquisition time via `BlockHeader.flags`; sweep branches on the flag (line_map rebuild + recycle push for small, whole-block check for medium).
- Dustman "huge" ↔ Immix "LOS" (large-object space, externally tracked).

Classic Immix mixes small and medium within the same block, and reclaims medium via contiguous free-line runs. Dustman separates them onto different block pools. This keeps each allocator's code self-contained at the cost of some memory density (a medium block is always medium even if it has unused line space).

## Compaction fills the fragmentation gap

Three kinds of fragmentation remain after line reclamation:

1. **Within-line** — dead slots in partially-live small lines.
2. **Within medium block** — dead medium objects in partially-live medium blocks.
3. **Within huge pool** — not a thing; huge allocations are one-per-region.

Classes 1 and 2 are cleaned up by **opportunistic evacuation** (phase 3b): a sparsely-live block has its survivors copied to a fresh block, and the original is freed. Evacuation in phases 3 and 4 is stop-the-world, so no read barrier is required — the mutator is paused throughout the copy-and-update-pointers sequence. Concurrent evacuation (mutator running while objects move) is phase 5+ territory and *does* require a read barrier.

Fragmentation is thus bounded in the steady state by the per-cycle evacuation budget, not by lifetime-correlation luck.

## Invariants shared across tiers

- All tiers use the same `BlockHeader` layout (the `line_map` is simply unused by medium and huge blocks).
- `gc_ptr<T>` semantics are identical across tiers; consumers do not see which tier allocated an object.
- `TypeInfo` dispatch is tier-agnostic; sweep, destroy, and tracing work the same way regardless of where the object lives.
- The consumer contract (no tracer allocations, destructors don't dereference GC-managed state, no raw `T*` across safepoints once movement arrives) applies uniformly.

## Cross-block reuse

Phase 3a-iii landed the **small recycle list**. Sweep pushes every surviving small block that has at least one free line onto `Heap::small_recycle_`. When the current small-TLAB exhausts its block, `alloc_slow_small` pops from the recycle list before reaching for a fresh OS-backed block. Old kept blocks' free lines are actually reused; line reclamation is now the full Immix reclamation story at the small-block level.

Medium blocks do not participate in the recycle list — the bump-only medium allocator can only use a block until its cursor reaches the block's end, at which point the block is effectively "full" for allocation purposes (dead slots remain inside until the whole block dies or compaction runs). When a medium block goes fully dead, sweep frees it.
