--------------------------------- MODULE gen --------------------------------
(***************************************************************************)
(* An abstract model of dustman's generational write-barrier and minor-    *)
(* collect invariant, at block granularity.                                *)
(*                                                                         *)
(* Phase 3c (proposed) adds a young generation and a copy-evacuating       *)
(* minor collect.  The write barrier marks a byte-sized card on every      *)
(* gc_ptr store; the minor collector uses dirty cards to discover old ->   *)
(* young references and treats those slots as extra mark roots.            *)
(* Correctness hinges on a single invariant:                               *)
(*                                                                         *)
(*   BarrierInvariant                                                      *)
(*     old_refs_young \subseteq dirty                                      *)
(*                                                                         *)
(*   Every old block that currently holds an old -> young reference has    *)
(*   its card marked dirty.  A missed barrier (store path that forgets to  *)
(*   mark the card) breaks this, and the collector would free a young      *)
(*   object that a live old slot still referenced.                         *)
(*                                                                         *)
(* This spec is deliberately narrower than collect.tla (collection         *)
(* pipeline) or stw.tla (safepoint protocol); those concerns are covered   *)
(* by their own specs.  Here we focus solely on the barrier / card state   *)
(* and the atomic reset performed by a minor collect.                      *)
(*                                                                         *)
(* Block generations are fixed at init.  In the real implementation a      *)
(* block can be freed and re-issued with a different generation, but that  *)
(* does not affect the barrier invariant -- what matters is that at any    *)
(* moment in "running", every currently-live old -> young ref has its      *)
(* source block in dirty.                                                  *)
(*                                                                         *)
(* Run with TLC using gen.cfg.  See specs/README.md for negative-path      *)
(* variants (MutatorStoreOldToYoungNoBarrier, BeginMinorClearsDirty) that  *)
(* TLC rejects.                                                            *)
(***************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS Blocks

VARIABLES
  phase,
  gen,
  old_refs_young,
  dirty

Phases == {"running", "minor_collecting"}

vars == <<phase, gen, old_refs_young, dirty>>

TypeOK ==
  /\ phase \in Phases
  /\ gen \in [Blocks -> {"young", "old"}]
  /\ old_refs_young \subseteq { b \in Blocks : gen[b] = "old" }
  /\ dirty \subseteq Blocks

(***************************************************************************)
(* Initial state.  Generations are assigned arbitrarily and fixed.  Both   *)
(* the old_refs_young set and the dirty set start empty.                   *)
(***************************************************************************)
Init ==
  /\ phase = "running"
  /\ gen \in [Blocks -> {"young", "old"}]
  /\ old_refs_young = {}
  /\ dirty = {}

(***************************************************************************)
(* Mutator actions (only in "running").                                    *)
(*                                                                         *)
(* MutatorStoreOldToYoung models a gc_ptr assignment whose source slot     *)
(* lives in an old-gen block and whose target is in a young-gen block.     *)
(* The write barrier marks the source block's card unconditionally.        *)
(*                                                                         *)
(* MutatorOverwriteToOld models overwriting the last remaining old ->      *)
(* young ref in block o with a same-generation (old) ref: o drops out of   *)
(* old_refs_young, but the card stays dirty (the barrier is unconditional  *)
(* on ref writes).                                                         *)
(***************************************************************************)

MutatorStoreOldToYoung(o) ==
  /\ phase = "running"
  /\ gen[o] = "old"
  /\ \E y \in Blocks : gen[y] = "young"
  /\ old_refs_young' = old_refs_young \cup {o}
  /\ dirty' = dirty \cup {o}
  /\ UNCHANGED <<phase, gen>>

MutatorOverwriteToOld(o) ==
  /\ phase = "running"
  /\ o \in old_refs_young
  /\ old_refs_young' = old_refs_young \ {o}
  /\ dirty' = dirty \cup {o}
  /\ UNCHANGED <<phase, gen>>

(***************************************************************************)
(* Negative-path variants.  Unused in Next below; documented here and in   *)
(* specs/README.md.  Each one, swapped into Next, produces a counter-      *)
(* example that violates BarrierInvariant.                                 *)
(***************************************************************************)

MutatorStoreOldToYoungNoBarrier(o) ==
  /\ phase = "running"
  /\ gen[o] = "old"
  /\ \E y \in Blocks : gen[y] = "young"
  /\ old_refs_young' = old_refs_young \cup {o}
  /\ UNCHANGED <<phase, gen, dirty>>

BeginMinorClearsDirty ==
  /\ phase = "running"
  /\ phase' = "minor_collecting"
  /\ dirty' = {}
  /\ UNCHANGED <<gen, old_refs_young>>

(***************************************************************************)
(* Minor collect.                                                          *)
(*                                                                         *)
(* BeginMinor enters the collecting phase with the dirty-card snapshot     *)
(* intact; the collector reads dirty to scan for old -> young refs.        *)
(* EndMinor models the atomic reset: all surviving young objects have      *)
(* been copied to old blocks, so no old -> young refs remain, and the      *)
(* card table is cleared for the next cycle.                               *)
(***************************************************************************)

BeginMinor ==
  /\ phase = "running"
  /\ phase' = "minor_collecting"
  /\ UNCHANGED <<gen, old_refs_young, dirty>>

EndMinor ==
  /\ phase = "minor_collecting"
  /\ phase' = "running"
  /\ old_refs_young' = {}
  /\ dirty' = {}
  /\ UNCHANGED gen

Next ==
  \/ \E o \in Blocks : MutatorStoreOldToYoung(o)
  \/ \E o \in Blocks : MutatorOverwriteToOld(o)
  \/ BeginMinor
  \/ EndMinor

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Safety properties.                                                      *)
(***************************************************************************)

BarrierInvariant ==
  old_refs_young \subseteq dirty

Invariant ==
  /\ TypeOK
  /\ BarrierInvariant

=============================================================================
