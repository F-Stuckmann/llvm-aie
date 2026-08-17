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
#include "Utils/AIELoopUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineSSAUpdater.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"
#include <optional>
#include <utility>

#define DEBUG_TYPE "aie-outer-loop-stage-split"

using namespace llvm;

static const char AIEOuterLoopStageSplitName[] = "AIE Outer Loop Stage Split";

namespace {

constexpr unsigned LongLatencyThreshold = 4;

/// The matching instructions of one stage-split candidate: the stage-0 copy in
/// Stage0Top and the steady-state copy in SteadyBottom.
struct InstructionPair {
  MachineInstr *Stage0MI = nullptr;
  MachineInstr *SteadyMI = nullptr;
  SUnit *Stage0SU = nullptr;
  SUnit *SteadySU = nullptr;
};

/// The four blocks of a deferred-split region, named on the stage0 / steady /
/// lastiter axis the outer-loop pipeliner uses when it emits them.
struct StageSplitBlocks {
  MachineBasicBlock *Stage0Top = nullptr;
  MachineBasicBlock *SteadyTop = nullptr;
  MachineBasicBlock *SteadyBottom = nullptr;
  MachineBasicBlock *LastIterTop = nullptr;
};

struct DependencyContext : MachineSchedContext {
  DependencyContext(MachineFunction &MF, const MachineLoopInfo &MLI,
                    AAResults &AA) {
    this->MF = &MF;
    this->MLI = &MLI;
    this->AA = &AA;
  }
};

class StageSplitLoop {
  MachineFunction &MF;
  MachineRegisterInfo &MRI;
  MachineBasicBlock &Stage0Top;
  MachineBasicBlock &SteadyTop;
  MachineBasicBlock &SteadyBottom;
  MachineBasicBlock &LastIterTop;
  SmallPtrSet<MachineBasicBlock *, 8> LastIterationBlocks;
  DependencyContext DAGContext;
  AIE::DataDependenceHelper Stage0DAG;
  AIE::DataDependenceHelper SteadyDAG;
  SmallVector<InstructionPair> Pairs;
  DenseMap<const MachineInstr *, unsigned> PairIndices;
  DenseSet<std::pair<Register, Register>> CorrespondingRegs;
  /// The SteadyTop PHI merging each (stage-0, steady) register pair.
  DenseMap<std::pair<Register, Register>, MachineInstr *> MergePHIs;
  DenseSet<unsigned> Selected;
  /// Old register -> its replacement defined in SteadyTop. Keyed both by the
  /// stage-0 definitions that moved and by the merge PHIs they made redundant.
  DenseMap<Register, Register> SteadyRewrite;
  DenseMap<Register, Register> LastIterationDefs;
  SmallVector<MachineInstr *, 8> ReplacedMergePHIs;

public:
  StageSplitLoop(MachineFunction &MF, const MachineLoopInfo &MLI, AAResults &AA,
                 const StageSplitBlocks &Blocks)
      : MF(MF), MRI(MF.getRegInfo()), Stage0Top(*Blocks.Stage0Top),
        SteadyTop(*Blocks.SteadyTop), SteadyBottom(*Blocks.SteadyBottom),
        LastIterTop(*Blocks.LastIterTop), DAGContext(MF, MLI, AA),
        Stage0DAG(DAGContext, /*AddMutators=*/true, /*ExactLatencies=*/true),
        SteadyDAG(DAGContext, /*AddMutators=*/true, /*ExactLatencies=*/true) {
    collectLastIterationBlocks(MLI);
  }

  bool run() {
    buildDAGs();
    collectRegisterCorrespondence();
    pairInstructions();
    selectProfitableRegions();
    if (Selected.empty() || !validateSelectedUses())
      return false;

    moveSelectedInstructions();
    return true;
  }

private:
  void collectLastIterationBlocks(const MachineLoopInfo &MLI) {
    LastIterationBlocks.insert(&LastIterTop);
    MachineBasicBlock *InnerHeader = *LastIterTop.succ_begin();
    const MachineLoop *InnerLoop = MLI.getLoopFor(InnerHeader);
    if (!InnerLoop)
      return;
    for (MachineBasicBlock *MBB : InnerLoop->blocks())
      LastIterationBlocks.insert(MBB);
    if (MachineBasicBlock *Exit = InnerLoop->getExitBlock())
      LastIterationBlocks.insert(Exit);
  }

