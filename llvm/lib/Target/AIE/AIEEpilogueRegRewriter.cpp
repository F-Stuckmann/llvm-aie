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
#include "Utils/AIELoopOptionOverrides.h"
#include "Utils/AIERegAllocationUtils.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
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

// Per-block opt-in: only rename WAR definitions in a block that carries this
// hint (see AIE::LoopOptionOverrides).
static cl::opt<bool> EnableOLPWarRename(
    "aie-olp-war-rename", cl::Hidden, cl::init(false),
    cl::desc("Enable WAR register renaming on blocks carrying the hint"));

namespace {

struct RewriteCandidate {
  Register OldReg;
  // Sorted by def slot index, so Defs.front() is the first def in the block.
  SmallVector<MachineOperand *, 4> Defs;
  MachineBasicBlock *MBB;
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
  collectCandidate(Register Reg, MachineBasicBlock &MBB,
                   MachineRegisterInfo &MRI, const AIEBaseRegisterInfo &TRI,
                   VirtRegMap &VRM, LiveIntervals &LIS) const;

  SmallVector<RewriteCandidate, 4>
  collectCandidates(MachineFunction &MF, MachineRegisterInfo &MRI,
                    const AIEBaseRegisterInfo &TRI, VirtRegMap &VRM,
                    LiveIntervals &LIS) const;

  MCPhysReg findReplacementPhysReg(const RewriteCandidate &Candidate,
                                   MachineRegisterInfo &MRI,
                                   const AIEBaseRegisterInfo &TRI,
                                   VirtRegMap &VRM, LiveIntervals &LIS,
                                   LiveRegMatrix &LRM,
                                   BitVector &ReservedRegUnits) const;

