--------------------------------- MODULE stw ---------------------------------
(***************************************************************************)
(* Safepoint protocol for dustman's phase 3.5 stop-the-world collector     *)
(* with multiple mutator threads.                                          *)
(*                                                                         *)
(* The collector's single-threaded sweep pipeline (modelled in             *)
(* collect.tla) assumes the heap is quiescent.  Phase 3.5 adds mutator    *)
(* threads that run concurrently; before the collector enters its sweep, *)
(* every attached mutator must be parked at a safepoint.  This spec       *)
(* models that coordination and proves the safety invariants the          *)
(* implementation needs to maintain.                                       *)
(*                                                                         *)
(* Threads are modelled as a fixed set Mutators, each in one of three    *)
(* lifecycle states:                                                       *)
(*                                                                         *)
(*   detached  thread is not attached; invisible to the collector         *)
(*   running   attached, executing mutator code                            *)
(*   parked    attached, waiting at a safepoint for the pause to clear   *)
(*                                                                         *)
(* Any running mutator can call collect() and thereby become the          *)
(* collector.  The scalar variable `collector` holds its id (or           *)
(* NoCollector); CAS-style serialisation is modelled by the action        *)
(* guard collector = NoCollector.  A thread that calls collect() while   *)
(* another cycle is already in progress does not become the collector --  *)
(* it simply parks at its first safepoint like any other mutator.         *)
(*                                                                         *)
(* Invariants:                                                             *)
(*                                                                         *)
(*   NoRunningDuringCollect                                                *)
(*     When col_state = "collecting", every attached non-collector        *)
(*     mutator is parked.  Equivalent to: the collector never touches    *)
(*     the heap alongside a running mutator.                               *)
(*                                                                         *)
(*   UniqueCollector                                                       *)
(*     At most one thread is the collector at any time.  col_state is    *)
(*     "idle" iff collector = NoCollector; otherwise the collector is    *)
(*     an attached mutator.                                                *)
(*                                                                         *)
(*   PauseFlagCoherent                                                     *)
(*     pause_req is TRUE iff the collector is in "waiting" or             *)
(*     "collecting".                                                       *)
(*                                                                         *)
(* Deliberately out of scope: write barriers (STW needs none); per-      *)
(* mutator TLAB state; which specific collector phases happen during     *)
(* "collecting" (collect.tla covers that).                                 *)
(*                                                                         *)
(* Run with TLC using the companion stw.cfg.  Sanity-check negative      *)
(* paths (see specs/README.md):                                            *)
(*                                                                         *)
(*   1. Replace CollectorBeginCollect's NonCollectorAttachedAllParked     *)
(*      guard with TRUE -- NoRunningDuringCollect violated in a few       *)
(*      steps.                                                              *)
(*   2. Make MutatorAttach unconditionally transition to "running"        *)
(*      (ignoring pause_req) -- NoRunningDuringCollect violated when a    *)
(*      thread attaches mid-cycle.                                         *)
(***************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS Mutators, NoCollector

VARIABLES
  mut_state,    \* function Mutators -> {"detached", "running", "parked"}
  col_state,    \* one of {"idle", "waiting", "collecting"}
  pause_req,    \* BOOLEAN
  collector     \* Mutators \cup {NoCollector}

vars == <<mut_state, col_state, pause_req, collector>>

MutStates == {"detached", "running", "parked"}
ColStates == {"idle", "waiting", "collecting"}

TypeOK ==
  /\ mut_state \in [Mutators -> MutStates]
  /\ col_state \in ColStates
  /\ pause_req \in BOOLEAN
  /\ collector \in (Mutators \cup {NoCollector})

Init ==
  /\ mut_state = [m \in Mutators |-> "detached"]
  /\ col_state = "idle"
  /\ pause_req = FALSE
  /\ collector = NoCollector

Attached(m) == mut_state[m] # "detached"

NonCollectorAttachedAllParked ==
  \A m \in Mutators: (Attached(m) /\ m # collector) => mut_state[m] = "parked"

(*************************************************************************)
(* Attach / detach.  A detached thread joining during a pause transitions *)
(* directly to "parked"; joining at any other time transitions to        *)
(* "running".  Detach is only allowed from "running" -- the collector   *)
(* cannot detach mid-cycle, and parked threads are blocked inside        *)
(* safepoint() until released.                                            *)
(*************************************************************************)
MutatorAttach(m) ==
  /\ mut_state[m] = "detached"
  /\ mut_state' = [mut_state EXCEPT
       ![m] = IF pause_req THEN "parked" ELSE "running"]
  /\ UNCHANGED <<col_state, pause_req, collector>>

MutatorDetach(m) ==
  /\ mut_state[m] = "running"
  /\ m # collector
  /\ mut_state' = [mut_state EXCEPT ![m] = "detached"]
  /\ UNCHANGED <<col_state, pause_req, collector>>

(*************************************************************************)
(* Mutator safepoint protocol.  A running mutator polls pause_req at its *)
(* next safepoint and parks if set; a parked mutator resumes only once   *)
(* pause_req is cleared.  The collector itself never parks -- m #        *)
(* collector on MutatorSafepoint -- otherwise every cycle deadlocks.     *)
(*************************************************************************)
MutatorSafepoint(m) ==
  /\ mut_state[m] = "running"
  /\ pause_req = TRUE
  /\ m # collector
  /\ mut_state' = [mut_state EXCEPT ![m] = "parked"]
  /\ UNCHANGED <<col_state, pause_req, collector>>

MutatorResume(m) ==
  /\ mut_state[m] = "parked"
  /\ pause_req = FALSE
  /\ mut_state' = [mut_state EXCEPT ![m] = "running"]
  /\ UNCHANGED <<col_state, pause_req, collector>>

(*************************************************************************)
(* Collector actions.  RequestPause serialises via collector =           *)
(* NoCollector; if two threads race to collect, one becomes the           *)
(* collector and the other falls through to MutatorSafepoint at its      *)
(* next poll.  BeginCollect fires only once every attached non-collector *)
(* is parked.  EndCollect atomically clears pause_req and releases the   *)
(* collector slot.                                                        *)
(*************************************************************************)
CollectorRequestPause(m) ==
  /\ mut_state[m] = "running"
  /\ col_state = "idle"
  /\ collector = NoCollector
  /\ col_state' = "waiting"
  /\ pause_req' = TRUE
  /\ collector' = m
  /\ UNCHANGED mut_state

CollectorBeginCollect ==
  /\ col_state = "waiting"
  /\ collector # NoCollector
  /\ NonCollectorAttachedAllParked
  /\ col_state' = "collecting"
  /\ UNCHANGED <<pause_req, mut_state, collector>>

CollectorEndCollect ==
  /\ col_state = "collecting"
  /\ collector # NoCollector
  /\ col_state' = "idle"
  /\ pause_req' = FALSE
  /\ collector' = NoCollector
  /\ UNCHANGED mut_state

Next ==
  \/ \E m \in Mutators: MutatorAttach(m)
  \/ \E m \in Mutators: MutatorDetach(m)
  \/ \E m \in Mutators: MutatorSafepoint(m)
  \/ \E m \in Mutators: MutatorResume(m)
  \/ \E m \in Mutators: CollectorRequestPause(m)
  \/ CollectorBeginCollect
  \/ CollectorEndCollect

Spec == Init /\ [][Next]_vars

(*************************************************************************)
(* Safety properties.                                                     *)
(*************************************************************************)

NoRunningDuringCollect ==
  (col_state = "collecting") =>
    \A m \in Mutators: (Attached(m) /\ m # collector) => mut_state[m] = "parked"

UniqueCollector ==
  /\ (col_state = "idle") <=> (collector = NoCollector)
  /\ (collector # NoCollector) => Attached(collector)

PauseFlagCoherent ==
  pause_req <=> (col_state \in {"waiting", "collecting"})

Invariant ==
  /\ TypeOK
  /\ NoRunningDuringCollect
  /\ UniqueCollector
  /\ PauseFlagCoherent

=============================================================================
