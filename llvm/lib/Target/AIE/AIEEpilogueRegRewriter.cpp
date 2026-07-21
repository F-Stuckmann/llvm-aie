//===-- AIEEpilogueRegRewriter.cpp - Rename epilogue definitions --------===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//

#include "AIE.h"
#include "AIEBaseInstrInfo.h"
#include "AIEBaseRegisterInfo.h"
#include "AIESuperRegUtils.h"
#include "Utils/AIELoopUtils.h"
#include "Utils/AIERegAllocationUtils.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/LiveDebugVariables.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "aie-epilogue-reg-rewriter"

static cl::opt<unsigned> EpilogueCopyBudget(
    "aie-epilogue-reg-rewrite-copy-budget", cl::Hidden, cl::init(4),
    cl::desc("Maximum materialized copy instructions per epilogue"));

namespace {

struct RewriteCandidate {
  Register OldReg;
  MCPhysReg OldPhys;
  const TargetRegisterClass *RC;
  SmallVector<MachineOperand *, 4> Defs;
  MachineInstr *FinalDef;
  MachineBasicBlock *Epilogue;
};

class AIEEpilogueRegRewriter : public MachineFunctionPass {
public:
  static char ID;

  AIEEpilogueRegRewriter() : MachineFunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<VirtRegMapWrapperLegacy>();
    AU.addPreserved<VirtRegMapWrapperLegacy>();
    AU.addRequired<SlotIndexesWrapperPass>();
    AU.addPreserved<SlotIndexesWrapperPass>();
    AU.addRequired<LiveIntervalsWrapperPass>();
    AU.addPreserved<LiveIntervalsWrapperPass>();
    AU.addRequired<LiveRegMatrixWrapperLegacy>();
    AU.addPreserved<LiveRegMatrixWrapperLegacy>();
    AU.addRequired<LiveDebugVariablesWrapperLegacy>();
    AU.addPreserved<LiveDebugVariablesWrapperLegacy>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  std::optional<RewriteCandidate>
  collectCandidate(Register Reg, MachineBasicBlock &Epilogue,
                   MachineRegisterInfo &MRI, const AIEBaseRegisterInfo &TRI,
                   VirtRegMap &VRM, LiveIntervals &LIS) const;

  SmallVector<RewriteCandidate, 4>
  collectCandidates(MachineFunction &MF, MachineRegisterInfo &MRI,
                    const AIEBaseRegisterInfo &TRI, VirtRegMap &VRM,
                    LiveIntervals &LIS, SlotIndexes &Indexes) const;

  MCPhysReg findReplacementPhysReg(const RewriteCandidate &Candidate,
                                   const AIEBaseRegisterInfo &TRI,
                                   LiveIntervals &LIS, LiveRegMatrix &LRM,
                                   SlotIndexes &Indexes,
                                   BitVector &ReservedRegUnits) const;