  void commitRewrite(RewriteCandidate &Candidate, MCPhysReg NewPhys,
                     MachineRegisterInfo &MRI, const AIEBaseInstrInfo &TII,
                     VirtRegMap &VRM, LiveRegMatrix &LRM, LiveIntervals &LIS,
                     LiveDebugVariables &DebugVars) const;
};

std::optional<RewriteCandidate> AIEEpilogueRegRewriter::collectCandidate(
    Register Reg, MachineBasicBlock &MBB, MachineRegisterInfo &MRI,
    const AIEBaseRegisterInfo &TRI, VirtRegMap &VRM, LiveIntervals &LIS) const {
  SmallVector<MachineOperand *, 4> Defs;
  SmallVector<MachineOperand *, 4> Uses;
  for (MachineOperand &MO : MRI.reg_nodbg_operands(Reg)) {
    if (MO.getParent()->getParent() != &MBB)
      continue;
    if (MO.isDef())
      Defs.push_back(&MO);
    else if (MO.isUse())
      Uses.push_back(&MO);
  }
  // A WAR hazard only exists if the register is both read and written here.
  if (Defs.empty() || Uses.empty())
    return std::nullopt;

  llvm::sort(Defs,
             [&](const MachineOperand *Left, const MachineOperand *Right) {
               return LIS.getInstructionIndex(*Left->getParent()) <
                      LIS.getInstructionIndex(*Right->getParent());
             });

  // Only defs get renamed and the repair copy lands at the block end, so a use
  // at or after the first rewritten def would see a partially renamed reg.
  const SlotIndex FirstDefIndex =
      LIS.getInstructionIndex(*Defs.front()->getParent());
  for (MachineOperand *Use : Uses)
    if (LIS.getInstructionIndex(*Use->getParent()) >= FirstDefIndex)
      return std::nullopt;

  LLVM_DEBUG(dbgs() << "Epilogue register rewrite: target "
                    << printReg(Reg, &TRI, 0, &MRI) << " in "
                    << MBB.getFullName() << ", old physical register "
                    << printReg(VRM.getPhys(Reg), &TRI) << '\n');
  return RewriteCandidate{Reg, std::move(Defs), &MBB};
}

SmallVector<RewriteCandidate, 4> AIEEpilogueRegRewriter::collectCandidates(
    MachineFunction &MF, MachineRegisterInfo &MRI,
    const AIEBaseRegisterInfo &TRI, VirtRegMap &VRM, LiveIntervals &LIS) const {
  SmallVector<RewriteCandidate, 4> Candidates;

  for (MachineBasicBlock &MBB : MF) {
    AIE::LoopOptionOverrides Overrides(MBB);
    if (!Overrides.get(EnableOLPWarRename))
      continue;

    SmallSet<Register, 16> SeenRegs;
    for (MachineInstr &MI : MBB) {
      for (const MachineOperand &Def : MI.defs()) {
        if (!Def.getReg().isVirtual())
          continue;

        Register Reg = Def.getReg();
        if (!SeenRegs.insert(Reg).second)
          continue;
        if (auto Candidate = collectCandidate(Reg, MBB, MRI, TRI, VRM, LIS))
          Candidates.push_back(std::move(*Candidate));
      }
    }
  }

  return Candidates;
}

MCPhysReg AIEEpilogueRegRewriter::findReplacementPhysReg(
    const RewriteCandidate &Candidate, MachineRegisterInfo &MRI,
    const AIEBaseRegisterInfo &TRI, VirtRegMap &VRM, LiveIntervals &LIS,
    LiveRegMatrix &LRM, BitVector &ReservedRegUnits) const {
  // Repair copy slot isn't allocated yet; MBB end is a stable, conservative
  // superset of the true [FirstDef, copy] range the new reg will occupy.
  const SlotIndex Boundary = LIS.getMBBEndIdx(Candidate.MBB);
  const MachineOperand *FirstDefMO = Candidate.Defs.front();
  const SlotIndex FirstDef = LIS.getInstructionIndex(*FirstDefMO->getParent())
                                 .getRegSlot(FirstDefMO->isEarlyClobber());

  // Defs live as one range up to the repair copy, and interference only
  // consults the main range, so per-def subranges cannot change the result.
  LiveInterval ProspectiveLI(Candidate.OldReg, 0.0F);
  VNInfo *MainValue =
      ProspectiveLI.getNextValue(FirstDef, LIS.getVNInfoAllocator());
  ProspectiveLI.addSegment(LiveRange::Segment(FirstDef, Boundary, MainValue));

  LRM.invalidateVirtRegs();
  const TargetRegisterClass *RC = MRI.getRegClass(Candidate.OldReg);
  const MCPhysReg OldPhys = VRM.getPhys(Candidate.OldReg);
  MCPhysReg NewPhys = AIERegAllocationUtils::findFreeNonOverlappingPhysReg(
      ProspectiveLI, *RC, RC->getRegisters(), OldPhys, ReservedRegUnits, TRI,
      LRM);
  LLVM_DEBUG(dbgs() << "Epilogue register rewrite: replacement for "
                    << printReg(Candidate.OldReg, &TRI) << " ("
                    << printReg(OldPhys, &TRI) << ") is "
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
      BuildMI(*Candidate.MBB, Candidate.MBB->getFirstTerminator(), DebugLoc(),
              TII.get(TargetOpcode::COPY), Candidate.OldReg)
          .addReg(NewReg)
          .getInstr();
  LIS.InsertMachineInstrInMaps(*Copy);
  LLVM_DEBUG(
      dbgs() << "Epilogue register rewrite: rewrite "
             << printReg(Candidate.OldReg, MRI.getTargetRegisterInfo(), 0, &MRI)
             << " to " << printReg(NewReg, MRI.getTargetRegisterInfo(), 0, &MRI)
             << " in " << Candidate.MBB->getFullName() << '\n');

  SmallSet<Register, 8> RegistersToRepair;
  RegistersToRepair.insert(Candidate.OldReg);
  AIESuperRegUtils::repairLiveIntervals(RegistersToRepair, VRM, LRM, LIS);

  LiveInterval &NewLI = LIS.createAndComputeVirtRegInterval(NewReg);
  LIS.shrinkToUses(&NewLI);

  LRM.invalidateVirtRegs();
#ifndef NDEBUG
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
  LiveIntervals &LIS = getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  LiveRegMatrix &LRM = getAnalysis<LiveRegMatrixWrapperLegacy>().getLRM();
  LiveDebugVariables &DebugVars =
      getAnalysis<LiveDebugVariablesWrapperLegacy>().getLDV();

  SmallVector<RewriteCandidate, 4> Candidates =
      collectCandidates(MF, MRI, TRI, VRM, LIS);
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
    unsigned &Spent = SpentBudget[Candidate.MBB];
    if (Spent >= EpilogueCopyBudget) {
      LLVM_DEBUG(dbgs() << "  skip "
                        << printReg(Candidate.OldReg, &TRI, 0, &MRI)
                        << ": epilogue copy budget exhausted\n");
      continue;
    }

    MCPhysReg NewPhys = findReplacementPhysReg(Candidate, MRI, TRI, VRM, LIS,
                                               LRM, ReservedRegUnits);
    if (!NewPhys) {
      LLVM_DEBUG(dbgs() << "  skip "
                        << printReg(Candidate.OldReg, &TRI, 0, &MRI)
                        << ": no free non-overlapping physical register\n");
      continue;
    }

    const std::optional<unsigned> CopyCost =
        TII.getCopyCost(TRI, VRM.getPhys(Candidate.OldReg), NewPhys);
    if (!CopyCost || *CopyCost > EpilogueCopyBudget - Spent) {
      LLVM_DEBUG(dbgs() << "  skip "
                        << printReg(Candidate.OldReg, &TRI, 0, &MRI)
                        << ": copy cost does not fit the remaining budget\n");
      continue;
    }

    // Block replaced registers from being selected again.
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
