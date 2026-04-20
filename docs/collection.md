# Collection semantics

This document captures the invariants, state machine, and failure modes of dustman's collection subsystem. It grows with each phase; today it covers through phase 3d (stop-the-world mark-sweep with opportunistic evacuation, multiple mutator threads, generational minor collect, auto-collect policy, visibility API). Later phases extend rather than replace.

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
- **`enter_native()` / `leave_native()` (primitive).** Consumer wraps blocking syscalls, `std::thread::join`, long external waits. Between `enter_native` and `leave_native` the thread counts as parked from the protocol's POV (`parked_count_` bumped, `native_` TLS flag set, TLABs retired), so the collector doesn't block waiting for it. `leave_native` blocks on `stw_cv_` if a pause is active, preventing the thread from running alongside the collector. `safepoint_slow` skips its park path when `native_` is set (would otherwise double-count). Not a policy — just a primitive the consumer calls at points only the consumer knows about.
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

The `enter_native` / `leave_native` primitive is not currently in the spec; it can be thought of as a specialisation of the `parked` state (increment `parked_count_`, wait for pause to clear before resuming), with `enter_native` firing regardless of `pause_req` and `leave_native` gated on `!pause_req`. Extending `stw.tla` with explicit native actions is a natural next step if the API grows interesting corner cases.

## Generational collection

Phase 3c adds a young generation on top of the mark-sweep-evacuate core. `dustman::minor_collect()` is a STW copy-evacuating nursery collect, complementing the full `dustman::collect()`. Minor cycles touch only young blocks (plus a dirty-card scan of old blocks and a full sweep of huge); major cycles still touch everything.

### Generations and the block tag

Each `BlockHeader` carries a one-byte `generation` field: `Young` or `Old`. `HugeRecord`s have no explicit tag and are treated as "always Old" for the purposes of minor. Fresh user allocations land in `Young` blocks; `minor_collect` copy-evacuates survivors into `Old` blocks. Major collect is currently generation-agnostic — its evac targets stay in the default (`Young`) generation, which is why the spec calls out major-vs-generational interplay as a follow-on.

`acquire_block(flags, gen)` is the single point that stamps a block's generation. Inside the allocator path, the thread-local `alloc_target_gen_` (default `Young`) determines the gen for fresh blocks; `minor_collect` toggles it to `Old` for the evac phase and restores it after.

### Write barrier

Every `gc_ptr<T>` copy/move constructor, copy/move assignment, and nullptr assignment calls `detail::gc_write_barrier(this)`. The barrier:

1. Masks the slot address down to the 32 KiB `block_alignment` to find the containing block base.
2. Looks up the base in `block_set_` (a `std::unordered_set<uintptr_t>` guarded by a `std::shared_mutex`, read-mostly).
3. If the base is registered, sets a byte in the block's `card_map` indexed by the slot's line (one card per 256 B line, matching `line_map`). If the base is not registered, the slot is on the stack / in a root / in a global — the barrier skips silently.

The barrier is unconditional on slot generation: we don't check whether the destination block is Old before marking. The branch would cost more than the occasional wasted card-write on a Young block, and the minor collector only reads Old-block cards, so Young marks are harmless.

The collector's own pointer writes — `gc_ptr_base::store` in the UpdateVisitor, and the raw `memcpy` during evacuation — bypass the barrier. This is correct: cards only need to track *mutator* stores between collects.

A thread-safe block-base set makes the barrier slow (50–100 ns per write) compared to a reserved-VA-arena design (~1–2 cycles). The trade is correctness-first for v1; future work is to mmap a contiguous heap arena so the containment check collapses to a single `addr - heap_base < heap_size`.

### Card table

`BlockHeader::card_map` is a flat array of `line_map_bytes` (= 128) bytes alongside `line_map`. `card_map[i] != 0` means "a mutator stored into a gc_ptr field somewhere in line `i` of this block since the last collect." After a minor collect clears every block's card_map, any subsequent dirty card was produced by a store during the intervening mutator work.