  void buildDAGs() {
    Stage0DAG.buildGraph(Stage0Top);
    SteadyDAG.buildGraph(SteadyBottom);
  }

  bool haveCompatibleRegisters(Register TopReg, Register LatchReg) const {
    if (!TopReg || !LatchReg)
      return TopReg == LatchReg;
    if (TopReg.isPhysical() || LatchReg.isPhysical())
      return TopReg == LatchReg;
    return MRI.getRegClass(TopReg) == MRI.getRegClass(LatchReg) &&
           MRI.getType(TopReg) == MRI.getType(LatchReg);
  }

  bool haveEquivalentRegisterShape(const MachineOperand &TopMO,
                                   const MachineOperand &LatchMO) const {
    assert(TopMO.isReg() && LatchMO.isReg());
    if (TopMO.isDef() != LatchMO.isDef() ||
        TopMO.isImplicit() != LatchMO.isImplicit() ||
        TopMO.isUndef() != LatchMO.isUndef() ||
        TopMO.isInternalRead() != LatchMO.isInternalRead() ||
        TopMO.isEarlyClobber() != LatchMO.isEarlyClobber() ||
        TopMO.getSubReg() != LatchMO.getSubReg())
      return false;
    Register TopReg = TopMO.getReg();
    Register LatchReg = LatchMO.getReg();
    if (TopReg.isPhysical() != LatchReg.isPhysical())
      return false;
    if (TopReg.isPhysical())
      return TopReg == LatchReg && TopMO.isRenamable() == LatchMO.isRenamable();
    return haveCompatibleRegisters(TopReg, LatchReg);
  }

  /// Whether both instructions read the same values under the correspondence
  /// established so far. Only meaningful after haveEquivalentShape passed.
  bool haveCorrespondingRegisters(const MachineInstr &TopMI,
                                  const MachineInstr &LatchMI) const {
    for (auto [TopMO, LatchMO] :
         zip_equal(TopMI.operands(), LatchMI.operands())) {
      if (!TopMO.isReg())
        continue;
      Register TopReg = TopMO.getReg();
      if (!TopReg.isPhysical() && TopReg != LatchMO.getReg() &&
          !CorrespondingRegs.contains({TopReg, LatchMO.getReg()}))
        return false;
    }
    return true;
  }

  bool areEquivalent(const MachineInstr &TopMI,
                     const MachineInstr &LatchMI) const {
    return haveEquivalentShape(TopMI, LatchMI) &&
           haveCorrespondingRegisters(TopMI, LatchMI);
  }

  static bool isPairCandidate(const MachineInstr &MI) {
    return !MI.isPHI() && !MI.isTerminator() && !MI.isDebugInstr() &&
           !MI.isPosition();
  }

  bool haveEquivalentShape(const MachineInstr &TopMI,
                           const MachineInstr &LatchMI) const {
    if (TopMI.getOpcode() != LatchMI.getOpcode() ||
        TopMI.getFlags() != LatchMI.getFlags() ||
        TopMI.getNumOperands() != LatchMI.getNumOperands())
      return false;
    for (auto [TopMO, LatchMO] :
         zip_equal(TopMI.operands(), LatchMI.operands())) {
      if (TopMO.isReg() != LatchMO.isReg())
        return false;
      if (!TopMO.isReg() && !TopMO.isIdenticalTo(LatchMO))
        return false;
      if (TopMO.isReg() && !haveEquivalentRegisterShape(TopMO, LatchMO))
        return false;
    }
    return true;
  }

  void collectRegisterPair(Register TopReg, Register LatchReg) {
    if (!haveCompatibleRegisters(TopReg, LatchReg) || !TopReg.isVirtual() ||
        !CorrespondingRegs.insert({TopReg, LatchReg}).second)
      return;

    MachineInstr *TopDef = MRI.getVRegDef(TopReg);
    MachineInstr *LatchDef = MRI.getVRegDef(LatchReg);
    if (!TopDef || !LatchDef || TopDef->getParent() != &Stage0Top ||
        LatchDef->getParent() != &SteadyBottom ||
        !haveEquivalentShape(*TopDef, *LatchDef))
      return;
    for (auto [TopMO, LatchMO] :
         zip_equal(TopDef->operands(), LatchDef->operands()))
      if (TopMO.isReg() && !TopMO.isDef())
        collectRegisterPair(TopMO.getReg(), LatchMO.getReg());
  }

