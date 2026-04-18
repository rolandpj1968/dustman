# Collection semantics

This document captures the invariants, state machine, and failure modes of dustman's collection subsystem. It grows with each phase; today it covers phase 2 (stop-the-world mark-sweep, single mutator). Later phases extend rather than replace.

## State machine

Three states, driven by `dustman::collect()`:

```
idle ──collect()──▶ marking ──worklist drained──▶ sweeping ──reclaim done──▶ idle
```

Phase 2 is stop-the-world on a single mutator thread: `collect()` runs synchronously and returns with `gc_state == idle`. Phase 2b implemented `idle → marking`; phase 2c (this step) adds **whole-block sweep** — a block with no mark bits set is reclaimed, destroying every object it contains and returning its memory to the OS. Blocks with any live object are kept entirely (dead objects in partially-live blocks wait for phase 2d's partial-block reuse).

## Invariants

**Before `collect()`:** `gc_state == idle`. Mark bits may carry stale state from the previous cycle; they are cleared on entry.

**After mark phase completes:** an object's mark bit is `1` iff the object is reachable from some registered `Root<T>` by following `Tracer<T>::trace` calls. No other objects are marked.

**During marking:** the mark worklist contains objects that are marked-but-not-yet-traced. `MarkVisitor::visit` skips already-marked objects, which makes the algorithm cycle-safe and lineary-bounded in the number of live objects.

**During `collect()`:** the reentrancy guard (thread-local `collecting_`) is set. `alloc<T>` asserts it is clear and aborts otherwise. `collect()` itself asserts it is clear on entry.

**After `collect()`:** `gc_state == idle`. The reentrancy guard is cleared. Mark bits reflect the reachability snapshot captured during the cycle.

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

Runtime-detectable violations of this contract abort.
