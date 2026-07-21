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

std::optional<RewriteCandidate> AIEEpilogueRegRewriter::collectCandidate(
    Register Reg, MachineBasicBlock &Epilogue, MachineRegisterInfo &MRI,
    const AIEBaseRegisterInfo &TRI, VirtRegMap &VRM, LiveIntervals &LIS) const {
  if (!Reg.isVirtual() || !LIS.hasInterval(Reg) || !VRM.hasPhys(Reg))
    return std::nullopt;

  const TargetRegisterClass *RC = MRI.getRegClass(Reg);
  if (!TRI.isVecOrAccRegClass(*RC))
    return std::nullopt;

  LLVM_DEBUG(dbgs() << "Epilogue register rewrite: inspect "
                    << printReg(Reg, &TRI, 0, &MRI) << " in "
                    << Epilogue.getFullName() << '\n');

  SmallVector<MachineOperand *, 4> Defs;
  SlotIndex FirstDefIndex;
  for (MachineOperand &Def : MRI.def_operands(Reg)) {
    if (Def.getParent()->getParent() != &Epilogue)
      continue;
    Defs.push_back(&Def);
    const SlotIndex DefIndex = LIS.getInstructionIndex(*Def.getParent());
    if (!FirstDefIndex.isValid() || DefIndex < FirstDefIndex)
      FirstDefIndex = DefIndex;
  }
  if (Defs.empty()) {
    LLVM_DEBUG(dbgs() << "  reject: no epilogue definitions\n");
    return std::nullopt;
  }

  // Only the def operands get renamed, and the repair copy is inserted at
  // the epilogue end, so an epilogue use observes the old register value as
  // long as it happens strictly before the first rewritten def: it is
  // reading the prior generation of Reg, which is untouched by the rewrite.
  // A use at or after the first def could instead observe a partially
  // rewritten (or fully renamed) register, so it must block the rewrite.
  // Uses outside the epilogue are unaffected by the repair copy and are
  // therefore allowed regardless of position.
  for (MachineOperand &Use : MRI.use_operands(Reg)) {
    MachineInstr *UseMI = Use.getParent();
    if (UseMI->isDebugInstr())
      continue;
    if (UseMI->getParent() != &Epilogue) {
      LLVM_DEBUG(dbgs() << "  allow: non-debug use outside the epilogue in "
                        << UseMI->getParent()->getFullName() << '\n');
      continue;
    }
    const SlotIndex UseIndex = LIS.getInstructionIndex(*UseMI);
    if (UseIndex >= FirstDefIndex) {
      LLVM_DEBUG({
        dbgs() << "  reject: non-debug epilogue use at or after the first "
                  "rewritten def in ";
        UseMI->print(dbgs());
      });
      return std::nullopt;
    }
    LLVM_DEBUG(dbgs() << "  allow: non-debug epilogue use before the first "
                         "rewritten def in "
                      << Epilogue.getFullName() << '\n');
  }

  LLVM_DEBUG(dbgs() << "  accept: " << Defs.size()
                    << " epilogue definition(s), old physical register "
                    << printReg(VRM.getPhys(Reg), &TRI) << '\n');
  return RewriteCandidate{Reg, static_cast<MCPhysReg>(VRM.getPhys(Reg)), RC,
                          std::move(Defs), &Epilogue};
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

    LLVM_DEBUG(dbgs() << "Epilogue register rewrite: loop "
                      << Loop->getFullName() << ", epilogue "
                      << Epilogue->getFullName() << '\n');

    SmallSet<MCPhysReg, 16> LoopUsedPhysRegs;
    for (MachineInstr &MI : *Loop) {
      for (const MachineOperand &MO : MI.operands()) {
        if (!MO.isReg() || !MO.isUse() || !MO.getReg())
          continue;

        Register Reg = MO.getReg();
        if (Reg.isPhysical())
          LoopUsedPhysRegs.insert(Reg.asMCReg());
        else if (VRM.hasPhys(Reg))
          LoopUsedPhysRegs.insert(VRM.getPhys(Reg));
      }
    }

    SmallSetVector<Register, 16> EpilogueDefRegs;
    for (MachineInstr &MI : *Epilogue) {
      for (const MachineOperand &MO : MI.operands()) {
        if (MO.isReg() && MO.isDef() && MO.getReg().isVirtual())
          EpilogueDefRegs.insert(MO.getReg());
      }
    }

    for (Register Reg : EpilogueDefRegs) {
      if (!VRM.hasPhys(Reg)) {
        LLVM_DEBUG(dbgs() << "  skip " << printReg(Reg, &TRI, 0, &MRI)
                          << ": no assigned physical register\n");
        continue;
      }

      const MCPhysReg CandidatePhys = VRM.getPhys(Reg);
      bool UsedInLoop = false;
      for (MCPhysReg LoopPhys : LoopUsedPhysRegs) {
        if (TRI.regsOverlap(CandidatePhys, LoopPhys)) {
          UsedInLoop = true;
          break;
        }
      }
      if (!UsedInLoop) {
        LLVM_DEBUG(dbgs() << "  skip " << printReg(Reg, &TRI, 0, &MRI) << " ("
                          << printReg(CandidatePhys, &TRI)
                          << "): physical register is not used in the loop\n");
        continue;
      }

      if (auto Candidate = collectCandidate(Reg, *Epilogue, MRI, TRI, VRM, LIS))
        Candidates.push_back(std::move(*Candidate));
    }
  }

  llvm::stable_sort(Candidates, [&](const RewriteCandidate &Left,
                                    const RewriteCandidate &Right) {
    auto FirstDefIndex = [&](const RewriteCandidate &Candidate) {
      SlotIndex First =
          Indexes.getInstructionIndex(*Candidate.Defs.front()->getParent());
      for (MachineOperand *Def : Candidate.Defs) {
        const SlotIndex Index = Indexes.getInstructionIndex(*Def->getParent());
        if (Index < First)
          First = Index;
      }
      return First;
    };
    return FirstDefIndex(Left) < FirstDefIndex(Right);
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
  }
  VNInfo *MainValue =
      ProspectiveLI.getNextValue(FirstDef, LIS.getVNInfoAllocator());
  ProspectiveLI.addSegment(LiveRange::Segment(FirstDef, Boundary, MainValue));

  LRM.invalidateVirtRegs();
  MCPhysReg NewPhys = AIERegAllocationUtils::findFreeNonOverlappingPhysReg(
      ProspectiveLI, *Candidate.RC, Candidate.RC->getRegisters(),
      Candidate.OldPhys, ReservedRegUnits, TRI, LRM);
  LLVM_DEBUG(dbgs() << "Epilogue register rewrite: replacement for "
                    << printReg(Candidate.OldReg, &TRI) << " ("
                    << printReg(Candidate.OldPhys, &TRI) << ") is "
                    << printReg(NewPhys, &TRI) << '\n');
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
              DebugLoc(), TII.get(TargetOpcode::COPY), Candidate.OldReg)
          .addReg(NewReg)
          .getInstr();
  LIS.InsertMachineInstrInMaps(*Copy);
  LLVM_DEBUG(
      dbgs() << "Epilogue register rewrite: rewrite "
             << printReg(Candidate.OldReg, MRI.getTargetRegisterInfo(), 0, &MRI)
             << " to " << printReg(NewReg, MRI.getTargetRegisterInfo(), 0, &MRI)
             << " in " << Candidate.Epilogue->getFullName() << '\n');

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
  if (Candidates.empty()) {
    LLVM_DEBUG(dbgs() << "Epilogue register rewrite: no candidates in "
                      << MF.getName() << '\n');
    return false;
  }

  LLVM_DEBUG(dbgs() << "Epilogue register rewrite: " << Candidates.size()
                    << " candidate(s) in " << MF.getName() << '\n');
  BitVector ReservedRegUnits(TRI.getNumRegUnits());
  DenseMap<MachineBasicBlock *, unsigned> SpentBudget;
  bool Changed = false;

  for (RewriteCandidate &Candidate : Candidates) {
    unsigned &Spent = SpentBudget[Candidate.Epilogue];
    if (Spent >= EpilogueCopyBudget) {
      LLVM_DEBUG(dbgs() << "  skip "
                        << printReg(Candidate.OldReg, &TRI, 0, &MRI)
                        << ": epilogue copy budget exhausted\n");
      continue;
    }

    MCPhysReg NewPhys = findReplacementPhysReg(Candidate, TRI, LIS, LRM,
                                               Indexes, ReservedRegUnits);
    if (!NewPhys) {
      LLVM_DEBUG(dbgs() << "  skip "
                        << printReg(Candidate.OldReg, &TRI, 0, &MRI)
                        << ": no free non-overlapping physical register\n");
      continue;
    }

    const std::optional<unsigned> CopyCost =
        TII.getCopyCost(TRI, Candidate.OldPhys, NewPhys);
    if (!CopyCost || *CopyCost > EpilogueCopyBudget - Spent) {
      LLVM_DEBUG(dbgs() << "  skip "
                        << printReg(Candidate.OldReg, &TRI, 0, &MRI)
                        << ": copy cost does not fit the remaining budget\n");
      continue;
    }

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
