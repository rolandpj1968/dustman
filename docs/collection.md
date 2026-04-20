# Collection semantics

This document captures the invariants, state machine, and failure modes of dustman's collection subsystem. It grows with each phase; today it covers through phase 3.5 (stop-the-world mark-sweep with opportunistic evacuation, multiple mutator threads). Later phases extend rather than replace.

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

## Multi-mutator safepoint protocol

Phase 3.5 wraps the inner `collect()` pipeline in a stop-the-world coordination protocol so the pipeline's single-threaded invariants hold even when multiple mutator threads are active.

### Thread lifecycle

Each thread is in one of three states from the collector's POV:

```
detached ──attach_thread()──▶ running ──safepoint()──▶ parked
   ▲                             │                        │
   │                             │                        │
   └────detach_thread()──────────┘                        │
                                                          │
   running ◀──collector released, !pause_requested────────┘
```

- **detached** — not visible to the collector, not in `attached_count_`.
- **running** — holds a TLAB, has root slots registered, may allocate.
- **parked** — blocked inside `safepoint_slow()` on the condvar waiting for the pause to clear. TLABs retired.

Attaching during an active pause (`pause_requested_` set) transitions the thread straight to **parked** — it never enters **running** alongside the collector, which would violate the core invariant.

### Collector identity

Any attached running thread can call `collect()`. Serialisation is via `has_collector_` under `stw_mu_`: the first thread to acquire becomes the collector, subsequent callers park as ordinary mutators and return once the cycle completes. At most one collector exists at a time.

The thread-local `collecting_` flag serves a second duty under STW: it is the "I am the collector, don't park me" marker that `safepoint_slow()` checks to avoid the collector parking itself.

### Safepoint protocol invariants

**`NoRunningDuringCollect`:** while `has_collector_` is set and the collector has passed the parked-count wait (equivalent to `col_state = "collecting"` in the spec), every attached non-collector thread is parked. No mutator touches the heap while the collector is sweeping.

**`UniqueCollector`:** exactly one of `has_collector_ = true ∧ collector ∈ {some attached thread}` or `has_collector_ = false` holds.

**`PauseFlagCoherent`:** `pause_requested_` is set iff the collector is between `acquire_collector_slot` and `release_collector_slot`.

**Attach-is-atomic-with-park:** the decision "attach as running vs parked" is made under `stw_mu_`; if pause is requested the new thread increments both `attached_count_` and `parked_count_` before releasing the lock, so the collector's wait predicate stays balanced.

