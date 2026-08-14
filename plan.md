//===-- plan.md - hoist + dedup ---------===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates

# AIEOuterLoopStageSplit — hoist + dedup mechanism

## Context

`AIEOuterLoopStageSplit` currently only *verifies* the CFG skeleton the outer-loop
pipeliner produces in skip-split mode (it returns `false`, `setPreservesAll()`; the
whole body is asserts — `AIEOuterLoopStageSplit.cpp:33-149`). The pipeliner defers
populating stage 0: with `-aie-outer-loop-pipelining-skip-split` stage 0 is left
empty and the stage-1 prologue lives duplicated in **both** stage1.top blocks
(steady + lastiter). This pass is the deferred populator.

Goal: turn it into a transform that (Phase 1) hoists stage-1-top instructions out of
the steady header into its predecessors — the stage-0 slots — and (Phase 2) deletes
the now-redundant duplicate from the last-iteration header. A **profitability check
must run first** and gate the entire mechanism.

The concrete blocks (input-body numbering in the fixture
`aie2ps/hoist-dedup/gemm-int8-0-v2-stage-split.mir`):

```
bb.14.stage0.top ─────────────►┐  (preheader / stage0 slot for iter 0)
                               ▼
        ┌────────────► bb.15.steady.stage1.top ──► bb.17 (inner loop) ──┐
        │ backedge          (PHIs merge bb.14/bb.16)                     │
        │                                                                ▼
        └──────────── bb.16.steady.stage1.bottom.and.stage0.top ◄────────┘
                          │  (latch = stage0 slot for iter n+1)
                          │ exit edge
                          ▼
                    bb.10.lastiter.stage1.top ──► bb.11 (peeled loop) ──► exit
```

- `bb.15` preds = {`bb.14`, `bb.16`}; starts with a large PHI block.
- `bb.10` preds = {`bb.16`} only; no PHIs.
- `bb.15` and `bb.10` are **siblings** via the shared parent `bb.16`.
- Key fact: `bb.10` was built by the pipeliner as `bb.15`'s stage-1 evaluated with the
  **`bb.16`-edge (last-iteration) inputs** (`AIEOuterLoopPipeliner.cpp:1327-1354`,
  `seedLastIterInputs`). So where `bb.15` reads a PHI (`%207 = PHI %3,%bb.14, %403,%bb.16`),
  `bb.10` uses the `bb.16` operand directly (`%403`). This is *why* dedup is possible.

## Measured reference output (ground truth for the transform)

Non-skip-split OLP already populates stage 0, so it *is* the target shape. Regenerate with:

```
build/bin/llc -mtriple=aie2ps --aie-enable-outer-loop-pipelining \
    -stop-before=aie-outer-loop-stage-split -o reference.mir \
    llvm/test/CodeGen/AIE/aie2ps/end-to-end/gemm_int8_0_v2.ll
```

What it shows (register numbers are from that run; regenerate rather than trust them):

- `bb.15` retains only PHIs + six `VMUL`/`VADDMAC` + `LoopStart` + jump. Everything else —
  mode-register writes, `COPY`s, all loads, the `VSHIFT`/`VSEL` bias-rotation chain — sits in
  `bb.14` and `bb.16`.
- `bb.10` retains only `LoopStart` + the **same six arithmetic ops**, reading the `bb.16`
  namespace directly: `bb.15` has `%417 = VMUL %12, %400, %418` with
  `%400 = PHI %868,%bb.14, %887,%bb.16`, and `bb.10` has `%606 = VMUL %12, %887, %418`.
- So the reference **hoists the prologue but does *not* dedup the stage-1 arithmetic.** The six
  VMUL/VADDMAC stay duplicated between `bb.15` and `bb.10` by design — that duplication *is*
  the software pipeline.

Two consequences for this plan, both load-bearing:

1. The transform must **stop at the stage-0/stage-1 boundary**, not hoist everything that
   happens to have a `bb.10` partner. Pairing (below) is necessary, not sufficient — in the
   fixture the VMULs also have partners (`%315`/`%505`), so a pairing-only rule would hoist
   stage-1 compute into stage 0 and undo the split. See "Stopping criterion".