Cards are conservative. A dirty card means "at least one store happened here," not "there is currently a live old → young reference here." The collector rescans the whole card; any stale refs are harmless (pointers to already-dead young become null-iffed via their own forwarding, or simply aren't followed).

`BlockHeader`'s alignment bumped from 128 to `line_size` (256) so that `card_map`'s 128 bytes keep `block_header_size` a multiple of `line_size`. `lines_per_block` drops from 123 to 122 — a one-line hit.

### Minor collect pipeline

```
idle ──minor_collect()──▶ marking ──worklist drained──▶ sweeping ──reclaim done──▶ idle
```

In `sweeping` the steps are simpler than major's:

1. **Clear marks on young blocks** (only). Old-block marks are left alone — minor doesn't touch them.
2. **Mark phase.** `MinorMarkVisitor` is called on every registered root, every `gc_ptr` field of every live object in an old block whose containing card is dirty, and every `gc_ptr` field of every huge object. The visitor marks only Young pointees (skipping Old and Huge) and traces transitively through the young mark worklist.
3. **Evacuate.** TLABs are retired; `alloc_target_gen_` flips to `Old`; `alloc_skip_recycle_` is set so the evac path won't pop from `small_recycle_` (which contains Young blocks). `evacuate_block` is called on each young block in the pre-mark snapshot; `acquire_block` appends fresh old targets to `minor_evac_targets_` for the update phase.
4. **Update phase.** `MinorUpdateVisitor` is called on the same root set plus the newly-created old blocks. For each gc_ptr it reads: if the pointee is forwarded, rewrite to the new body; otherwise leave alone. No transitive walk is needed because every slot that *could* contain a forwarded young reference is reached directly (roots, dirty old-block cards, huge bodies, new evac target objects).
5. **Finalize.** `free_young_blocks_and_clear_cards` destroys non-forwarded (dead) objects in each young block via `destroy_non_forwarded_in`, frees the block memory, and clears `card_map` on every surviving block.

Young blocks are always freed after minor — their contents have either been evacuated or died. Any future young allocations come from fresh `acquire_block(..., Young)` calls.

### Minor-collect invariants

**`BarrierInvariant`:** at every mutator store into a `gc_ptr<T>`, if the destination slot lives in an Old block, that block's `card_map` entry for the slot's line is set. Verified by `specs/gen.tla` (`old_refs_young \subseteq dirty`).

**`PostMinorNoYoungLive`:** after `minor_collect` returns, no Young block remains in the heap. All survivors have been copied to Old blocks.

**`CardResetPostMinor`:** after `minor_collect` returns, every remaining block's `card_map` is zero.

**`SmallRecycleYoungOnly`:** the small recycle list contains only Young blocks. Fresh user allocations pop from it; minor evac does not. This is what keeps the user-alloc / minor-evac generations disjoint despite a shared recycle pool.

### Interaction with major collect

For v1, `collect()` is unchanged. Its evac targets default to the current `alloc_target_gen_` (= `Young`), so post-major, retained blocks mix the two generations: any block that was Old stays Old, any sparse block becomes a fresh Young evac target. Running `minor_collect` on that mixed state works — the minor collector treats every Young block as nursery regardless of its lineage — but the memory shape is suboptimal (long-lived data that happened to fit in a sparse block gets re-tenured on the next minor).

Promoting everything on major is a natural follow-on and fits under the same `alloc_target_gen_` plumbing.

### Auto-collect policy

Dustman decides when to collect; consumers don't have to. Two thresholds are checked at the top of each `alloc_slow_*` path (after the safepoint, guarded by `collecting_` and `auto_collect_enabled_`):

- **Minor trigger.** `bytes_since_last_minor_` is bumped by `block_body_size` at every young-generation `acquire_block` and by `alloc_size` at every `alloc_huge`. When it crosses `minor_threshold_bytes_` (default 4 MiB), the next `alloc_slow_*` calls `minor_collect()`. The counter resets to 0 at the end of every minor and major.
- **Major trigger.** At the end of each `minor_collect`, `count_old_block_bytes()` (old blocks' body bytes plus live huge bytes) is compared to `major_threshold_bytes_`. If crossed, a `needs_major_` latch fires so the next `alloc_slow_*` calls `collect()`. After that major runs, `major_threshold_bytes_` is recomputed as `max(current_old × major_growth_factor_percent_ / 100, major_min_bytes_)` — defaults 200% growth, 16 MiB minimum.

Net: a long-running mutator no longer needs to call `collect()` itself. Minor cycles fire at roughly 4 MiB of young allocation; major cycles fire when the old generation has doubled relative to its size at the previous major.

Guards:

- `collecting_` (thread-local) prevents the collector's own evac allocations from recursing into auto-trigger. Only mutator `alloc_slow_*` paths can fire an auto-collect.
- `auto_collect_enabled_` is a global atomic bool; `set_auto_collect_enabled(false)` disables both triggers. Explicit `dustman::collect()` / `dustman::minor_collect()` still work.

Tuning knobs (all `noexcept`, atomic-relaxed):

```cpp
void set_minor_threshold_bytes(std::size_t);
void set_major_growth_factor_percent(std::uint32_t);  // 200 = 2.0x
void set_major_min_bytes(std::size_t);
void set_auto_collect_enabled(bool);
```

`set_major_min_bytes` also clamps the currently-active `major_threshold_bytes_` up to the new minimum, so consumers can raise the floor mid-run without waiting for a major to retune.

The trigger checks are coarse (once per `alloc_slow_*`, not per `alloc<T>`), so the minor threshold can be overshot by up to one block's worth (32 KiB) before the collect fires. For the major trigger, the latch-based design means between a minor that crosses the threshold and the next `alloc_slow_*` the old generation may continue to grow — a bounded overshoot of up to one minor cycle's worth of promotion.

### Visibility API

Dustman maintains a small set of always-on counters for monitoring. Reading them is lock-free except for `heap_stats()`, which acquires the heap mutex briefly to snapshot block counts by generation.

```cpp
struct HeapStats {
  std::size_t minor_count;             // total minors since process start
  std::size_t major_count;             // total majors since process start
  std::size_t total_bytes_allocated;   // cumulative (never resets)
  std::size_t current_heap_bytes;      // young + old + huge, live at snapshot
  std::size_t current_young_bytes;
  std::size_t current_old_bytes;
  std::size_t huge_bytes;
  std::uint64_t last_minor_pause_us;   // STW body only, no park-wait
  std::uint64_t last_major_pause_us;
};

HeapStats heap_stats();
std::size_t get_minor_count();
std::size_t get_major_count();
std::uint64_t get_last_minor_pause_us();
std::uint64_t get_last_major_pause_us();
```

Pause timing brackets the collect body *after* `acquire_collector_slot` and *before* `release_collector_slot`, so it measures the time the collector was actually running — not time spent waiting for other threads to park. Units are microseconds via `std::chrono::steady_clock`.

`total_bytes_allocated` is cumulative (bumped at every `acquire_block` / `acquire_huge`), so delta-sampling gives an allocation rate. `current_*` fields reflect the moment `heap_stats()` was called.

### Implementation-to-spec mapping (gen.tla)

`specs/gen.tla` models only the barrier invariant; other concerns (pipeline phases, STW) are covered by `collect.tla` and `stw.tla`.

| TLA+ action | Implementation |
|---|---|
| `MutatorStoreOldToYoung(o)` | `gc_ptr<T>::operator=` / copy ctor / move ctor → `detail::gc_write_barrier(this)` → `h->card_map[line] = 1` |
| `MutatorOverwriteToOld(o)` | same write-barrier path (the barrier is unconditional on ref generation) |
| `BeginMinor` | `minor_collect()` through the mark phase |
| `EndMinor` | `free_young_blocks_and_clear_cards` — the atomic card/young reset |

The spec is abstract at block granularity: it has no notion of individual slots, live-object filtering, or huge records. Those are implementation concerns that the C++ test suite checks (`tests/test_minor.cpp`).

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
| 14 | Write barrier skipped on some `gc_ptr<T>` store path | Old → Young reference untracked; minor collector misses the young object and frees it under a live old slot | The barrier is in every `gc_ptr<T>::operator=` / copy ctor / move ctor (except the primitive raw-slot `store()` used only by the collector). Spec variant (`MutatorStoreOldToYoungNoBarrier` in `specs/gen.tla`) reproduces the bug in a two-state TLC counterexample. |
| 15 | Card table cleared at the *start* of minor instead of the end | Collector enters minor with `dirty = {}` while live old → young refs exist; all newly-young becomes unreachable and is freed | `minor_collect` clears `card_map` in `free_young_blocks_and_clear_cards` after the update phase. Spec variant (`BeginMinorClearsDirty`) reproduces the bug in a three-state counterexample. |
| 16 | Small recycle list contains Old blocks after a minor collect | Fresh user allocation pops an Old block and places a young-meant object into it; the next minor tries to evacuate that old block's contents as if they were young | `Heap::finalize_sweep` and `Heap::free_specific_blocks_and_clear_all_cards` filter to `generation == Young` when rebuilding `small_recycle_`. Regression-tested by the "preserves a young object reachable via old → young" case in `tests/test_minor.cpp`. |
| 17 | Minor evac target block selected from the young recycle list | Young-meant block acquires old survivor data; the subsequent minor treats it as young and loses the old data | `minor_collect` sets `alloc_skip_recycle_ = true` before the evac phase, forcing `alloc_slow_small` to go directly to `alloc_fresh_small_block`. |

## Consumer contract

- All GC-managed references outside the heap live in `Root<T>`. A stack-local `gc_ptr<T>` is not a root; its pointee may be collected.
- Tracers visit every `gc_ptr<T>` field of their type. `FieldList<T, ...>` handles the common case without hand-written bookkeeping.
- Tracers must not allocate.
- `collect()` must not be called from within another `collect()` or from a tracer.
- **Threads must `detach_thread()` before they exit.** A thread that exits while still attached leaves a dangling pointer in the global root registry; the next `collect()` dereferences it. `attach_thread()` is called implicitly on first use (first `alloc<T>`, first `Root<T>`, first `collect()`), so explicit attach is optional; explicit detach is mandatory.
- **Long-running loops that do not allocate must call `safepoint()` periodically.** The allocator's slow paths poll `safepoint()` for free, so allocation-heavy workloads reach safepoints naturally. Pure compute loops (number crunching, tight tracer-like walks in consumer code) can starve the collector indefinitely unless they poll.
- **A thread blocked in an external synchronisation primitive (`join`, `lock`, I/O) counts as attached-but-not-polling from the collector's POV and will block the pause.** Wrap the block in `dustman::enter_native()` / `dustman::leave_native()`: the thread is counted as parked during the block, so the collector proceeds without waiting for it. `leave_native` itself blocks on a cycle in progress, so the thread never resumes running alongside the collector.
- **Destructors of GC-managed types must not read from or dereference other GC-managed objects.** When the sweep phase destroys a fully-dead block, the order in which it destroys objects is unspecified, and any object a destructor might touch may itself already be destroyed. Destructors should free only non-GC resources; for most types with only `gc_ptr<T>` fields the compiler-generated destructor is already correct (it is trivial; it does not dereference).
- **GC-managed types must be trivially relocatable.** When the collector evacuates a sparsely-live block (phase 3b), it moves live objects to a new location via `memcpy` and abandons the old location without calling the destructor. For this to be safe, a bit-copy must produce a valid live object at the new location. Most sensible types satisfy this — plain aggregates, types with `gc_ptr<T>` fields and primitive members, types with trivially-relocatable RAII members (`std::unique_ptr`, `std::shared_ptr`, `std::string` without SBO, `std::vector` on most implementations). Types with **self-referential internal pointers** (`std::list`, `std::deque`, types storing `this` or the address of a member) are not trivially relocatable — put the bulk in a separately-allocated buffer and hold a `gc_ptr` to it instead. Where the compiler supports it, `TypeInfoFor<T>::value`'s instantiation `static_assert`s `__is_trivially_relocatable(T)`; otherwise the constraint is documented-only and the consumer is responsible.

Runtime-detectable violations of this contract abort.

## Formal model

Three TLA+ specs cover the state machines that have non-trivial between-phase invariants:

- [`specs/collect.tla`](../specs/collect.tla) — `collect()`'s inner pipeline (marking / classify / evacuate / update / finalize). Verified by TLC against `NoSelfEvacuation` and `RecycleCleanDuringEvac`. Landed alongside phase 3b-ii after a recycle-list-survives-into-evacuation bug produced a cascading self-iteration in the evacuator.
- [`specs/stw.tla`](../specs/stw.tla) — phase 3.5 safepoint protocol. Verified against `NoRunningDuringCollect`, `UniqueCollector`, `PauseFlagCoherent`. Covers attach/detach lifecycle, collector-identity serialisation via the `collector = NoCollector` guard, and the attach-during-pause case.
- [`specs/gen.tla`](../specs/gen.tla) — phase 3c generational write-barrier invariant. Verified against `BarrierInvariant` (`old_refs_young \subseteq dirty`). Narrower than the other two by design: the collection pipeline is already covered by `collect.tla` and the STW protocol by `stw.tla`, so `gen.tla` focuses exclusively on what the write barrier must guarantee.

All three specs ship negative-path sanity-check variants (documented in [`specs/README.md`](../specs/README.md)) — flipping one load-bearing action guard produces a TLC counterexample in seconds. The "Implementation-to-spec mapping" subsections above name the correspondence between TLA+ actions and the C++ code; when the code changes, those tables are the first place to check whether the spec still models reality.

Future phases extend the specs rather than replace them — phase 4 concurrent mark will land with its own TLA+ spec covering the tri-color protocol on top of the card-table barrier, per the commitment in [`docs/design.md`](design.md).