**Root set global registry:** each thread's `ThreadRootSet` is pointer-registered in `all_thread_roots_` at attach time (protected by `roots_mu_`), so the collector — running on one thread — can still visit every other thread's roots. `register_root_slot` itself calls `ensure_attached()` so creating a `Root<T>` from an externally-provided `gc_ptr<T>` (which doesn't go through `alloc<T>`) still attaches the thread.

### Liveness

The spec does not currently prove liveness, and the implementation inherits that gap: a mutator in a tight loop without any `safepoint()` calls and without hitting an allocator slow path will starve the collector indefinitely. Allocation-heavy workloads reach safepoints naturally; pure compute loops are the consumer's responsibility to instrument.

### Pause-responsiveness policy

Dustman takes a tiered approach, with the cost/risk shape of each mechanism deciding whether it's baked in, opt-in, or left to the consumer.

- **Fast-path safepoint (baked in).** `alloc<T>` calls `safepoint()` on every call, including the TLAB-bump fast path. Cost is one relaxed atomic load plus one predicted-not-taken branch (~1-2 cycles on x86/ARM64, ≲0.1 % of a realistic allocator-heavy workload). Closes the "tight TLAB-bump loop" starvation case completely. Cheap enough that making it optional would cost more in code complexity than it saves at runtime.
- **Slow-path safepoint (baked in).** `alloc_slow_small`, `alloc_slow_medium`, `alloc_huge` each poll on entry. Load-bearing for the case where an attach happens on first slow path and `ensure_attached()` needs to park a new thread during a pause.
- **Signal-based nudge (planned, opt-in).** An API along the lines of `dustman::enable_signal_preempt(signum)` would install a handler on demand; the handler sets a TLS flag that the mutator checks at its next safepoint. Opt-in because signal delivery has real portability and correctness implications: consumer-owned signal handlers, freestanding contexts without signals, third-party libraries that mask signals. Not load-bearing for correctness — strictly a responsiveness nudge.
- **`enter_native()` / `leave_native()` (planned, primitive).** Consumer wraps blocking syscalls, `std::thread::join`, long external waits. Between `enter_native` and `leave_native` the thread counts as parked from the protocol's POV, so the collector doesn't block waiting for it. Not a policy — just a primitive the consumer calls at points only the consumer knows about.
- **Explicit `safepoint()` in long non-allocating loops (consumer contract).** Documented, not enforced. Pure compute loops, tracer-like walks in consumer code.

The philosophy: bake in things that are cheap and universally correct; expose primitives for things the consumer must drive at specific points; opt-in things that have real downsides. Signal-based *preemption* of arbitrary consumer code (HotSpot's polling-page trick, `mprotect` + `SIGSEGV` handler) is out of scope — it needs compiler cooperation to place polls at known-safe instructions, which we don't have for hand-written C++.

### Implementation-to-spec mapping

The TLA+ spec at [`specs/stw.tla`](../specs/stw.tla) uses idealised actions that correspond closely to the C++:

| TLA+ action | Implementation |
|---|---|
| `MutatorAttach(m)` | `attach_thread()` — registers `ThreadRootSet`, bumps `attached_count_`, parks if `pause_requested_` |
| `MutatorDetach(m)` | `detach_thread()` — retires TLABs, unregisters the root set, decrements `attached_count_` |
| `MutatorSafepoint(m)` | `safepoint_slow()` — retires TLABs, parks on `stw_cv_` until `!pause_requested_` |
| `MutatorResume(m)` | cv wait predicate in `safepoint_slow` exits when the collector clears `pause_requested_` |
| `CollectorRequestPause(m)` | first half of `acquire_collector_slot` — sets `has_collector_` and `pause_requested_` |
| `CollectorBeginCollect` | `acquire_collector_slot`'s wait on `parked_count_ + 1 >= attached_count_` (equivalent to "all attached non-collector threads parked") |
| `CollectorEndCollect` | `release_collector_slot` — clears both flags atomically under `stw_mu_`, notifies all |

The spec is deliberately abstract: it does not model TLABs, root registration, or the allocator tiers. Those are implementation details that sit below the spec's level of abstraction, checked by the C++ test suite rather than TLC. If the code drifts from the spec's action semantics — for example, if `CollectorBeginCollect`'s guard is weakened or `MutatorAttach` loses its pause-aware branch — the spec's negative-path sanity checks (documented in [`specs/README.md`](../specs/README.md)) produce a counterexample in seconds.

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
| 8 | New thread attaches during a pause and races the collector | Mutator touches the heap alongside the collector — violates `NoRunningDuringCollect` | `attach_thread()` checks `pause_requested_` under `stw_mu_` and transitions the new thread straight to parked if a pause is active. Spec variant (`MutatorAttach` unconditionally to `running`) reproduces the bug in a three-step TLC counterexample. |
| 9 | `collect()` called concurrently from two threads | Two collectors race, heap corruption | `acquire_collector_slot` serialises via `has_collector_` under `stw_mu_`; the losing caller parks as a mutator and returns once the in-flight cycle completes. Covered by `STW: concurrent collect() callers both return`. |
| 10 | Collector starts sweeping before all mutators park | Mutator touches heap during mark/evacuate | Collector waits on `parked_count_ + 1 >= attached_count_` before proceeding. Spec variant (guard replaced with `TRUE`) fails `NoRunningDuringCollect` in five steps. |
| 11 | Mutator thread-local TLAB not retired before park | Collector frees a block the still-valid TLAB cursor points into → UAF after resume | `safepoint_slow` zeros both TLABs before parking. The collector cannot reach another thread's TLS, so this has to be done mutator-side. |
| 12 | Thread exits without calling `detach_thread()` | `all_thread_roots_` holds a dangling pointer into destroyed TLS; next collect dereferences it | Documented contract: threads must `detach_thread()` before exit. Not runtime-enforced. |
| 13 | Mutator never calls `safepoint()` and never hits an allocator slow path | Collector starves, cycle never progresses | Liveness, not safety — spec does not currently prove it. Allocator slow paths poll `safepoint()` for free; pure compute loops are a consumer responsibility. |

## Consumer contract

- All GC-managed references outside the heap live in `Root<T>`. A stack-local `gc_ptr<T>` is not a root; its pointee may be collected.
- Tracers visit every `gc_ptr<T>` field of their type. `FieldList<T, ...>` handles the common case without hand-written bookkeeping.
- Tracers must not allocate.
- `collect()` must not be called from within another `collect()` or from a tracer.
- **Threads must `detach_thread()` before they exit.** A thread that exits while still attached leaves a dangling pointer in the global root registry; the next `collect()` dereferences it. `attach_thread()` is called implicitly on first use (first `alloc<T>`, first `Root<T>`, first `collect()`), so explicit attach is optional; explicit detach is mandatory.
- **Long-running loops that do not allocate must call `safepoint()` periodically.** The allocator's slow paths poll `safepoint()` for free, so allocation-heavy workloads reach safepoints naturally. Pure compute loops (number crunching, tight tracer-like walks in consumer code) can starve the collector indefinitely unless they poll.
- **A thread blocked in an external synchronisation primitive (`join`, `lock`, I/O) counts as attached-but-not-polling from the collector's POV and will block the pause.** If the thread must remain attached across such a block, it needs to park itself first; the shortest workaround today is to `detach_thread()` before the block and `attach_thread()` after. A native-state transition API (analogous to JVM's `in_native`) is planned.
- **Destructors of GC-managed types must not read from or dereference other GC-managed objects.** When the sweep phase destroys a fully-dead block, the order in which it destroys objects is unspecified, and any object a destructor might touch may itself already be destroyed. Destructors should free only non-GC resources; for most types with only `gc_ptr<T>` fields the compiler-generated destructor is already correct (it is trivial; it does not dereference).
- **GC-managed types must be trivially relocatable.** When the collector evacuates a sparsely-live block (phase 3b), it moves live objects to a new location via `memcpy` and abandons the old location without calling the destructor. For this to be safe, a bit-copy must produce a valid live object at the new location. Most sensible types satisfy this — plain aggregates, types with `gc_ptr<T>` fields and primitive members, types with trivially-relocatable RAII members (`std::unique_ptr`, `std::shared_ptr`, `std::string` without SBO, `std::vector` on most implementations). Types with **self-referential internal pointers** (`std::list`, `std::deque`, types storing `this` or the address of a member) are not trivially relocatable — put the bulk in a separately-allocated buffer and hold a `gc_ptr` to it instead. Where the compiler supports it, `TypeInfoFor<T>::value`'s instantiation `static_assert`s `__is_trivially_relocatable(T)`; otherwise the constraint is documented-only and the consumer is responsible.

Runtime-detectable violations of this contract abort.

## Formal model

Two TLA+ specs cover the two state machines that have non-trivial between-phase invariants:

- [`specs/collect.tla`](../specs/collect.tla) — `collect()`'s inner pipeline (marking / classify / evacuate / update / finalize). Verified by TLC against `NoSelfEvacuation` and `RecycleCleanDuringEvac`. Landed alongside phase 3b-ii after a recycle-list-survives-into-evacuation bug produced a cascading self-iteration in the evacuator.
- [`specs/stw.tla`](../specs/stw.tla) — phase 3.5 safepoint protocol. Verified against `NoRunningDuringCollect`, `UniqueCollector`, `PauseFlagCoherent`. Covers attach/detach lifecycle, collector-identity serialisation via the `collector = NoCollector` guard, and the attach-during-pause case.

Both specs ship negative-path sanity-check variants (documented in [`specs/README.md`](../specs/README.md)) — flipping one load-bearing action guard produces a TLC counterexample in seconds. The "Implementation-to-spec mapping" subsection above names the correspondence between TLA+ actions and the C++ code; when the code changes, that table is the first place to check whether the spec still models reality.

Future phases extend the specs rather than replace them — phase 4 concurrent mark will land with its own TLA+ spec covering the tri-color / write-barrier protocol, per the commitment in [`docs/design.md`](design.md).
