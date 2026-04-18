--------------------------------- MODULE stw ---------------------------------
(***************************************************************************)
(* Safepoint protocol for dustman's phase 3.5 stop-the-world collector     *)
(* with multiple mutator threads.                                          *)
(*                                                                         *)
(* The collector's existing single-threaded sweep pipeline (modelled in   *)
(* collect.tla) assumes the heap is quiescent.  Phase 3.5 adds mutator   *)
(* threads that run concurrently; before the collector enters its sweep, *)
(* every mutator must be parked at a safepoint.  This spec models that    *)
(* coordination and proves two invariants that the implementation needs   *)
(* to maintain:                                                            *)
(*                                                                         *)
(*   NoRunningDuringCollect                                                *)
(*     When the collector is in state "collecting", every mutator is      *)
(*     parked.  Equivalent to: no mutator can observe (or mutate) the     *)
(*     heap while the collector is touching it.                            *)
(*                                                                         *)
(*   PauseFlagCoherent                                                     *)
(*     pause_req is TRUE iff the collector is in state "waiting" or       *)
(*     "collecting".  Together with the action guards this prevents a    *)
(*     mutator from parking spuriously or from resuming before the        *)
(*     collector is done.                                                  *)
(*                                                                         *)
(* Deliberately out of scope for this first spec: attach/detach of        *)
(* mutator threads during a cycle (the model has a fixed Mutators set);   *)
(* write barriers (STW needs none); per-mutator TLAB state.  Those extend *)
(* the model later rather than landing in a single monolithic spec.       *)
(*                                                                         *)
(* Run with TLC using the companion stw.cfg.  To confirm the spec has    *)
(* teeth: replace CollectorBeginCollect's AllParked guard with TRUE and   *)
(* rerun -- TLC produces a one-step counterexample violating             *)
(* NoRunningDuringCollect.                                                 *)
(***************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS Mutators

VARIABLES
  mut_state,    \* function Mutators -> {"running", "parked"}
  col_state,    \* one of {"idle", "waiting", "collecting"}
  pause_req     \* BOOLEAN

vars == <<mut_state, col_state, pause_req>>

MutStates == {"running", "parked"}
ColStates == {"idle", "waiting", "collecting"}

TypeOK ==
  /\ mut_state \in [Mutators -> MutStates]
  /\ col_state \in ColStates
  /\ pause_req \in BOOLEAN

Init ==
  /\ mut_state = [m \in Mutators |-> "running"]
  /\ col_state = "idle"
  /\ pause_req = FALSE

AllParked ==
  \A m \in Mutators: mut_state[m] = "parked"

(*************************************************************************)
(* Mutator actions.  A running mutator polls pause_req at its next        *)
(* safepoint and parks if set.  A parked mutator resumes only when       *)
(* pause_req is cleared.                                                  *)
(*************************************************************************)
MutatorSafepoint(m) ==
  /\ mut_state[m] = "running"
  /\ pause_req = TRUE
  /\ mut_state' = [mut_state EXCEPT ![m] = "parked"]
  /\ UNCHANGED <<col_state, pause_req>>

MutatorResume(m) ==
  /\ mut_state[m] = "parked"
  /\ pause_req = FALSE
  /\ mut_state' = [mut_state EXCEPT ![m] = "running"]
  /\ UNCHANGED <<col_state, pause_req>>

(*************************************************************************)
(* Collector actions.  Request sets the pause flag; begin fires only      *)
(* once every mutator has reached a safepoint; end atomically clears the *)
(* flag and returns to idle.                                              *)
(*************************************************************************)
CollectorRequestPause ==
  /\ col_state = "idle"
  /\ col_state' = "waiting"
  /\ pause_req' = TRUE
  /\ UNCHANGED mut_state

CollectorBeginCollect ==
  /\ col_state = "waiting"
  /\ AllParked
  /\ col_state' = "collecting"
  /\ UNCHANGED <<pause_req, mut_state>>

CollectorEndCollect ==
  /\ col_state = "collecting"
  /\ col_state' = "idle"
  /\ pause_req' = FALSE
  /\ UNCHANGED mut_state

Next ==
  \/ \E m \in Mutators: MutatorSafepoint(m)
  \/ \E m \in Mutators: MutatorResume(m)
  \/ CollectorRequestPause
  \/ CollectorBeginCollect
  \/ CollectorEndCollect

Spec == Init /\ [][Next]_vars

(*************************************************************************)
(* Safety properties.                                                     *)
(*************************************************************************)

NoRunningDuringCollect ==
  (col_state = "collecting") => AllParked

PauseFlagCoherent ==
  pause_req <=> (col_state \in {"waiting", "collecting"})

Invariant ==
  /\ TypeOK
  /\ NoRunningDuringCollect
  /\ PauseFlagCoherent

=============================================================================
