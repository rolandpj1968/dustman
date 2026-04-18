# Collection semantics

This document captures the invariants, state machine, and failure modes of dustman's collection subsystem. It grows with each phase; today it covers phase 2 (stop-the-world mark-sweep, single mutator). Later phases extend rather than replace.

## State machine

Three states, driven by `dustman::collect()`:

```
idle ──collect()──▶ marking ──worklist drained──▶ sweeping ──reclaim done──▶ idle
```

Phase 2 is stop-the-world on a single mutator thread: `collect()` runs synchronously and returns with `gc_state == idle`. Phase 2b implemented `idle → marking`; phase 2c added **whole-block sweep** — a block with no mark bits set is reclaimed, destroying every object it contains and returning its memory to the OS. Blocks with any live object are kept entirely; dead objects in partially-live blocks wait for phase 2d's partial-block reuse (planned as the transition to Immix line reclamation in phase 3a).

Phase 3a-i (this step) begins the Immix transition: sweep additionally computes a per-block **`line_map`** (one byte per 256-byte line) for kept blocks. A line is flagged live iff it contains any part of a live allocation — header, body, or trailing padding. The allocator does not yet consult the line_map (that lands in 3a-ii).

## Invariants

**Before `collect()`:** `gc_state == idle`. Mark bits may carry stale state from the previous cycle; they are cleared on entry.

**After mark phase completes:** an object's mark bit is `1` iff the object is reachable from some registered `Root<T>` by following `Tracer<T>::trace` calls. No other objects are marked.

**During marking:** the mark worklist contains objects that are marked-but-not-yet-traced. `MarkVisitor::visit` skips already-marked objects, which makes the algorithm cycle-safe and lineary-bounded in the number of live objects.

**During `collect()`:** the reentrancy guard (thread-local `collecting_`) is set. `alloc<T>` asserts it is clear and aborts otherwise. `collect()` itself asserts it is clear on entry.

**During `sweeping`:** the current TLAB is retired (cursor and end set to `nullptr`). Fully-dead blocks have their objects destroyed (start_bitmap walk + `TypeInfo::destroy`) and their memory returned to the OS. Kept blocks have their `line_map` rebuilt from the `start_bitmap ∩ mark_bitmap`, extended by the per-object size (via `TypeInfo::size`) so that every line the allocation spans is flagged live.

**After `collect()`:** `gc_state == idle`. The reentrancy guard is cleared. Mark bits reflect the reachability snapshot captured during the cycle; surviving blocks carry a `line_map` consistent with that snapshot.

## Failure modes and how we catch them

| # | Failure | Effect | Detection |
|---|---|---|---|
| 1 | Tracer omits a `gc_ptr<T>` field | Reachable object unmarked; freed by sweep; silent UAF later | Stress mode (collect-on-every-alloc, scheduled to land with sweep) surfaces it at first access. A negative test with a deliberately-buggy tracer proves the mechanism detects omissions. |
| 2 | Allocation during `collect()` (tracer allocates, for example) | New object untracked; state machine corruption | Reentrancy guard: `alloc<T>` aborts if `collecting_` is set. |
| 3 | Reentrant `collect()` | State machine corruption | Same reentrancy guard; aborts on entry. |
| 4 | Slot arithmetic wrong | Mark bits set for the wrong slot | Round-trip tests: alloc, collect, `is_marked(p)` agrees with expected reachability. |
| 5 | Cycle in the object graph | Infinite loop during marking | `MarkVisitor` checks the mark bit before queueing; already-marked objects are skipped. |

## Consumer contract (phase 2)

- All GC-managed references outside the heap live in `Root<T>`. A stack-local `gc_ptr<T>` is not a root; its pointee may be collected.
- Tracers visit every `gc_ptr<T>` field of their type. `FieldList<T, ...>` handles the common case without hand-written bookkeeping.
- Tracers must not allocate.
- `collect()` must not be called from within another `collect()` or from a tracer.
- **Destructors of GC-managed types must not read from or dereference other GC-managed objects.** When the sweep phase destroys a fully-dead block, the order in which it destroys objects is unspecified, and any object a destructor might touch may itself already be destroyed. Destructors should free only non-GC resources; for most types with only `gc_ptr<T>` fields the compiler-generated destructor is already correct (it is trivial; it does not dereference).
- **GC-managed types must be trivially relocatable.** When the collector evacuates a sparsely-live block (phase 3b), it moves live objects to a new location via `memcpy` and abandons the old location without calling the destructor. For this to be safe, a bit-copy must produce a valid live object at the new location. Most sensible types satisfy this — plain aggregates, types with `gc_ptr<T>` fields and primitive members, types with trivially-relocatable RAII members (`std::unique_ptr`, `std::shared_ptr`, `std::string` without SBO, `std::vector` on most implementations). Types with **self-referential internal pointers** (`std::list`, `std::deque`, types storing `this` or the address of a member) are not trivially relocatable — put the bulk in a separately-allocated buffer and hold a `gc_ptr` to it instead. Where the compiler supports it, `TypeInfoFor<T>::value`'s instantiation `static_assert`s `__is_trivially_relocatable(T)`; otherwise the constraint is documented-only and the consumer is responsible.

Runtime-detectable violations of this contract abort.

## Formal model

The `collect()` state machine — phases, block-set transitions, recycle-list interaction — is modelled in TLA+ at [`specs/collect.tla`](../specs/collect.tla), verified by TLC against the invariants `NoSelfEvacuation` and `RecycleCleanDuringEvac`. The spec was introduced alongside phase 3b-ii after a between-phase invariant violation (recycle list surviving into evacuation) produced a cascading self-iteration bug in the evacuator. Single-threaded bugs at phase boundaries are TLA+ territory just as multi-threaded ones are.

Future phases extend the spec rather than replace it — phase 3.5 multi-mutator STW and phase 4 concurrent mark are planned to land together with their own TLA+ specs, per the commitment in [`docs/design.md`](design.md).
