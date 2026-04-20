# TLA+ specifications

Formal specs of dustman's state machines, maintained alongside the code.

## Files

- **`collect.tla`** — abstract model of `dustman::collect()`'s phases and the
  between-phase invariants (recycle-list cleanliness during evacuation,
  no self-evacuation). Catches the class of invariant-violation bug hit in
  phase 3b-ii.
- **`collect.cfg`** — TLC configuration for `collect.tla`.
- **`stw.tla`** — phase 3.5 safepoint protocol. Models mutator threads
  polling a pause flag and parking, attach/detach lifecycle, and
  collector identity (any running thread can call collect(); the first
  wins via the `collector = NoCollector` guard, others fall through to
  a safepoint park). Invariants: `NoRunningDuringCollect`,
  `UniqueCollector`, `PauseFlagCoherent`.
- **`stw.cfg`** — TLC configuration for `stw.tla` (three mutator threads).
- **`gen.tla`** — phase 3c (proposed) generational write-barrier and minor-
  collect invariant, at block granularity. Models the single safety
  property that matters: every live old → young reference has its source
  card marked dirty. Narrower than `collect.tla` by design — the
  collection pipeline and STW protocol are covered by their own specs.
  Invariant: `BarrierInvariant`.
- **`gen.cfg`** — TLC configuration for `gen.tla` (three blocks).

## Running

### With the TLA+ Toolbox (GUI, easiest)

Download from <https://lamport.azurewebsites.net/tla/toolbox.html>. Open
`collect.tla`, create a new TLC Model Checker model, set the configuration
from `collect.cfg`, run.

### With `tla2tools.jar` (command line)

```bash
# Download: https://github.com/tlaplus/tlaplus/releases
java -XX:+UseParallelGC -jar tla2tools.jar -config collect.cfg collect.tla
```

Expected output on a clean spec:

```
Model checking completed. No error has been found.
```

### Reproducing the phase 3b-ii bug

Edit `collect.tla`, replace the `ClassifyAndClearRecycle(s)` call in `Next`
with `ClassifyWithoutClearingRecycle(s)`. Rerun TLC. It finds a minimal
counterexample in under a second: a sequence of states where the recycle
list survives into the evacuation phase, `EvacuateSource` pops a
flagged-for-evacuation block out of it, and `NoSelfEvacuation` is
violated.

### Sanity-checking the STW spec

Three negative-path edits exercise different pieces of the protocol:

 1. Replace `CollectorBeginCollect`'s `NonCollectorAttachedNotRunning`
    guard with `TRUE`. TLC produces a five-state counterexample where
    the collector enters "collecting" while an attached mutator is
    still running — violating `NoRunningDuringCollect`.

 2. Change `MutatorAttach` to unconditionally transition to `"running"`
    (ignoring `pause_req`). TLC produces a counterexample where a
    thread attaches mid-cycle, goes straight to running while the
    collector is in "collecting", and violates the same invariant.
    This is a classic STW mistake: on attach during a pause, the new
    thread must join the parked cohort.

 3. Drop the `pause_req = FALSE` guard from `MutatorLeaveNative`. TLC
    finds a seven-step counterexample where a thread leaves native
    back to running while the collector is still in "collecting".
    `leave_native` must wait for the in-flight cycle to finish before
    returning to mutator code.

### Sanity-checking the gen spec

Two negative-path edits exercise the write-barrier and card-reset
invariants:

 1. Replace `MutatorStoreOldToYoung(o)` with
    `MutatorStoreOldToYoungNoBarrier(o)` in `Next`. TLC produces a
    two-state counterexample where an old → young reference appears
    with no corresponding dirty card — a missed barrier, which would
    cause the minor collector to miss the young object and free it
    under a live old-gen slot.

 2. Replace `BeginMinor` with `BeginMinorClearsDirty` in `Next`. TLC
    produces a three-state counterexample where the collector enters
    `minor_collecting` with `dirty = {}` but `old_refs_young = {b3}` —
    the dirty-card snapshot has been wiped before the collector could
    read it, so live cross-gen refs are invisible.

## When to write a new spec

Any time dustman grows a state machine with between-phase invariants that
are not immediately obvious — especially anything involving concurrency
(phase 3.5 multi-mutator, phase 4 concurrent mark), but also for
single-threaded state machines that coordinate across multiple data
structures (this spec is an example).

The design doc (`docs/design.md`) commits to writing and maintaining
specs for each load-bearing concurrent protocol; phase 3b showed this
discipline is worth extending to sufficiently-intricate single-threaded
protocols too.
