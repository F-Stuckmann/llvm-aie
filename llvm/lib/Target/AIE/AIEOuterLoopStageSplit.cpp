//===-- AIEOuterLoopStageSplit.cpp - Populate deferred OLP stages ---------===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
// Post-ISel pass for the outer-loop pipeliner's deferred skip-split stages.
//===----------------------------------------------------------------------===//

#include "AIE.h"
#include "AIEDataDependenceHelper.h"
#include "AIELiveRegs.h"
#include "Utils/AIELoopUtils.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineOptimizationRemarkEmitter.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineSSAUpdater.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"
#include <utility>

#define DEBUG_TYPE "aie-outer-loop-stage-split"

using namespace llvm;

static const char AIEOuterLoopStageSplitName[] = "AIE Outer Loop Stage Split";

namespace {

static void initializeDependencyContext(MachineSchedContext &Context,
                                        MachineFunction &MF,
                                        const MachineLoopInfo &MLI,
                                        AAResults &AA) {
  Context.MF = &MF;
  Context.MLI = &MLI;
  Context.AA = &AA;
}

class AIEOuterLoopStageSplit : public MachineFunctionPass {
  MachineRegisterInfo *MRI = nullptr;
  const TargetRegisterInfo *TRI = nullptr;

public:
  static char ID;
  AIEOuterLoopStageSplit() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    if (skipFunction(MF.getFunction()))
      return false;

    MachineLoopInfo &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();
    MachineOptimizationRemarkEmitter &ORE =
        getAnalysis<MachineOptimizationRemarkEmitterPass>().getORE();
    AAResults &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
    MRI = &MF.getRegInfo();
    TRI = MF.getSubtarget().getRegisterInfo();
    assert(MRI->isSSA() && "stage split must run before PHI elimination");

    MachineSchedContext Context;
    initializeDependencyContext(Context, MF, MLI, AA);
    AIE::DataDependenceHelper DAG(Context, /*AddMutators=*/false,
                                  /*ExactLatencies=*/false);

    bool Changed = false;
    SmallVector<MachineBasicBlock *> Latches;
    for (MachineBasicBlock &MBB : MF)
      if (AIELoopUtils::isOuterLoopPipelined(MBB))
        Latches.push_back(&MBB);