2. The bias-rotation chain (`REG_SEQUENCE`/`VSHIFT`/`VSEL_32`) and every load *are* in the
   hoist set in the reference. Any eligibility gate that excludes physreg operands or memory
   excludes the entire payload — see the scope note in assumption 1.

## Stopping criterion (which instructions are stage 0)

Open design question, to settle before implementing: this pass can either **re-derive** the
stage assignment by hoisting-with-pairing, or **import** the pipeliner's own stage-0 selection
(`AIEOuterLoopPipeliner.cpp`, the logic that non-skip-split mode already runs). Re-deriving
risks drifting from the reference in exactly the VMUL case above; importing keeps one owner for
the stage decision and makes this pass a pure mechanism. Preference: import, and treat the
hoist/dedup machinery below as the *executor* of that decision, with pairing as a safety check
rather than the selection rule.

## Feasibility verdict (Phase 2)

**Feasible in SSA, but not via BranchFolding.** The direct analog is
`HoistCommonCodeInSuccs` (`BranchFolding.cpp:1943-2154`) — literally "hoist common
insns from sibling successors into the common predecessor, delete the sibling copy."
Its guard set is our checklist. **But it requires `NoPHIs`
(`BranchFolding.cpp:109-111`) and runs post-PHI-elim**, so it sidesteps SSA. Our pass
runs *in SSA* (`AIE2TargetMachine.cpp:122`, before `PHIEliminationID`), confirmed by
PHIs + vregs in the fixture. There is **no reusable AIE-backend hoist/PHI helper**; we
introduce `MachineSSAUpdater` (first user in this backend).

