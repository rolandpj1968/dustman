# Collection semantics

This document captures the invariants, state machine, and failure modes of dustman's collection subsystem. It grows with each phase; today it covers through phase 3b-ii (stop-the-world mark-sweep with opportunistic evacuation, single mutator). Later phases extend rather than replace.

## State machine

Outer states, driven by `dustman::collect()`:

```
idle ──collect()──▶ marking ──worklist drained──▶ sweeping ──reclaim done──▶ idle
```

The collector runs stop-the-world on a single mutator thread: `collect()` runs synchronously and returns with `gc_state == idle`.

**Sweeping** is split into four sub-phases, run back-to-back in a single call:

```
classify ──▶ evacuate ──▶ update ──▶ finalize
```

- **classify** walks every block, destroys fully-dead blocks outright, classifies the rest by live-byte fraction against `evacuation_threshold_percent_`. Sparse blocks get the `flag_block_evacuating` bit and are handed off to evacuate; dense blocks have their `line_map` recomputed. The small recycle list is cleared on entry — the load-bearing step that prevents the phase 3b-ii cascade (see [`specs/collect.tla`](../specs/collect.tla)).
- **evacuate** memcpy's every live object out of each sparse source block into a fresh target, stamps `start` and `mark` on the copy, and writes a forwarding pointer into the source header (low bit of the `TypeInfo*` set to 1).
- **update** walks roots and reachable objects, rewriting every `gc_ptr<T>` whose pointee is forwarded so it points at the new body. `UpdateVisitor` keeps a `visited_` hash set for cycle-safety without touching mark bits.
- **finalize** frees the flagged-evacuating source blocks (running destructors only on non-forwarded objects, since forwarded ones are aliases of the live copy) and rebuilds the small recycle list from surviving blocks with any free line.

Per-tier handling inside sweeping:

- **small** (≤ line size): line-aware bump allocation; recycle list pops small blocks with any free line.
- **medium** (up to `medium_size_limit`): whole-block bump; no recycle participation.
- **huge** (above `medium_size_limit` or `alignof > alignof(void*)`): side-table managed. Huge objects are never evacuated. Marking and updating of huge records use dedicated `marked` / `updated` flags for cycle-safety.

## Invariants

**Before `collect()`:** `gc_state == idle`. Mark bits may carry stale state from the previous cycle; they are cleared on entry.

**After mark phase completes:** an object's mark bit is `1` iff the object is reachable from some registered `Root<T>` by following `Tracer<T>::trace` calls. No other objects are marked.

**During marking:** the mark worklist contains objects that are marked-but-not-yet-traced. `MarkVisitor::visit` skips already-marked objects, which makes the algorithm cycle-safe and lineary-bounded in the number of live objects.

**During `collect()`:** the reentrancy guard (thread-local `collecting_`) is set. `alloc<T>` asserts it is clear and aborts otherwise. `collect()` itself asserts it is clear on entry.

**Entering `sweeping`:** both TLABs (small and medium) are retired (cursor and end set to `nullptr`). The small recycle list is cleared on entry to classify — without this, an earlier cycle's recycle leftovers can hand a now-flagged block back to the evacuator as a target.

**During `classify`:** fully-dead blocks have their objects destroyed (start_bitmap walk + `TypeInfo::destroy`) and their memory returned to the OS. Sparse blocks (live bytes below threshold) are tagged `flag_block_evacuating` and pushed to the evacuation worklist. Dense blocks have their `line_map` rebuilt from the `start_bitmap ∩ mark_bitmap`, extended by per-object size so every line the allocation spans is flagged live.

**During `evacuate`:** for every flagged source block, each live object is copied to a fresh target via `memcpy`; the target is taken from the evacuation TLAB, which bumps into fresh blocks acquired by `alloc_slow_small` / `alloc_slow_medium`. No flagged block is ever selected as a target (TLA+ invariant `NoSelfEvacuation`). The source header is rewritten to a forwarding pointer: a `uintptr_t` whose low bit is 1 and whose remaining bits are the new body address. The source's `start_bitmap` and `mark_bitmap` are left untouched for the update pass.

**During `update`:** all roots and transitively reachable `gc_ptr<T>` fields are visited. Any pointee whose header is forwarded gets rewritten to the new address. `UpdateVisitor` tracks already-visited objects in a local hash set — it does **not** clear mark bits — so the mark-snapshot invariant survives the cycle. Huge records use their own `updated` flag for the same purpose.

**During `finalize`:** blocks tagged `flag_block_evacuating` are freed. Any non-forwarded object in such a block (by construction, an unmarked one that was never live) has its destructor called; forwarded objects are aliases of the surviving copy and are skipped. The small recycle list is rebuilt from surviving blocks that still have any free line.

**After `collect()`:** `gc_state == idle`. The reentrancy guard is cleared. Mark bits reflect the reachability snapshot captured during the cycle; surviving blocks carry a `line_map` consistent with that snapshot. No reachable `gc_ptr<T>` in the heap or in `Root<T>` references a forwarded header.

## Failure modes and how we catch them

| # | Failure | Effect | Detection |
|---|---|---|---|
| 1 | Tracer omits a `gc_ptr<T>` field | Reachable object unmarked; freed by sweep; silent UAF later | Stress mode (collect-on-every-alloc, scheduled to land with sweep) surfaces it at first access. A negative test with a deliberately-buggy tracer proves the mechanism detects omissions. |
| 2 | Allocation during `collect()` (tracer allocates, for example) | New object untracked; state machine corruption | Reentrancy guard: `alloc<T>` aborts if `collecting_` is set. |
| 3 | Reentrant `collect()` | State machine corruption | Same reentrancy guard; aborts on entry. |
| 4 | Slot arithmetic wrong | Mark bits set for the wrong slot | Round-trip tests: alloc, collect, `is_marked(p)` agrees with expected reachability. |
| 5 | Cycle in the object graph | Infinite loop during marking | `MarkVisitor` checks the mark bit before queueing; already-marked objects are skipped. |
| 6 | Recycle list leaks across cycles into evacuation | Evacuator picks a flagged block as its target; `set_start`/`set_mark` on the copy land in the source's bitmaps, which the same `evacuate_block` loop rediscovers — self-iterating cascade of chain-forwarded headers | `classify_and_destroy_dead` clears `small_recycle_` on entry; `finalize_sweep` rebuilds from survivors. Modelled as `RecycleCleanDuringEvac` in [`specs/collect.tla`](../specs/collect.tla) and regression-tested by the `consecutive collects` case in `tests/test_evacuate.cpp`. |
| 7 | Cycle in the object graph during update | Infinite loop during update | `UpdateVisitor` tracks visited objects in a `std::unordered_set` (does not reuse mark bits, preserving the mark-snapshot invariant). Huge records use their own `updated` flag. |

## Consumer contract

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