  void collectRegisterCorrespondence() {
    for (MachineInstr &PHI : SteadyTop.phis()) {
      auto Inputs = getPHIInputs(PHI);
      if (!Inputs)
        continue;
      MergePHIs.try_emplace(*Inputs, &PHI);
      collectRegisterPair(Inputs->first, Inputs->second);
    }
  }

  void pairInstructions() {
    auto LatchIt = SteadyBottom.begin();
    for (MachineInstr &TopMI : Stage0Top) {
      if (!isPairCandidate(TopMI))
        continue;
      auto Match = llvm::find_if(
          make_range(LatchIt, SteadyBottom.end()), [&](MachineInstr &LatchMI) {
            return isPairCandidate(LatchMI) && areEquivalent(TopMI, LatchMI);
          });
      if (Match == SteadyBottom.end())
        continue;

      SUnit *TopSU = Stage0DAG.getSUnit(&TopMI);
      SUnit *LatchSU = SteadyDAG.getSUnit(&*Match);
      if (!TopSU || !LatchSU)
        continue;
      unsigned Index = Pairs.size();
      Pairs.push_back({&TopMI, &*Match, TopSU, LatchSU});
      PairIndices[&TopMI] = Index;
      PairIndices[&*Match] = Index;
      LatchIt = std::next(Match);
    }
  }

  static bool isMovable(const MachineInstr &MI) {
    // isSafeToMove already rejects calls, inline asm, stores and ordered loads.
    bool SawStore = false;
    return !MI.isNotDuplicable() && !MI.isConvergent() &&
           MI.isSafeToMove(SawStore);
  }

  using DependencyEdge = std::pair<unsigned, unsigned>;

  bool getSuccessors(const InstructionPair &Pair, bool UseTop,
                     SmallVectorImpl<DependencyEdge> &Successors) const {
    const SUnit *SU = UseTop ? Pair.Stage0SU : Pair.SteadySU;
    for (const SDep &Dep : SU->Succs) {
      if (Dep.getSUnit()->isBoundaryNode())
        continue;
      MachineInstr *SuccMI = Dep.getSUnit()->getInstr();
      auto It = PairIndices.find(SuccMI);
      if (It == PairIndices.end())
        return false;
      Successors.emplace_back(It->second, static_cast<unsigned>(Dep.getKind()));
    }
    llvm::sort(Successors);
    Successors.erase(llvm::unique(Successors), Successors.end());
    return true;
  }

  bool collectSuffix(unsigned Index, DenseSet<unsigned> &Suffix) const {
    if (!Suffix.insert(Index).second)
      return true;
    const InstructionPair &Pair = Pairs[Index];
    if (!isMovable(*Pair.Stage0MI) || !isMovable(*Pair.SteadyMI))
      return false;

    SmallVector<DependencyEdge, 4> TopSuccessors;
    SmallVector<DependencyEdge, 4> LatchSuccessors;
    if (!getSuccessors(Pair, /*UseTop=*/true, TopSuccessors) ||
        !getSuccessors(Pair, /*UseTop=*/false, LatchSuccessors) ||
        TopSuccessors != LatchSuccessors)
      return false;
    for (const DependencyEdge &Successor : TopSuccessors)
      if (!collectSuffix(Successor.first, Suffix))
        return false;
    return true;
  }

  unsigned getWorstPredecessorLatency(const InstructionPair &Pair) const {
    const auto GetWorstLatency = [](const SUnit *SU) {
      unsigned WorstLatency = 0;
      for (const SDep &Dep : SU->Preds) {
        const SUnit *PredSU = Dep.getSUnit();
        if (!PredSU->isBoundaryNode())
          WorstLatency =
              std::max(WorstLatency, static_cast<unsigned>(PredSU->Latency));
      }
      return WorstLatency;
    };
    return std::max(GetWorstLatency(Pair.Stage0SU),
                    GetWorstLatency(Pair.SteadySU));
  }

  void selectProfitableRegions() {
    for (unsigned Index = 0; Index < Pairs.size(); ++Index) {
      const InstructionPair &Pair = Pairs[Index];
      const unsigned WorstLatency = getWorstPredecessorLatency(Pair);
      if (WorstLatency <= LongLatencyThreshold)
        continue;

      DenseSet<unsigned> Suffix;
      if (!collectSuffix(Index, Suffix))
        continue;
      Selected.insert(Suffix.begin(), Suffix.end());
    }
  }

