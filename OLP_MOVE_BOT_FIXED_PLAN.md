# OLP: Move Post-Pipeliner Bottom-Fixed Prologue Into Predecessors — Plan & Status

Session: `gemm-prologue-split`
Branch: `stuckmann.olp-move-fixpoint` (based on `public/stuckmann.fixpoint.scheduling.2`)

## Goal

In an outer-loop-pipelined (OLP) loop nest, the outer-loop **header** (`for.body`, `.LBB0_1`
in the gemm test) is simultaneously the inner loop's preheader, so it carries the inner
loop's **bottom-fixed SWP prologue** band. Every steady-state outer iteration re-executes
that whole prologue, lengthening the steady state.

Relocate the leading `N` (default 6, `-aie-olp-move-bot-fixed-count`) cycles/bundles of that
prologue OUT of the header and INTO the header's predecessors:
- the **outer latch** (`for.cond.cleanup99`, `%bb.3`) — the steady-state predecessor; it ends
  in a `jnzd` conditional decrement-branch and already carries the inner-loop epilogue as a
  TOP-fixed band. The relocated loads co-issue with the epilogue drain here (the actual win).
- the **warm-up** predecessor — reached only on the first outer iteration ("not performance
  relevant"); it just needs a correctness copy.

Must be cycle-accurate: relocated instructions stay a fixed band; behind an off-by-default
flag; revert to the flag-off layout if timing can't be preserved.

## Verified idea

- The header prologue and the latch drain are the same problem the user's earlier
  cycle-regression question pointed at: pulling loads into the latch means letting a
  bottom-fixed prologue instruction co-issue in the same cycle as top-fixed epilogue-drain
  instructions.
- The `jnzd` latch's delay slots lead to loop EXIT on fall-through; the back-edge to the
  header is the taken path. So relocated loads must sit BEFORE the `jnzd`, not appended after.

## Foundation: why the fixpoint-scheduling branch

The gemm branch (`stuckmann.gemm_int8_1`) uses the OLDER *bundled* scheduling, where top-fixed
and bottom-fixed bands are pre-formed bundles in disjoint Top/Bot zones that CANNOT co-issue,
and pinning a band before a terminator needs backend-wide `ExitSU`/delay-slot surgery. That is
a dead end.

`stuckmann.fixpoint.scheduling.2` introduced **fixpoint scheduling** (`-aie-fixpoint-scheduling`,
default on, `AIEMachineScheduler.cpp:94-99`): fixed bands are emitted UNBUNDLED as instructions
at fixed cycles (cycle distances re-encoded as DAG edge latencies), free instructions may
co-issue into band bundles, and **dual-band regions are already supported** (one block carrying
both a top-fixed epilogue and a bot-fixed prologue). This dissolves both blockers.

Key anchors (fixpoint branch):
- Dual-band set on one region: `AIEInterBlockScheduling.cpp:1131-1163` (`enterRegion`).
- Dual-band commit/verify: `AIEMachineScheduler.cpp:1338-1393`; placement asserts
  `verifyFixedBandPlacement`/`isFixedBandPlacementValid`/`bandPlacedAt` at `:1231-1276`.
- Band geometry per region: `computeBandGeometry` `AIEInterBlockScheduling.cpp:1499-1509`.
- Bot band pinned to `ExitSU`: `chainBotFixedSUsToExitSU` `AIEBaseSubtarget.cpp:508-525`;
  `SchedulingLength` seed `seedSchedulingLength` `:529-553`.
- Terminator (`jnzd`) is a FREE delay-slot instr pinned via `reserveDelaySlotCycles` +
  `RegionBottomUpCycles` (`AIEMachineScheduler.cpp:467-489, 529-540`); no special-casing.
- Plumbing: `BlockState::TopInsert`/`BottomInsert`/`BottomInsertSemanticOrder`
  (`AIEInterBlockScheduling.h:263-271`), filled by `PipelineExtractor`
  (`AIEInterBlockScheduling.cpp:444-533`), driven from `leaveBlock` `PipeliningDone`
  (`:605-611`). At that point latch/header/warm-up are all still UNSCHEDULED (loops first).

## Plan (staged)

1. **Flags** — `-aie-olp-move-bot-fixed` (bool, default false), `-aie-olp-move-bot-fixed-count`
   (unsigned, default 6). [DONE]
2. **Before-terminator bottom emit** — `emitInterBlockBottom` inserts the band before
   `getFirstTerminator()` so a non-fall-through latch can carry a bot-fixed band.
   Behavior-preserving for fall-through blocks (`getFirstTerminator() == end()` there). [DONE]
3. **Relocation** — `relocateBotFixedToPredecessors(Prologue, Epilogue)`, called from
   `leaveBlock` `PipeliningDone` under the flag: gate on OLP steady state
   (`isEpilogueOfOuterPipelinedLoop()` on the latch predecessor == the inner-loop epilogue
   block); peel leading `N` bundles of the header's `BottomInsert`; clone into BOTH the
   warm-up (`BottomInsert`, fall-through) and the latch (`BottomInsert`, before-terminator);
   only then trim the header to `[N..end)`; rebuild `updatePerSuccEdges` for both preds;
   revert entirely if the gate/attachment fails or convergence/placement won't hold. [TODO]
4. **Verify + test** — flag-off strict no-op; flag-on: header shrinks, latch absorbs the loads
   co-issued into the drain, steady-state cycle count does not increase (ideally -N). Regenerate
   the gemm end-to-end CHECKs as a baseline-then-flip pair. [TODO]

## Two remaining constraints for Stage 3

1. **Before-terminator detection**: region bot-fixed detection currently keys off
   `RegionEnd == BB->end()` (`AIEInterBlockScheduling.cpp:1160`). For the latch it must key off
   the first terminator. (Stage 2 handled the *emit* side; the *detection* side is still TODO.)
2. **Per-block sub-band geometry**: bot-fixed band is assumed to be the trailing
   `BotFixedInstrCount` instrs of a single block (`bot_fixed_instrs()`
   `AIEInterBlockScheduling.h:175-180`, `setBotFixedBundles` assert `:1562`). Splitting the
   prologue across header + predecessors changes each block's `BotGeo.NumCycles`, the
   `chainBotFixedSUsToExitSU` latencies, the `SchedulingLength` seed, and the placement
   verifier. Each block computes geometry from its own band, so setting each block's
   `BottomInsert` correctly should mostly recompute per-block — watch `verifyFixedBandPlacement`
   (`AIEMachineScheduler.cpp:1271`); if it asserts, fix the split geometry, do NOT disable it.

## Current status

DONE and committed (builds clean; strict no-op — verified byte-identical gemm output with the
flag off AND on, since Stage 3 is not yet implemented):
- Stage 1: the two flags.
- Stage 2: `emitInterBlockBottom` generalized to insert before `getFirstTerminator()`.

TODO:
- Stage 3: the relocation itself (the two constraints above).
- Stage 4: verification + regenerated gemm CHECKs.

Note: the full `llvm-lit` harness was not run (needs additional build deps — `llvm-config`,
`llvm-readobj`, etc.); no-op was verified by (a) byte-identical gemm end-to-end output in both
flag states, and (b) the insertion point being identical for every current fall-through caller.

## Build & reproduce

    cd <this worktree>
    ninja -C build llc
    # Repro target (copy the gemm .ll from the gemm branch; on that branch it now lives at
    # llvm/test/CodeGen/AIE/aie2ps/end-to-end/gemm_int8_outerloop_pipelined_out_mode_1.ll):
    build/bin/llc -mtriple=aie2ps --aie-enable-outer-loop-pipelining \
      llvm/test/CodeGen/AIE/aie2ps/end-to-end/gemm_int8_1.ll -o -
    # Structure: %bb.0 entry ; .LBB0_1 for.body (header/prologue) ; .LBB0_2 for.body100 (inner
    # loop) ; %bb.3 for.cond.cleanup99 (latch, jnzd) ; %bb.4/.LBB0_5/%bb.6 cooldown.

The gemm `.ll` used for local repro is intentionally NOT committed (its CHECK lines were
generated under the older bundled scheduling and would fail FileCheck under fixpoint scheduling;
Stage 4 will regenerate them).