**The reframing that makes Phase 2 clean.** "Same instruction" cannot be literal
`MachineInstr::isIdenticalTo` — it compares vregs *by number* (`MachineOperand.cpp:324-326`)
and the two blocks define different vregs / read different operands. Do **Phase 1
first with per-predecessor operand remap** (the `processPHI` pattern,
`TailDuplicator.cpp:355-385`): the instruction hoisted into `bb.16` is then expressed
in `bb.16`'s live-out register namespace — the same namespace `bb.10` already uses.
Phase-2 matching then reduces to `isIdenticalTo(IgnoreVRegDefs)` between the hoisted
`bb.16` instruction and `bb.10`'s instruction, and the delete is a `replaceRegWith`
(safe: `bb.16` is `bb.10`'s sole predecessor, so it dominates `bb.10`).

## Stated assumptions (confirm at approval)

1. **Scope (core):** vreg-defining instructions whose block-local dependences are either
   loop-invariant or a `bb.15` PHI (remapped per-predecessor), **including instructions with
   implicit physical-register operands** — mode/config reads (`$crsat`, `$crupsmode`,
   `$crsrsmode`, `$crrnd`, `$upssign1`, `$srssign0/1`, `$crbf8conf`, `$crfp8conf`), the
   explicit mode writes (`$crupsmode = MOVX_mvx_cr_imm 0`), and status-flag defs
   (`implicit-def $srups_of`, `implicit-def dead $srsrs_of`). These come from TableGen
   `Uses`/`Defs` lists (e.g. `AIE2PSGenInstrInfo.td:6907`: `Defs = [srUPS_of]`,
   `Uses = [crSat, crUPSMode, upsSign1]`), so they are ordinary implicit operands, not a
   special form. **Rationale for including them:** in the reference output every hoisted
   instruction carries them, and the survivors of a physreg-excluding gate
   (`MOV_RLC_imm11_pseudo`, `IMPLICIT_DEF`) are transitively blocked anyway because their
   consumers root at excluded mode-dependent defs — an excluding gate hoists nothing useful.
   Memory operands are subject to the same argument (the reference hoists all the loads); with
   per-instruction pairing (Phase 0) a hoisted load executes exactly as many times as before,
   so the speculation objection does not apply and only MMO handling remains. **Confirm at
   approval whether loads are in the first cut**; the safety rules below are written to cover
   them.
2. **Phase 1 = true hoist (move out of `bb.15`):** remove from `bb.15`, insert a merge
   PHI in `bb.15` (2 preds), rewrite uses. This matches "move to all parents" and
   fills both stage-0 slots.

## Algorithm

### Phase 0 — per-instruction profitability (gates each candidate; no mutation)

**Profitability is decided per instruction, not as an aggregate count.** An instruction is
hoisted only if it individually pays for itself; there is no global threshold and no
`cl::opt` count. An instruction `I` in `bb.15` is hoisted iff **all** hold:

1. `I` passes the eligibility gate (below).
2. `I` has a **dedup partner** `J` in `bb.10` that Phase 2 will delete. Without a partner the
   hoist is pure cost: `I` would execute once more (the final `bb.16` execution on the exit
   edge) with nothing removed, which for a load means an extra out-of-bounds access on a
   post-increment address stream (`%228 = VLD_x_pstm_nrm_imm_pseudo %203, 64`).
3. `I` is on the stage-0 side of the boundary (see "Stopping criterion") — pairing alone would
   also drag the stage-1 VMUL/VADDMAC chain across.
4. Every operand def `I` needs is itself hoisted. This makes the candidate set a **closure**:
   build one `DataDependenceHelper` graph for the steady header and walk backward from each
   load over assigned register-data edges. Reject that load's complete closure when any node
   fails eligibility; independent load roots remain eligible.

The shared DAG initializer excludes terminators; PHI data edges are traversal boundaries, as
are values defined outside the header. Memory-order, anti/output, barrier, artificial, and
target-mutation edges are not value prerequisites and are excluded from the closure.

**Why pairing makes the execution count exact.** `bb.15` executes `N` times and `bb.10` once,
so `I`+`J` execute `N+1` times today. After the transform: one clone in `bb.14` (once) plus one
clone in `bb.16` (`N` times, since `bb.16` runs once per steady iteration) and `J` deleted —
`N+1` again. Identical count, identical relative order within the hoisted group. This is what
makes physreg status-flag defs (`$srups_of`), mode writes, and loads safe to hoist: no
instruction is speculated, duplicated, or dropped. The count argument depends on the rotated
do-while shape (`N ≥ 1`, `bb.10` reached exactly once) already asserted by `verifyOLPCFG`.

If the closure is empty, bail with the MIR untouched.

### Eligibility gate (per instruction in `bb.15`)
Mirrors `HoistCommonCodeInSuccs`/`isSafeToMove`. Reject: terminators, PHIs, predicated
(`TII->isPredicated`), regmask/call, `isNotDuplicable`, convergent, side-effecting /
ordered-atomic (`MachineInstr::isSafeToMove`, `MachineInstr.cpp:1323-1368`).

**One uniform operand rule — no reserved/mode-register category.** Every operand of `I`,
virtual or physical, explicit or implicit, is traced the same way: find its reaching def and
require that def to be available at both insertion points, i.e. a member of the closure, or
defined outside the loop, or reachable via a `bb.15` PHI's per-predecessor incoming operand.
Mode and status registers get no special treatment — `$crupsmode`, `$crsat`, `$srups_of` are
just physical operands whose reaching def must be traced like any other.

Explicitly **do not** gate on `TRI->isSimplifiableReservedReg` (`AIE2PSRegisterInfo.cpp:775`).
It looks like the right category — its classes `mSCm`/`mCRm_file`
(`AIE2PSRegisterInfo.td:1138,1164`) do cover every mode and status register we touch — but it
is the wrong axis and, decisively, it is gated on the `-aie-simplify-crsr-edges` option
(`AIEBaseTargetMachine.cpp:146`). Gating a correctness condition on an optimization flag means
`-aie-simplify-crsr-edges=false` would silently change which hoists we consider legal.

**The one irreducible asymmetry: a physreg clone cannot be renamed.** Tracing unifies; cloning
does not. A cloned vreg def gets a fresh name from `MRI.createVirtualRegister`, so it cannot
disturb anything. A cloned physreg def writes *the same register*, so it is observable by every
instruction between the new position and the old one. Hence exactly one extra rule, and no
more:

> A hoisted group's physical-register defs must not change the value observed by any
> instruction outside the group.

Because the group is hoisted as a prefix in original program order, the interference window is
small and checkable with `LivePhysRegs` (`AIELiveRegs.cpp`, `ReservedRegsLICM.cpp`,
`AIESpillSlotOptimization.cpp` already use it) plus a clobber scan over that window. Concretely
in `bb.16`: inserting **before the terminators but after the existing body** puts our mode
writes after `bb.16`'s own `VST_SRS*` stores and its `$crsrsmode`/`$srssign0` writes, so those
are unaffected; what must be checked is that the group does not clobber a register `bb.16`'s
exit edges still rely on.

Note `ReachingDefAnalysis` — the generic utility that would make physreg tracing as mechanical
as vreg tracing — is **not available here**: it declares
`getRequiredProperties().setNoVRegs()` (`ReachingDefAnalysis.h:174-176`), so it is post-RA
only. The reaching-def scan is ours to write; `RegDefMap` in `ReservedRegsLICM.cpp:34-71` is
the miniature version of it and the natural thing to extract.
- **Ordering:** the closure is inserted in original `bb.15` program order, so mode-write →
  consumer ordering is preserved by construction. Never hoist terminators; keep them last.

### Phase 1 — hoist `I` (def `OldVReg`) from `bb.15` into `bb.14` and `bb.16`
For each predecessor `P ∈ {bb.14, bb.16}`:
1. Build a per-`P` operand map: for every PHI in `bb.15`, `map[phiDef] = P-incoming
   operand` (`processPHI` seed, `TailDuplicator.cpp:361-366`).
2. Clone `I` before `P`'s terminators, fresh dest `NewVReg_P =
   MRI.createVirtualRegister(RC)`, remap virtual-reg uses through the map
   (`duplicateInstruction`, `TailDuplicator.cpp:401-477`).
3. Record `(P, NewVReg_P)`.

Then one `MachineSSAUpdater` episode (recipe: `TailDuplicator.cpp:200-249`):
```
MachineSSAUpdater SSA(MF, &NewPHIs);
SSA.Initialize(OldVReg);
for (auto [P, NewVReg_P] : perPred) SSA.AddAvailableValue(P, NewVReg_P);
for (MachineOperand &U : make_early_inc_range(MRI.use_operands(OldVReg)))
    SSA.RewriteUse(U);            // inserts merge PHI at bb.15.begin() automatically
erase I from bb.15;
```
`RewriteUse` places the merge PHI in `bb.15` and picks the right per-pred value for
successor PHIs (`MachineSSAUpdater.cpp:229-256`).

### Phase 2 — dedup the sibling `bb.10`
Precondition already true here: `bb.10.pred_size()==1` and its pred is `bb.16`.
For the instruction `I16` just hoisted into `bb.16` (def `NewVReg_bb16`):
1. Find `J` in `bb.10` with `I16.isIdenticalTo(J, IgnoreVRegDefs)` (uses now share the
   `bb.16` namespace). This is the user's "sibling has the same instruction" +
   "exists in all parents (of bb.10 = {bb.16})" gate, made precise.
2. `MRI.replaceRegWith(J.defReg, NewVReg_bb16)` (constrain reg class), then erase `J`.
   Safe because `bb.16` dominates `bb.10`. Recompute live-ins as needed.

## Pitfalls to encode as guards (from tail-merge/tail-dup research)
- vreg-number identity is meaningless pre-dedup → rely on the Phase-1 remap + `IgnoreVRegDefs`.
- successor-PHI operands must be updated → handled by `MachineSSAUpdater::RewriteUse`.
- physreg / mode-register defs → in scope; enforce the liveness rules above with
  `LivePhysRegs` at each insertion point, plus `IMPLICIT_DEF` fixups where a hoisted def makes
  a register live into a block that did not have it live-in
  (`BranchFolding.cpp:1990-2028, 401-414`).
- memory ops → safe on execution count thanks to per-instruction pairing. **Phase 1 uses a
  plain clone; do not adjust memory operands.** `MachineFunction::CloneMachineInstr` carries the
  MMOs over unchanged, which is correct: our clone reaches a different address because its
  address *operand* was remapped (`%207` → `%403`), not because of an implicit per-stage
  offset. `ModuloScheduleExpander::updateMemOperands` (`ModuloSchedule.cpp`) solves a different
  problem — it shifts an MMO offset by `Delta * Num` for the `Num`-th unrolled copy of an
  instruction whose base operand is unchanged, and it early-returns on `Num == 0`. We have no
  stage index and no unchanged base, so it would be a no-op at best and wrong at worst. Merge
  MMOs only at the **dedup** step (below), never at the clone step.
- `bb.16` contains `VST_SRS*` stores, so a hoisted load landing after them in `bb.16` must not
  alias them — check MMOs rather than assuming the pipeliner's `!noalias` metadata carries over
  (`MachineInstr.cpp:1362-1365`, load-past-store).
- **out-of-block uses of a hoisted def get rewritten too** — see "Out-of-block uses" below.
- never hoist terminators / keep them last in `P`.

## PHI handling when instructions move

Two distinct PHI interactions, in opposite directions. Keeping them separate is the whole
mental model:

- **PHIs we read — "unwrapped" on the way down.** An existing `bb.15` PHI is *consumed* by the
  per-predecessor remap: cloning into `P` substitutes `map[phiDef] = P`'s incoming operand, so
  the clone reads `%3` in `bb.14` and `%403` in `bb.16` where the original read
  `%207 = PHI %3,%bb.14, %403,%bb.16`. No PHI is created, and the original PHI dies if the
  hoisted instruction was its last user.
- **PHIs we create — a merge on the way back up.** Any value still used *below* the hoist point
  needs its two per-predecessor definitions merged again, and that merge is a new PHI at the
  top of `bb.15`.

### What `MachineSSAUpdater` does for us

`RewriteUse` (`MachineSSAUpdater.cpp`) dispatches per use, and handles the cases that would
otherwise be hand-written:

- **A use inside a PHI is not a use in that PHI's block.** `RewriteUse` detects `UseMI->isPHI()`
  and resolves via `findCorrespondingPred(UseMI, &U)` + `GetValueAtEndOfBlockInternal(SourceBB)`
  — the value at the end of the *incoming* block. Getting this wrong by hand is the classic
  successor-PHI bug.
- **Lazy creation.** A PHI appears only where a use actually needs a merged value.
- **Single reaching value → no PHI.** A use in `bb.10` resolves through its sole predecessor
  `bb.16` to `NewVReg_bb16` directly.
- **Identical-PHI reuse.** `LookForIdenticalPHI(BB, PredValues)` returns an existing PHI with
  the same incoming pairs instead of adding a duplicate.
- **Constant-value PHI folding.** A PHI whose incoming values are all the same is erased via
  `isConstantValuePHI()` and the value used directly — relevant here because loop-invariant
  hoists produce exactly that shape.
- **Placement.** Inserted at `BB->begin()`, i.e. above the existing PHIs, so the PHI block stays
  contiguous and the machine verifier is satisfied.
- **Register classes.** It calls `constrainRegClass(NewVR, UseRC)` and, only if that fails,
  inserts a `COPY` at `getFirstNonPHI()`. Give the clones `MRI.getRegClass(OldVReg)` so this
  path stays unused.

### Measured cost — PHI count tracks the stage interface, not the instruction count

Comparing the frozen fixture's `bb.15` against the reference `bb.15` (both counted from the
blocks cited in "Measured reference output"):

| | fixture (before) | reference (after) |
|---|---|---|
| PHIs | 14 | 24 |
| total instructions | 80 | 32 |

66 non-PHI instructions leave the block and only **10** PHIs appear. So it is *not* one PHI per
hoisted instruction: most hoisted values are consumed only by other members of the closure and
never need re-merging. The resulting PHI count is the size of the **stage-0 → stage-1 register
interface** — the set of values live across the boundary — which is a property of where the cut
is made, not of how much moved. This is a useful invariant to assert in tests.

### Footguns to guard explicitly

- **A missing `AddAvailableValue` yields silent `IMPLICIT_DEF`, not an error.**
  `GetValueInMiddleOfBlock` materializes `IMPLICIT_DEF` at `getFirstTerminator()` for a
  predecessor with no available value. Since `bb.15` has exactly two predecessors (already
  asserted by `verifyOLPCFG`), assert that `AddAvailableValue` was called for both before any
  `RewriteUse`, otherwise a wrong-but-verifier-clean undef silently enters the loop.
- **Iterate uses with `make_early_inc_range`** — `RewriteUse` mutates the operand it is handed,
  which invalidates a plain `MRI.use_operands` walk.
- **Erase `I` from `bb.15` only after all its uses are rewritten**, so `MRI.use_operands` still
  enumerates them.
- **Subregister operands are not registers to remap.** `REG_SEQUENCE %222, %subreg.sub_256_lo`
  and `COPY %777.sub_256_lo` appear throughout the candidate set; the remap must touch only
  `MO.isReg() && MO.getReg().isVirtual()`, as the copied `duplicateInstruction` does.
- **Dead PHIs left behind.** Unwrapping can strand the original `bb.15` PHIs with no users. They
  are harmless but should be cleaned; note `AIEEliminateDuplicatePHI` exists in this backend and
  runs pre-legalization, i.e. too early to help us.

## Identity checking — how to decide `I16` and `J` are the same instruction

Four mechanisms, in the order they should be applied. The first is the matcher; the rest are
corroboration, and each catches a failure mode the syntactic match cannot.

**1. Hash-bucketed syntactic match — `MachineInstrExpressionTrait`.** Do not scan `bb.10`
linearly per candidate. `MachineInstr.h:2089-2107` defines a DenseMap trait whose `isEqual` is
exactly `isIdenticalTo(*RHS, MachineInstr::IgnoreVRegDefs)` and whose `getHashValue` hashes
opcode + operands ignoring defs. Build one
`DenseMap<MachineInstr *, …, MachineInstrExpressionTrait>` over `bb.10` and look each hoisted
instruction up — O(n) instead of O(n²), and it is the same primitive `MachineCSE` uses. This
only works *after* the Phase-1 remap has put `I16` in `bb.16`'s register namespace; before
that, vreg numbers make it meaningless.

**2. A separate physical-register check on top.** Syntactic equality of physreg operands is
not value equality — both instructions name `$crupsmode`, but that says nothing about what the
register holds at each site. `MachineCSE` treats this as a distinct step for exactly this
reason: `hasLivePhysRegDefUses` collects the physreg refs and `PhysRegDefsReach` verifies the
reaching defs agree (`MachineCSE.cpp:577,588`) *after* the DenseMap already matched. Mirror
that structure — never let `isIdenticalTo` alone decide a physreg-carrying instruction.

**3. Order-based pairing as the primary walk.** `bb.10` is a straight-line clone of `bb.15`'s
stage 1 in the same order, so a two-pointer walk in program order is more robust than a set
lookup: it disambiguates genuinely repeated instructions (`bb.15` has four
`%XXX:es = COPY %14`, `bb.10` has five), which a hash set cannot. Use the walk to propose
pairs and (1)+(2) to verify them.

**4. Provenance corroboration — assert, don't match on it.** `bb.10` was *generated* from
`bb.15`, so the strongest signal is provenance rather than syntax, and some of it survives into
MIR: the pipeliner's region suffixes appear in MMO IR values — `%ir.p_bias_in.0.steady` in
`bb.15` vs `%ir.p_bias_in.0.lastiter` in `bb.10`, `%ir.add.ptr.ascast.i.steady` vs
`%ir.add.ptr.ascast.i.lastiter`. Useful as an `assert`-level cross-check on a proposed pair for
memory ops. Do **not** match on names: they are debug-quality information, absent at `-g0`-like
settings and free to change.

The real fix for (4) is upstream of this pass: have `AIEOuterLoopPipeliner` record the
`bb.15` ↔ `bb.10` instruction correspondence explicitly when it clones, instead of making this
pass re-derive it syntactically. That is a larger change and is not required for the first cut,
but it is where the information actually lives.

**MMOs at the dedup step.** When Phase 2 erases `J`, the surviving `I16` covers both roles — it
executes `N` times where `I` ran `N` times with `J`'s single execution folded in. Its memory
operands must describe all of those executions, so merge them there with
`MachineInstr::cloneMergedMemRefs(MF, {I16, J})`. This is the *only* place MMOs are touched;
see the clone-step note above.

## Out-of-block uses of a hoisted def — resolved

**Motivation.** Phase 1 erases `I` from `bb.15` and lets `MachineSSAUpdater::RewriteUse`
retarget *every* user of `OldVReg`, not only users inside `bb.15`. `bb.10` already reads
`bb.15`-defined vregs directly: in the fixture, `%770:vec512 = REG_SEQUENCE %214, …`
(`gemm-int8-0-v2-stage-split.mir:1039`, defined in `bb.15`) is used by
`%794 = VSEL_32 %770, %789, %793` at `:906`, inside `bb.10`. Same for `%778` at `:1045`/`:907`.

**Main idea.** For a user in `bb.10`, SSAUpdater resolves through its sole predecessor `bb.16`
to `NewVReg_bb16` — the value corresponding to steady iteration *N+1*. Previously `bb.10` read
`bb.15`'s iteration-*N* value. `%770` is **not** loop-invariant; it sits on a recurrence
`%214 = PHI %596,%bb.14, %223,%bb.16` (`:1023`) → `%770` (`:1039`) → `%777` (`:1043`) →
`%223 = COPY %777.sub_256_lo` (`:1044`) → back into `%214`. So the two values genuinely differ,
and Phase 1 changes what `bb.10` computes as a side effect of the rewrite rather than through
the deliberate Phase 2 dedup. The question was whether that is a miscompile or the fix.

**Data.** The reference run above answers it directly. In non-skip-split output the analogous
`VSEL_32` chain is materialized **twice** — `%868` in `bb.14`/`bb.15`'s stage-0 slot and `%887`
in `bb.16` — with `bb.15` merging them via `%400 = PHI %868,%bb.14, %887,%bb.16` and **`bb.10`
using `%887`, the `bb.16` value** (`%606 = VMUL %12, %887, %418`). Skip-split output instead
has `bb.10` and `bb.15` sharing the single `bb.15`-resident `%770`. **Verdict: validated —
Phase 1's rewrite is the required behavior, not a hazard.** `bb.10` is the peeled iteration
*N+1* and must read iteration-*N+1* inputs; the skip-split CFG leaves it reading an
iteration-stale operand precisely because stage 0 has not been populated yet, and the hoist
converges it onto the reference. This is the same mechanism as `%207`/`%403`, just applied to a
value the pipeliner had not already redirected.

**Guard that still belongs in the code.** The convergence argument holds for users in the
last-iteration region reached through `bb.16`. It does not automatically hold for an arbitrary
out-of-block user elsewhere in the function. So: enumerate out-of-block users of `OldVReg`
before hoisting, accept those dominated by `bb.16` in the lastiter region, and reject the
candidate otherwise. Assert the accepted set matches the reference-derived expectation.

**Residual sub-question (not blocking).** Whether skip-split mode is a *live* miscompile today
for `%770`/`%778`, or merely an incomplete intermediate state that only this pass makes final,
is not settled — skip-split output is not expected to be correct standalone. It does not change
what this pass must do.

## Files
- `llvm/lib/Target/AIE/AIEOuterLoopStageSplit.cpp` — replace verify-only body with the
  Phase 0/1/2 transform; drop `setPreservesAll()` (now mutates: preserve nothing beyond
  what holds, keep `MachineLoopInfo` required). Add `#include
  "llvm/CodeGen/MachineSSAUpdater.h"`. Keep the existing `verifyOLPCFG` skeleton checks
  as `assert`-guarded preconditions before transforming.
- Reuse existing: `AIELoopUtils::isOuterLoopPipelined` (`AIELoopUtils.cpp:58-64`) for
  latch detection; `MachineLoop` pre/latch/header accessors already used.

### Reuse inventory

Used as-is, no changes anywhere: `DataDependenceHelper`, `LivePhysRegs.h`, `MachineSSAUpdater.h`,
`MachineInstr::isIdenticalTo(…, IgnoreVRegDefs)`, `MachineInstrExpressionTrait`,
`MachineInstr::cloneMergedMemRefs`.

Extract from `ReservedRegsLICM.cpp` into a shared AIE header: `RegDefMap` (`:34-71`),
`getSinglePhysRegDef`, `collectLoopReservedLiveins`, `moveInstruction` — currently in an
anonymous namespace, needed by both passes. AIE-local, no upstream dependency.

**Copied code, not reused code.** Phase 1's per-predecessor clone-and-remap is a
**verbatim-with-attribution copy** of `TailDuplicator::processPHI` and
`TailDuplicator::duplicateInstruction` (`TailDuplicator.cpp:355`, `:389`). It is a copy, and
the source comment must say so and name the origin. It is *not* a reuse and *not* a candidate
for an upstream visibility change: both methods write into `TailDuplicator`'s own SSA
bookkeeping via `addSSAUpdateEntry` → `SSAUpdateVRs`/`SSAUpdateVals`, so making them `public`
would hand a caller an object entangled with tail-duplication state. Real reuse would need an
upstream refactor into free functions; do not block this work on that. When copying, keep the
two functions recognisably close to the originals so the provenance stays checkable.

Deliberately not used: `BranchFolder::HoistCommonCodeInSuccs` (requires `NoPHIs`,
`BranchFolding.cpp:110`, and runs post-PHI-elim); `ReachingDefAnalysis` (requires `NoVRegs`,
post-RA only); `ModuloScheduleExpander::updateMemOperands` (solves per-stage MMO offsetting,
which we do not have — see the memory pitfall above); `AIEOuterLoopPipeliner`'s stage
classification (IR-level `FunctionPass` over `Instruction *`, `AIEOuterLoopPipeliner.cpp:173-187,518-522`
— nothing of it reaches MIR).

Separate, source-level follow-up: `ReservedRegsLICM` is the natural owner of loop-invariant
mode-register hoisting but declines this loop — `runOnLoop` bails on `L.getNumBlocks() != 1`
("TODO: Handle simple multi-BB loops"), and the steady loop has three blocks. Measured: after
`ReservedRegsLICM` runs, `bb.6.steady.stage1.top` still contains both
`$crupsmode = MOVX_mvx_cr_imm 0` and `$crsrsmode = MOVX_mvx_cr_imm 0`. Lifting that TODO would
shrink this pass's job but not eliminate it — `processForPreheaderHoist` also requires a unique
def in the loop, and `$crsrsmode` is written in both `bb.15` and `bb.16`.

## Verification
0. **Primary check — converge on the reference.** Run `gemm_int8_0_v2.ll` through both modes
   and compare the post-pass MIR against the non-skip-split reference (command in "Measured
   reference output"). Compare *structurally*, not textually — vreg numbering will differ —
   by asserting per block: which opcodes remain in `bb.15`, `bb.10`, `bb.14`, `bb.16`, and that
   `bb.10`'s surviving operands resolve to `bb.16`-namespace defs. Any instruction this pass
   leaves in `bb.15` that the reference put in stage 0 is a coverage gap; any instruction it
   hoists that the reference kept in `bb.15` (the six VMUL/VADDMAC) is a stopping-criterion
   bug. This catches both failure directions; regenerated CHECK lines catch neither.
1. Unit test (extend the existing fixture): the frozen
   `aie2ps/hoist-dedup/gemm-int8-0-v2-stage-split.mir` — regenerate CHECK lines with
   `utils/update_mir_test_checks.py` after the transform; assert the mode-write + load
   prologue appears in `bb.14`/`bb.16`, merge PHIs appear in `bb.15`, and the duplicates are
   gone from `bb.10` while the six VMUL/VADDMAC remain there. Add a negative fixture where a
   candidate exists but has **no** `bb.10` partner → MIR unchanged (the interesting bail, and
   the direct test of per-instruction profitability).
2. `llc -mtriple=aie2ps -run-pass=aie-outer-loop-stage-split -verify-machineinstrs`
   must pass the machine verifier (SSA still valid, PHIs well-formed).
3. Build `llc`; run the AIE codegen lit suite
   (`llvm/test/CodeGen/AIE/aie2ps/...`) to confirm no regressions, especially the
   speculative/lean-stage0 path (no lastiter region → Phase 2 is a no-op). Note the blast
   radius: the pass is live in the aie2 pipeline at `-O1+`
   (`llvm/lib/Target/AIE/aie2/AIE2TargetMachine.cpp:122`), so making it mutate churns frozen
   CHECK lines across the whole suite, not only OLP tests. Expect and budget for that churn.
4. Per repo convention: land the baseline/test commit first ([Baseline]), then the
   transform commit; ensure unit tests pass before each commit.