  std::optional<std::pair<Register, Register>>
  getPHIInputs(const MachineInstr &PHI) const {
    Register TopReg;
    Register LatchReg;
    for (unsigned I = 1; I < PHI.getNumOperands(); I += 2) {
      Register Reg = PHI.getOperand(I).getReg();
      MachineBasicBlock *Incoming = PHI.getOperand(I + 1).getMBB();
      if (Incoming == &Stage0Top)
        TopReg = Reg;
      else if (Incoming == &SteadyBottom)
        LatchReg = Reg;
    }
    if (!TopReg || !LatchReg)
      return std::nullopt;
    return std::pair(TopReg, LatchReg);
  }

  MachineInstr *findMergePHI(Register TopReg, Register LatchReg) const {
    return MergePHIs.lookup({TopReg, LatchReg});
  }

  bool isSelectedInstruction(const MachineInstr *MI) const {
    auto It = PairIndices.find(MI);
    return It != PairIndices.end() && Selected.contains(It->second);
  }

  bool validateDefinitionUses(Register TopReg, Register LatchReg) const {
    MachineInstr *OutputPHI = findMergePHI(TopReg, LatchReg);
    for (MachineOperand &Use : MRI.use_nodbg_operands(TopReg)) {
      MachineInstr *UseMI = Use.getParent();
      if (!isSelectedInstruction(UseMI) && UseMI != OutputPHI)
        return false;
    }
    for (MachineOperand &Use : MRI.use_nodbg_operands(LatchReg)) {
      MachineInstr *UseMI = Use.getParent();
      if (!isSelectedInstruction(UseMI) && UseMI != OutputPHI &&
          !LastIterationBlocks.contains(UseMI->getParent()))
        return false;
    }
    return true;
  }

  bool validateSelectedUses() const {
    for (unsigned Index : Selected) {
      const InstructionPair &Pair = Pairs[Index];
      for (auto [TopMO, LatchMO] :
           zip_equal(Pair.Stage0MI->operands(), Pair.SteadyMI->operands())) {
        if (!TopMO.isReg() || !TopMO.isDef() || !TopMO.getReg().isVirtual())
          continue;
        if (!validateDefinitionUses(TopMO.getReg(), LatchMO.getReg()))
          return false;
      }
    }
    return true;
  }

  /// The name a moved instruction reads in the steady header: an earlier moved
  /// definition, else the value merged out of Stage0Top and SteadyBottom.
  Register getOrCreateSteadyInput(Register TopReg, Register LatchReg) {
    if (TopReg == LatchReg)
      return TopReg;
    if (Register Rewritten = SteadyRewrite.lookup(TopReg))
      return Rewritten;

    MachineSSAUpdater SSA(MF);
    SSA.Initialize(TopReg);
    SSA.AddAvailableValue(&Stage0Top, TopReg);
    SSA.AddAvailableValue(&SteadyBottom, LatchReg);
    return SSA.GetValueInMiddleOfBlock(&SteadyTop);
  }

  void remapUses(InstructionPair &Pair, MachineInstr &SteadyMI,
                 MachineInstr &LastMI) {
    for (auto [SteadyMO, LastMO, TopMO, LatchMO] :
         zip_equal(SteadyMI.operands(), LastMI.operands(),
                   Pair.Stage0MI->operands(), Pair.SteadyMI->operands())) {
      if (!SteadyMO.isReg() || SteadyMO.isDef() ||
          !SteadyMO.getReg().isVirtual())
        continue;

      Register TopReg = TopMO.getReg();
      Register LatchReg = LatchMO.getReg();
      SteadyMO.setReg(getOrCreateSteadyInput(TopReg, LatchReg));
      if (Register Moved = LastIterationDefs.lookup(LatchReg))
        LastMO.setReg(Moved);
      else if (Register Rewritten = SteadyRewrite.lookup(LatchReg))
        LastMO.setReg(Rewritten);
    }
  }