    for (MachineBasicBlock *Latch : Latches) {
      LLVM_DEBUG(dbgs() << "Found OLP Movement candidate: "
                        << printMBBReference(*Latch) << " (" << Latch->getName()
                        << ")\n");
      verifyOLPCFG(*Latch, MLI);
    }
    return Changed;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addRequired<MachineOptimizationRemarkEmitterPass>();
    AU.addRequired<AAResultsWrapperPass>();
    AU.addPreserved<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override { return AIEOuterLoopStageSplitName; }

private:
  /// Assert the CFG around a detected OLP latch matches the skeleton the
  /// pipeliner produces (see AIEOuterLoopPipeliner.cpp:36-62; naming at
  /// :1072,1160-1161,1177-1194):
  ///
  ///   [outer preheader] -> [stage0.top] -> [steady.stage1.top] <----\  header
  ///                                              |                   |
  ///                                     [steady inner loop]          |
  ///                                     backedge
  ///                                              |                   |
  ///                        [steady.stage1.bottom.and.stage0.top] ----/  latch
  ///                                              | (exit edge)
  ///                        [lastiter.stage1.top]
  ///                                              |
  ///                                     [lastiter inner loop]
  ///                                              |
  ///                        [lastiter.stage1.bottom] -> [exit]
  ///
  /// The steady side is identical across all OLP modes. The last-iteration
  /// region is only present in non-speculative modes; under speculative /
  /// lean-stage0 the latch exits straight to the function.
  void verifyOLPCFG(const MachineBasicBlock &Latch,
                    const MachineLoopInfo &MLI) {
    const MachineLoop *L = MLI.getLoopFor(&Latch);
    assert(L && L->getLoopLatch() == &Latch &&
           "OLP marker must sit on the steady loop's unique latch");

    [[maybe_unused]] const MachineBasicBlock *Header = L->getHeader();
    [[maybe_unused]] const MachineBasicBlock *Preheader = L->getLoopPreheader();
    assert(Preheader &&
           "OLP steady-state loop lacks the stage0.top preheader slot");

    // stage0.top slot: single entry from the outer preheader, single exit to
    // the steady header.
    assert(Preheader->pred_size() == 1 &&
           "stage0.top must have a single predecessor (outer preheader)");
    assert(Preheader->succ_size() == 1 && Preheader->isSuccessor(Header) &&
           "stage0.top must fall through only to the steady header");

    // steady.stage1.top header: entered exactly from the preheader and the
    // latch backedge; falls into the inner-loop region.
    assert(Header->pred_size() == 2 && Preheader->isSuccessor(Header) &&
           Latch.isSuccessor(Header) &&
           "steady header must be entered only from stage0.top and the latch");
    assert(Header->succ_size() == 1 && L->contains(*Header->succ_begin()) &&
           "steady header must fall into the inner-loop region");

    // steady.stage1.bottom.and.stage0.top latch: backedge to the header plus a
    // single out-of-loop exit; entered only from the inner-loop exit.
    assert(Latch.succ_size() == 2 && Latch.isSuccessor(Header) &&
           "steady latch must have a backedge and one exit successor");
    [[maybe_unused]] const MachineBasicBlock *Exit = nullptr;
    for (const MachineBasicBlock *Succ : Latch.successors())
      if (Succ != Header)
        Exit = Succ;
    assert(Exit && !L->contains(Exit) &&
           "steady latch's non-backedge successor must leave the loop");
    assert(Latch.pred_size() == 1 && L->contains(*Latch.pred_begin()) &&
           "steady latch must be entered only from the inner-loop exit");

    // Names the pipeliner assigns to the steady blocks.
    assert(Latch.getName().contains("steady.stage1.bottom") &&
           Header->getName().contains("steady.stage1.top") &&
           Preheader->getName().contains("stage0.top") &&
           "unexpected steady-loop block naming");

    // Last-iteration region: only in non-speculative modes.
    if (!Exit->getName().contains("lastiter"))
      return;

    // lastiter.stage1.top: lies outside the steady loop; entered only from the
    // steady latch; single fall-through into the peeled inner loop.
    [[maybe_unused]] const MachineBasicBlock *LastTop = Exit;
    assert(!L->contains(LastTop) &&
           "last-iteration region must lie outside the steady loop");
    assert(LastTop->pred_size() == 1 && Latch.isSuccessor(LastTop) &&
           "lastiter top must be entered only from the steady latch");
    assert(LastTop->succ_size() == 1 &&
           "lastiter top must fall into the peeled inner loop");

    // The peeled inner loop: a distinct loop whose preheader is lastiter top.
    [[maybe_unused]] const MachineLoop *IL =
        MLI.getLoopFor(*LastTop->succ_begin());
    assert(IL && IL != L && IL->getLoopPreheader() == LastTop &&
           "lastiter inner loop must be a distinct loop below lastiter top");

    // lastiter.stage1.bottom: the peeled loop's single exit; falls through once
    // to the function exit.
    [[maybe_unused]] const MachineBasicBlock *LastBottom = IL->getExitBlock();
    assert(LastBottom && !IL->contains(LastBottom) &&
           LastBottom->succ_size() == 1 &&
           "lastiter bottom must leave the peeled loop and fall through once");
    assert(LastTop->getName().contains("lastiter.stage1.top") &&
           LastBottom->getName().contains("lastiter.stage1.bottom") &&
           "unexpected last-iteration block naming");
  }
};

} // namespace

char AIEOuterLoopStageSplit::ID = 0;

INITIALIZE_PASS_BEGIN(AIEOuterLoopStageSplit, DEBUG_TYPE,
                      AIEOuterLoopStageSplitName, false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineOptimizationRemarkEmitterPass)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(AIEOuterLoopStageSplit, DEBUG_TYPE,
                    AIEOuterLoopStageSplitName, false, false)

namespace llvm {
MachineFunctionPass *createAIEOuterLoopStageSplitPass() {
  return new AIEOuterLoopStageSplit();
}
} // namespace llvm
