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

Two negative-path edits exercise different pieces of the protocol:

 1. Replace `CollectorBeginCollect`'s `NonCollectorAttachedAllParked`
    guard with `TRUE`. TLC produces a five-state counterexample where
    the collector enters "collecting" while an attached mutator is
    still running — violating `NoRunningDuringCollect`.

 2. Change `MutatorAttach` to unconditionally transition to `"running"`
    (ignoring `pause_req`). TLC produces a counterexample where a
    thread attaches mid-cycle, goes straight to running while the
    collector is in "collecting", and violates the same invariant.
    This is a classic STW mistake: on attach during a pause, the new
    thread must join the parked cohort.

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