  void remapDefinitions(InstructionPair &Pair, MachineInstr &SteadyMI,
                        MachineInstr &LastMI) {
    for (auto [SteadyMO, LastMO, TopMO, LatchMO] :
         zip_equal(SteadyMI.operands(), LastMI.operands(),
                   Pair.Stage0MI->operands(), Pair.SteadyMI->operands())) {
      if (!SteadyMO.isReg() || !SteadyMO.isDef() ||
          !SteadyMO.getReg().isVirtual())
        continue;

      Register TopReg = TopMO.getReg();
      Register LatchReg = LatchMO.getReg();
      Register SteadyReg = MRI.cloneVirtualRegister(TopReg);
      Register LastReg = MRI.cloneVirtualRegister(LatchReg);
      SteadyMO.setReg(SteadyReg);
      LastMO.setReg(LastReg);
      SteadyRewrite[TopReg] = SteadyReg;
      LastIterationDefs[LatchReg] = LastReg;
      if (MachineInstr *PHI = findMergePHI(TopReg, LatchReg)) {
        SteadyRewrite[PHI->getOperand(0).getReg()] = SteadyReg;
        ReplacedMergePHIs.push_back(PHI);
      }
    }
  }

  void cloneSelectedInstructions() {
    auto SteadyInsert = SteadyTop.getFirstNonPHI();
    auto LastInsert = LastIterTop.getFirstNonPHI();
    for (unsigned Index = 0; Index < Pairs.size(); ++Index) {
      if (!Selected.contains(Index))
        continue;
      InstructionPair &Pair = Pairs[Index];
      MachineInstr *SteadyMI = MF.CloneMachineInstr(Pair.Stage0MI);
      MachineInstr *LastMI = MF.CloneMachineInstr(Pair.SteadyMI);
      SteadyTop.insert(SteadyInsert, SteadyMI);
      LastIterTop.insert(LastInsert, LastMI);
      remapUses(Pair, *SteadyMI, *LastMI);
      remapDefinitions(Pair, *SteadyMI, *LastMI);
      SteadyMI->cloneMergedMemRefs(MF, {Pair.Stage0MI, Pair.SteadyMI});
    }
  }

  void rewriteLastIterationUses() {
    for (auto [OldReg, NewReg] : LastIterationDefs) {
      SmallVector<MachineOperand *, 8> Uses;
      for (MachineOperand &Use : MRI.use_operands(OldReg))
        if (LastIterationBlocks.contains(Use.getParent()->getParent()))
          Uses.push_back(&Use);
      for (MachineOperand *Use : Uses)
        Use->setReg(NewReg);
    }
  }

  void replaceOutputPHIs() {
    for (MachineInstr *PHI : ReplacedMergePHIs) {
      Register OldReg = PHI->getOperand(0).getReg();
      MRI.replaceRegWith(OldReg, SteadyRewrite.lookup(OldReg));
      PHI->eraseFromParent();
    }
  }

  void eraseInstruction(MachineInstr *MI) {
    for (MachineOperand &Def : MI->defs())
      if (Def.getReg().isVirtual())
        MRI.markUsesInDebugValueAsUndef(Def.getReg());
    MI->eraseFromParent();
  }

  void eraseOriginalInstructions() {
    for (unsigned Index = Pairs.size(); Index-- > 0;) {
      if (!Selected.contains(Index))
        continue;
      eraseInstruction(Pairs[Index].Stage0MI);
      eraseInstruction(Pairs[Index].SteadyMI);
    }
  }

  void eraseDeadPHIs() {
    bool Changed;
    do {
      Changed = false;
      for (MachineInstr &PHI : make_early_inc_range(SteadyTop.phis())) {
        if (!PHI.isDead(MRI))
          continue;
        MRI.markUsesInDebugValueAsUndef(PHI.getOperand(0).getReg());
        PHI.eraseFromParent();
        Changed = true;
      }
    } while (Changed);
  }

  void moveSelectedInstructions() {
    cloneSelectedInstructions();
    replaceOutputPHIs();
    rewriteLastIterationUses();
    eraseOriginalInstructions();
    eraseDeadPHIs();
  }
};

class AIEOuterLoopStageSplit : public MachineFunctionPass {
public:
  static char ID;
  AIEOuterLoopStageSplit() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    if (skipFunction(MF.getFunction()))
      return false;

    MachineLoopInfo &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();
    AAResults &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
    assert(MF.getRegInfo().isSSA() &&
           "stage split must run before PHI elimination");

    bool Changed = false;
    SmallVector<MachineBasicBlock *> Latches;
    for (MachineBasicBlock &MBB : MF)
      if (AIELoopUtils::hasDeferredOuterLoopStageSplit(MBB))
        Latches.push_back(&MBB);

