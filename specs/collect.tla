------------------------------ MODULE collect ------------------------------
(***************************************************************************)
(* An abstract model of dustman's collect() state machine, focused on the  *)
(* between-phase invariants that a single-threaded algorithm has to honour.*)
(*                                                                         *)
(* Motivation: even stop-the-world, single-mutator collect() is a state    *)
(* machine with non-trivial transitions.  Phase 3b-ii landed a bug where   *)
(* the recycle list from a previous collect survived into the evacuation  *)
(* phase of the current one, letting alloc_slow_small pop a block that    *)
(* had just been flagged for evacuation and use it as the evacuation      *)
(* target.  The result was that set_start / set_mark on the newly-placed  *)
(* object landed in the source block's bitmaps, which the same            *)
(* evacuate_block loop then rediscovered -- a self-iterating cascade that *)
(* ended in a chain of forwarded headers.  That class of bug is what TLA+ *)
(* catches: an invariant violation at a phase boundary, completely        *)
(* independent of concurrency.                                             *)
(*                                                                         *)
(* This spec models the collect() state machine at a high level -- phases *)
(* and block-set state, not individual objects or bitmaps -- and verifies *)
(* two safety invariants that the implementation relies on:                *)
(*                                                                         *)
(*   NoSelfEvacuation                                                      *)
(*     During the evacuating phase, no block currently being filled by    *)
(*     the evacuation tlab is also flagged for evacuation.  Equivalently: *)
(*     target_blocks and flagged are disjoint.                             *)
(*                                                                         *)
(*   RecycleCleanDuringEvac                                                *)
(*     During the evacuating and updating phases, no block on the small   *)
(*     recycle list is also flagged for evacuation.  The classify phase   *)
(*     enforces this by clearing the recycle list on exit; finalize       *)
(*     rebuilds it from survivors.                                         *)
(*                                                                         *)
(* Run with TLC using the companion collect.cfg.  If the invariant        *)
(* ClassifyAndClearRecycle below is replaced with its "forgot to clear"   *)
(* variant (kept in the file as ClassifyWithoutClearingRecycle for        *)
(* reference), TLC produces a counterexample demonstrating the recycle-  *)
(* list cascade.                                                           *)
(***************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS Blocks

VARIABLES
  phase,
  flagged,
  recycle,
  evacuated_srcs,
  target_blocks

Phases ==
  {"idle", "marking", "classifying", "evacuating", "updating", "finalizing"}

vars == <<phase, flagged, recycle, evacuated_srcs, target_blocks>>

TypeOK ==
  /\ phase \in Phases
  /\ flagged \subseteq Blocks
  /\ recycle \subseteq Blocks
  /\ evacuated_srcs \subseteq Blocks
  /\ target_blocks \subseteq Blocks

(*************************************************************************)
(* Initial state.  Recycle may be non-empty from a prior collect cycle.  *)
(*************************************************************************)
Init ==
  /\ phase = "idle"
  /\ flagged = {}
  /\ evacuated_srcs = {}
  /\ target_blocks = {}
  /\ recycle \in SUBSET Blocks

(*************************************************************************)
(* Phase transitions.                                                     *)
(*************************************************************************)

StartCollect ==
  /\ phase = "idle"
  /\ phase' = "marking"
  /\ UNCHANGED <<flagged, recycle, evacuated_srcs, target_blocks>>

MarkDone ==
  /\ phase = "marking"
  /\ phase' = "classifying"
  /\ UNCHANGED <<flagged, recycle, evacuated_srcs, target_blocks>>

(*************************************************************************)
(* Classify: flag some subset of blocks for evacuation.  Clearing the    *)
(* recycle list here is the load-bearing step -- without it, the         *)
(* evacuation phase can re-acquire a flagged block as an allocation      *)
(* target.                                                                *)
(*************************************************************************)
ClassifyAndClearRecycle(to_flag) ==
  /\ phase = "classifying"
  /\ to_flag \subseteq Blocks
  \* Model constraint: leave at least one non-flagged block available as an
  \* evacuation target.  In reality the allocator falls back to acquiring
  \* a fresh block from the OS (alloc_fresh_small_block), which the model's
  \* bounded universe can't represent directly; this constraint avoids a
  \* spurious deadlock in the "all blocks flagged simultaneously" state.
  /\ (Blocks \ to_flag) # {}
  /\ flagged' = to_flag
  /\ recycle' = {}
  /\ phase' = "evacuating"
  /\ UNCHANGED <<evacuated_srcs, target_blocks>>

(*************************************************************************)
(* Alternative Classify that does NOT clear the recycle list.  Unused in *)
(* Next below; documents the negative-path scenario.  Swapping           *)
(* ClassifyAndClearRecycle for this in Next produces an invariant        *)
(* violation -- the bug we hit in phase 3b-ii.                            *)
(*************************************************************************)
ClassifyWithoutClearingRecycle(to_flag) ==
  /\ phase = "classifying"
  /\ to_flag \subseteq Blocks
  /\ (Blocks \ to_flag) # {}
  /\ flagged' = to_flag
  /\ phase' = "evacuating"
  /\ UNCHANGED <<recycle, evacuated_srcs, target_blocks>>

(*************************************************************************)
(* Evacuate one source block.  The target is selected by alloc_slow_     *)
(* small: if the recycle list is non-empty, pop from it; otherwise       *)
(* acquire a fresh block not in flagged.                                  *)
(*                                                                        *)
(* This models the implementation faithfully: pop_small_recycled does    *)
(* NOT filter out flagged blocks.  If the recycle list were to contain   *)
(* a flagged block, that block would be returned, producing the          *)
(* cascading bug that motivates this spec.                                *)
(*************************************************************************)
EvacuateSource(src, tgt) ==
  /\ phase = "evacuating"
  /\ src \in flagged
  /\ src \notin evacuated_srcs
  /\ \/ tgt \in recycle
     \/ (tgt \in (Blocks \ flagged)) /\ (tgt \notin recycle)
  /\ evacuated_srcs' = evacuated_srcs \cup {src}
  /\ target_blocks' = target_blocks \cup {tgt}
  /\ recycle' = IF tgt \in recycle THEN recycle \ {tgt} ELSE recycle
  /\ UNCHANGED <<phase, flagged>>

EvacuationDone ==
  /\ phase = "evacuating"
  /\ evacuated_srcs = flagged
  /\ phase' = "updating"
  /\ UNCHANGED <<flagged, recycle, evacuated_srcs, target_blocks>>

UpdateDone ==
  /\ phase = "updating"
  /\ phase' = "finalizing"
  /\ UNCHANGED <<flagged, recycle, evacuated_srcs, target_blocks>>

(*************************************************************************)
(* Finalize: free all flagged blocks, rebuild the recycle list from      *)
(* survivors.  The new recycle list may include target blocks (they held *)
(* evacuation copies but are otherwise ordinary blocks) but must not     *)
(* include any flagged block since those are freed.                       *)
(*************************************************************************)
Finalize(new_recycle) ==
  /\ phase = "finalizing"
  /\ new_recycle \subseteq (Blocks \ flagged)
  /\ flagged' = {}
  /\ evacuated_srcs' = {}
  /\ target_blocks' = {}
  /\ recycle' = new_recycle
  /\ phase' = "idle"

Next ==
  \/ StartCollect
  \/ MarkDone
  \/ \E s \in SUBSET Blocks: ClassifyAndClearRecycle(s)
  \/ \E src \in flagged, tgt \in Blocks: EvacuateSource(src, tgt)
  \/ EvacuationDone
  \/ UpdateDone
  \/ \E r \in SUBSET Blocks: Finalize(r)

Spec == Init /\ [][Next]_vars

(*************************************************************************)
(* Safety properties.                                                     *)
(*************************************************************************)

NoSelfEvacuation ==
  (phase = "evacuating") => (target_blocks \cap flagged = {})

RecycleCleanDuringEvac ==
  (phase \in {"evacuating", "updating"}) => (recycle \cap flagged = {})

Invariant ==
  /\ TypeOK
  /\ NoSelfEvacuation
  /\ RecycleCleanDuringEvac

=============================================================================