  void commitRewrite(RewriteCandidate &Candidate, MCPhysReg NewPhys,
                     MachineRegisterInfo &MRI, const AIEBaseInstrInfo &TII,
                     VirtRegMap &VRM, LiveRegMatrix &LRM, LiveIntervals &LIS,
                     LiveDebugVariables &DebugVars) const;
};

bool isPermittedPreservingUse(const MachineInstr &MI, unsigned UseIndex,
                              unsigned DefIndex) {
  const MachineOperand &Use = MI.getOperand(UseIndex);
  if (Use.isImplicit())
    return true;
  return Use.isTied() && MI.findTiedOperandIdx(UseIndex) == DefIndex;
}

SmallVector<MachineOperand *, 2> getPartialDefs(MachineInstr &MI,
                                                Register Reg) {
  SmallVector<MachineOperand *, 2> Defs;
  for (MachineOperand &MO : MI.operands()) {
    if (MO.isReg() && MO.isDef() && MO.getReg() == Reg && MO.getSubReg())
      Defs.push_back(&MO);
  }
  return Defs;
}

bool hasIndependentUse(const MachineInstr &MI, Register Reg,
                       const MachineOperand &Def) {
  const unsigned DefIndex = Def.getOperandNo();
  for (unsigned I = 0, E = MI.getNumOperands(); I != E; ++I) {
    const MachineOperand &MO = MI.getOperand(I);
    if (!MO.isReg() || !MO.isUse() || MO.getReg() != Reg)
      continue;
    if (!isPermittedPreservingUse(MI, I, DefIndex))
      return true;
  }
  return false;
}

std::optional<RewriteCandidate> AIEEpilogueRegRewriter::collectCandidate(
    Register Reg, MachineBasicBlock &Epilogue, MachineRegisterInfo &MRI,
    const AIEBaseRegisterInfo &TRI, VirtRegMap &VRM, LiveIntervals &LIS) const {
  const TargetRegisterClass *RC = MRI.getRegClass(Reg);
  if (!TRI.isVecOrAccRegClass(*RC))
    return std::nullopt;

  if (!LIS.hasInterval(Reg) || !VRM.hasPhys(Reg))
    return std::nullopt;

  const SmallSet<int, 8> CoveringSubRegs = TRI.getCoveringSubRegs(*RC);
  if (CoveringSubRegs.empty())
    return std::nullopt;

  SmallSet<int, 8> DefinedSubRegs;
  SmallVector<MachineOperand *, 4> Defs;
  bool Collecting = false;

  for (MachineInstr &MI : Epilogue) {
    SmallVector<MachineOperand *, 2> PartialDefs = getPartialDefs(MI, Reg);
    if (!Collecting) {
      if (PartialDefs.size() != 1 || !PartialDefs.front()->isUndef())
        continue;
      Collecting = true;
    } else if (PartialDefs.empty()) {
      for (const MachineOperand &MO : MI.operands())
        if (MO.isReg() && MO.isUse() && MO.getReg() == Reg)
          return std::nullopt;
      continue;
    }

    if (PartialDefs.size() != 1)
      return std::nullopt;

    MachineOperand &Def = *PartialDefs.front();
    const int SubReg = Def.getSubReg();
    if (!CoveringSubRegs.count(SubReg) || DefinedSubRegs.count(SubReg))
      return std::nullopt;
    if (hasIndependentUse(MI, Reg, Def))
      return std::nullopt;

    DefinedSubRegs.insert(SubReg);
    Defs.push_back(&Def);
    if (DefinedSubRegs == CoveringSubRegs)
      return RewriteCandidate{Reg, static_cast<MCPhysReg>(VRM.getPhys(Reg)),
                              RC,  std::move(Defs),
                              &MI, &Epilogue};
  }

  return std::nullopt;
}

SmallVector<RewriteCandidate, 4> AIEEpilogueRegRewriter::collectCandidates(
    MachineFunction &MF, MachineRegisterInfo &MRI,
    const AIEBaseRegisterInfo &TRI, VirtRegMap &VRM, LiveIntervals &LIS,
    SlotIndexes &Indexes) const {
  SmallVector<RewriteCandidate, 4> Candidates;

  for (MachineBasicBlock *Loop : AIELoopUtils::getSingleBlockLoopMBBs(MF)) {
    MachineBasicBlock *Epilogue =
        AIELoopUtils::findPrologueEpilogue(*Loop).second;
    if (!Epilogue)
      continue;

    SmallSetVector<Register, 16> LoopRegs;
    DenseMap<Register, std::pair<bool, bool>> LoopDefUse;
    for (MachineInstr &MI : *Loop) {
      for (MachineOperand &MO : MI.operands()) {
        if (!MO.isReg() || !MO.getReg().isVirtual())
          continue;
        Register Reg = MO.getReg();
        LoopRegs.insert(Reg);
        auto &DefUse = LoopDefUse[Reg];
        DefUse.first |= MO.isDef();
        DefUse.second |= MO.isUse();
      }
    }

    for (Register Reg : LoopRegs) {
      const auto [HasDef, HasUse] = LoopDefUse.lookup(Reg);
      if (!HasDef || !HasUse)
        continue;
      if (auto Candidate = collectCandidate(Reg, *Epilogue, MRI, TRI, VRM, LIS))
        Candidates.push_back(std::move(*Candidate));
    }
  }

  llvm::stable_sort(Candidates, [&](const RewriteCandidate &Left,
                                    const RewriteCandidate &Right) {
    return Indexes.getInstructionIndex(*Left.Defs.front()->getParent()) <
           Indexes.getInstructionIndex(*Right.Defs.front()->getParent());
  });
  return Candidates;
}

MCPhysReg AIEEpilogueRegRewriter::findReplacementPhysReg(
    const RewriteCandidate &Candidate, const AIEBaseRegisterInfo &TRI,
    LiveIntervals &LIS, LiveRegMatrix &LRM, SlotIndexes &Indexes,
    BitVector &ReservedRegUnits) const {
  const SlotIndex Boundary = Indexes.getMBBEndIdx(Candidate.Epilogue);
  LiveInterval ProspectiveLI(Candidate.OldReg, 0.0F);
  SlotIndex FirstDef;

  for (MachineOperand *Def : Candidate.Defs) {
    const SlotIndex DefIndex = LIS.getInstructionIndex(*Def->getParent())
                                   .getRegSlot(Def->isEarlyClobber());
    if (!FirstDef.isValid() || DefIndex < FirstDef)
      FirstDef = DefIndex;

    LaneBitmask LaneMask = TRI.getSubRegIndexLaneMask(Def->getSubReg());
    LiveInterval::SubRange *SubRange =
        ProspectiveLI.createSubRange(LIS.getVNInfoAllocator(), LaneMask);
    VNInfo *Value = SubRange->getNextValue(DefIndex, LIS.getVNInfoAllocator());
    SubRange->addSegment(LiveRange::Segment(DefIndex, Boundary, Value));
  }
  VNInfo *MainValue =
      ProspectiveLI.getNextValue(FirstDef, LIS.getVNInfoAllocator());
  ProspectiveLI.addSegment(LiveRange::Segment(FirstDef, Boundary, MainValue));

  LRM.invalidateVirtRegs();
  MCPhysReg NewPhys = AIERegAllocationUtils::findFreeNonOverlappingPhysReg(
      ProspectiveLI, *Candidate.RC, Candidate.RC->getRegisters(),
      Candidate.OldPhys, ReservedRegUnits, TRI, LRM);
  return NewPhys;
}

void AIEEpilogueRegRewriter::commitRewrite(
    RewriteCandidate &Candidate, MCPhysReg NewPhys, MachineRegisterInfo &MRI,
    const AIEBaseInstrInfo &TII, VirtRegMap &VRM, LiveRegMatrix &LRM,
    LiveIntervals &LIS, LiveDebugVariables &DebugVars) const {
  Register NewReg = MRI.cloneVirtualRegister(Candidate.OldReg);
  VRM.grow();
  for (MachineOperand *Def : Candidate.Defs)
    Def->setReg(NewReg);

  MachineInstr *Copy =
      BuildMI(*Candidate.Epilogue, Candidate.Epilogue->getFirstTerminator(),
              Candidate.FinalDef->getDebugLoc(), TII.get(TargetOpcode::COPY),
              Candidate.OldReg)
          .addReg(NewReg)
          .getInstr();
  LIS.InsertMachineInstrInMaps(*Copy);

  SmallSet<Register, 8> RegistersToRepair;
  RegistersToRepair.insert(Candidate.OldReg);
  AIESuperRegUtils::repairLiveIntervals(RegistersToRepair, VRM, LRM, LIS);

  LiveInterval &NewLI = LIS.createAndComputeVirtRegInterval(NewReg);
  LIS.shrinkToUses(&NewLI);

#ifndef NDEBUG
  LRM.invalidateVirtRegs();
  assert(LRM.checkInterference(NewLI, NewPhys) == LiveRegMatrix::IK_Free &&
         "Prevalidated physical register became unavailable");
#endif

  VRM.setRequiredPhys(NewReg, NewPhys);
  LRM.assign(NewLI, NewPhys);
  SmallVector<Register, 2> SplitRegs{Candidate.OldReg, NewReg};
  DebugVars.splitRegister(Candidate.OldReg, SplitRegs, LIS);
}

bool AIEEpilogueRegRewriter::runOnMachineFunction(MachineFunction &MF) {
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const auto &TRI =
      *static_cast<const AIEBaseRegisterInfo *>(MRI.getTargetRegisterInfo());
  const auto &TII =
      *static_cast<const AIEBaseInstrInfo *>(MF.getSubtarget().getInstrInfo());
  VirtRegMap &VRM = getAnalysis<VirtRegMapWrapperLegacy>().getVRM();
  SlotIndexes &Indexes = getAnalysis<SlotIndexesWrapperPass>().getSI();
  LiveIntervals &LIS = getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  LiveRegMatrix &LRM = getAnalysis<LiveRegMatrixWrapperLegacy>().getLRM();
  LiveDebugVariables &DebugVars =
      getAnalysis<LiveDebugVariablesWrapperLegacy>().getLDV();

  SmallVector<RewriteCandidate, 4> Candidates =
      collectCandidates(MF, MRI, TRI, VRM, LIS, Indexes);
  if (Candidates.empty())
    return false;

  BitVector ReservedRegUnits(TRI.getNumRegUnits());
  DenseMap<MachineBasicBlock *, unsigned> SpentBudget;
  bool Changed = false;

  for (RewriteCandidate &Candidate : Candidates) {
    unsigned &Spent = SpentBudget[Candidate.Epilogue];
    if (Spent >= EpilogueCopyBudget)
      continue;

    MCPhysReg NewPhys = findReplacementPhysReg(Candidate, TRI, LIS, LRM,
                                               Indexes, ReservedRegUnits);
    if (!NewPhys)
      continue;

    const std::optional<unsigned> CopyCost =
        TII.getCopyCost(TRI, Candidate.OldPhys, NewPhys);
    if (!CopyCost || *CopyCost > EpilogueCopyBudget - Spent)
      continue;

    for (MCRegUnit Unit : TRI.regunits(NewPhys))
      ReservedRegUnits.set(Unit);
    Spent += *CopyCost;
    commitRewrite(Candidate, NewPhys, MRI, TII, VRM, LRM, LIS, DebugVars);
    Changed = true;
  }

  return Changed;
}

} // namespace

char AIEEpilogueRegRewriter::ID = 0;
char &llvm::AIEEpilogueRegRewriterID = AIEEpilogueRegRewriter::ID;

INITIALIZE_PASS_BEGIN(AIEEpilogueRegRewriter, DEBUG_TYPE,
                      "AIE epilogue register rewrite", false, false)
INITIALIZE_PASS_DEPENDENCY(VirtRegMapWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(SlotIndexesWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveRegMatrixWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveDebugVariablesWrapperLegacy)
INITIALIZE_PASS_END(AIEEpilogueRegRewriter, DEBUG_TYPE,
                    "AIE epilogue register rewrite", false, false)

FunctionPass *llvm::createAIEEpilogueRegRewriter() {
  return new AIEEpilogueRegRewriter();
}