    for (MachineBasicBlock *Latch : Latches) {
      LLVM_DEBUG(dbgs() << "Found OLP Movement candidate: "
                        << printMBBReference(*Latch) << " (" << Latch->getName()
                        << ")\n");
      Changed |= splitDeferredLoop(MF, MLI, AA, *Latch);
    }
    return Changed;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addRequired<AAResultsWrapperPass>();
    AU.addPreserved<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override { return AIEOuterLoopStageSplitName; }

private:
  bool splitDeferredLoop(MachineFunction &MF, MachineLoopInfo &MLI,
                         AAResults &AA, MachineBasicBlock &SteadyBottom) {
    std::optional<StageSplitBlocks> Blocks =
        analyzeDeferredSplitCFG(SteadyBottom, MLI);
    if (!Blocks)
      return false;

    StageSplitLoop Split(MF, MLI, AA, *Blocks);
    return Split.run();
  }

  /// Resolve the four blocks of the deferred-split region around \p
  /// SteadyBottom; std::nullopt when the pipeliner's promised shape breaks.
  static std::optional<StageSplitBlocks>
  analyzeDeferredSplitCFG(MachineBasicBlock &SteadyBottom,
                          const MachineLoopInfo &MLI) {
    const MachineLoop *L = MLI.getLoopFor(&SteadyBottom);
    if (!L || L->getLoopLatch() != &SteadyBottom)
      return std::nullopt;

    MachineBasicBlock *SteadyTop = L->getHeader();
    MachineBasicBlock *Stage0Top = L->getLoopPreheader();
    if (!Stage0Top)
      return std::nullopt;

    // The stage-0 slot has one entry and falls through to the steady header.
    if (Stage0Top->pred_size() != 1 || Stage0Top->succ_size() != 1 ||
        !Stage0Top->isSuccessor(SteadyTop))
      return std::nullopt;

    // The steady header merges the stage-0 entry and the backedge, and falls
    // into the inner-loop region.
    if (SteadyTop->pred_size() != 2 || !SteadyBottom.isSuccessor(SteadyTop) ||
        SteadyTop->succ_size() != 1 || !L->contains(*SteadyTop->succ_begin()))
      return std::nullopt;

    // The latch has one inner predecessor, a backedge, and an exit.
    if (SteadyBottom.succ_size() != 2 || SteadyBottom.pred_size() != 1 ||
        !L->contains(*SteadyBottom.pred_begin()))
      return std::nullopt;

    // The non-backedge successor leaves the loop into the last iteration.
    MachineBasicBlock *LastIterTop = nullptr;
    for (MachineBasicBlock *Succ : SteadyBottom.successors()) {
      const bool IsBackedge = Succ == SteadyTop;
      if (!IsBackedge)
        LastIterTop = Succ;
    }
    if (!LastIterTop || L->contains(LastIterTop))
      return std::nullopt;

    // The last-iteration region is a distinct peeled loop below LastIterTop.
    if (LastIterTop->pred_size() != 1 || LastIterTop->succ_size() != 1)
      return std::nullopt;

    const MachineLoop *LastIterLoop =
        MLI.getLoopFor(*LastIterTop->succ_begin());
    if (!LastIterLoop || LastIterLoop == L ||
        LastIterLoop->getLoopPreheader() != LastIterTop)
      return std::nullopt;

    const MachineBasicBlock *LastIterBottom = LastIterLoop->getExitBlock();
    if (!LastIterBottom || LastIterLoop->contains(LastIterBottom) ||
        LastIterBottom->succ_size() != 1)
      return std::nullopt;

    // Names are derived from IR and can be stripped, so they only cross-check
    // the structural result; they never decide it.
    assert(SteadyBottom.getName().contains("steady.stage1.bottom") &&
           SteadyTop->getName().contains("steady.stage1.top") &&
           Stage0Top->getName().contains("stage0.top") &&
           LastIterTop->getName().contains("lastiter.stage1.top") &&
           LastIterBottom->getName().contains("lastiter.stage1.bottom") &&
           "unexpected deferred-split block naming");

    return StageSplitBlocks{Stage0Top, SteadyTop, &SteadyBottom, LastIterTop};
  }
};

} // namespace

char AIEOuterLoopStageSplit::ID = 0;

INITIALIZE_PASS_BEGIN(AIEOuterLoopStageSplit, DEBUG_TYPE,
                      AIEOuterLoopStageSplitName, false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(AIEOuterLoopStageSplit, DEBUG_TYPE,
                    AIEOuterLoopStageSplitName, false, false)

namespace llvm {
MachineFunctionPass *createAIEOuterLoopStageSplitPass() {
  return new AIEOuterLoopStageSplit();
}
} // namespace llvm
