//===- AIESubRegSpiller.cpp - Custom AIE SubReg Spiller -------------------===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2025 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
//
// This file implements the AIESubRegSpiller class, which provides AIE-specific
// spilling strategies by wrapping the standard InlineSpiller.
//
//===----------------------------------------------------------------------===//

#include "AIESubRegSpiller.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/Register.h"
#include "llvm/CodeGen/Spiller.h"
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "regalloc"

STATISTIC(NumSubRegSpills, "Number of subregister spills inserted");
STATISTIC(NumSubRegReloads, "Number of subregister reloads inserted");

AIESubRegSpiller::AIESubRegSpiller(const Spiller::RequiredAnalyses &Analyses,
                                   MachineFunction &MF, VirtRegMap &VRM,
                                   VirtRegAuxInfo &VRAI)
    : InlineSpiller(Analyses, MF, VRM, VRAI) {
  // All common members initialized by InlineSpiller base constructor
  // SpillInfos will be default-initialized (empty vector)
  // Add AIE-specific initialization here if needed
}

namespace {

SubregSpiller::VirtRegInfoAndOps getVirtRegInfoAndOps(MachineInstr &MI,
                                                      const Register Reg) {
  SmallVector<std::pair<MachineInstr *, unsigned>, 8> Ops;
  VirtRegInfo RI = AnalyzeVirtRegInBundle(MI, Reg, &Ops);
  return SubregSpiller::VirtRegInfoAndOps{RI, Ops};
}

} // namespace

void SubregSpiller::VirtRegInfoAndOps::dump(
    const MachineRegisterInfo *MRI, const TargetRegisterInfo *TRI) const {
  dbgs() << "VirtRegInfoAndOps:\n";
  dbgs() << "  Reads: " << (RI.Reads ? "true" : "false") << "\n";
  dbgs() << "  Writes: " << (RI.Writes ? "true" : "false") << "\n";
  dbgs() << "  Tied: " << (RI.Tied ? "true" : "false") << "\n";
  dbgs() << "  Ops (" << Ops.size() << "):\n";
  for (const auto &Op : Ops) {
    MachineOperand &MO = Op.first->getOperand(Op.second);
    dbgs() << "    OpIdx: " << Op.second;
    if (MO.isReg()) {
      Register Reg = MO.getReg();
      dbgs() << ", Reg: " << printReg(Reg, TRI);
      if (MRI && Reg.isVirtual()) {
        const TargetRegisterClass *RC = MRI->getRegClass(Reg);
        dbgs() << ", RC: " << TRI->getRegClassName(RC);
      }
      if (MO.getSubReg())
        dbgs() << ", SubReg: " << MO.getSubReg();
    }
    dbgs() << "\n";
    dbgs() << "      " << *Op.first;
  }
}

void AIESubRegSpiller::spillAll() {
  SpillInfo SI = collectSpillInfo();

  SI.calcStack(MRI, TRI, VRM, LSS);

  // todo: FIXME: perform optimizations

  LLVM_DEBUG(SI.dump()); // MRI will be auto-fetched from
                         // SpillLocations/ReloadLocations

  SI.insertReloads(MRI, TII, TRI, VRM, LIS);
  SI.insertSpills(MRI, TII, TRI, VRM, LIS);
  SpillInfos.push_back(SI);

  // Update LiveIntervals for the original register and the edited register.
  SmallVector<Register, 2> EditRegs = {SI.getReg()};
  if (Edit->getReg() != SI.getReg())
    EditRegs.push_back(Edit->getReg());
  SI.updateLIS(EditRegs, LIS, true);

  // todo: why not include Edit->getReg() in RegsToSpill?
  RegsToSpill.emplace_back(SI.getReg());

  deleteSpilledVirtualRegs();
  LLVM_DEBUG(dbgs() << "[SubRegSpiller] After deleteSpilledVirtualRegs:\n";
             LIS.dump());
}

SpillInfo AIESubRegSpiller::collectSpillInfo() const {
  LLVM_DEBUG(dbgs() << "Collecting Spill info for " << RegsToSpill.size()
                    << " regs\n");

  // todo: is Edit->getReg() the same as the first RegsToSpill?
  SpillInfo SI(Original);
  SI.updateDefSubRegs(RegsToSpill, MRI);

  for (Register Reg : RegsToSpill) {
    LLVM_DEBUG(dbgs() << "updating Reg " << printReg(Reg) << "\n");
    SI.update(Reg, MRI);
  }
  return SI;
}

void SpillInfo::updateDefSubRegs(ArrayRef<Register> RegsToSpill,
                                 const MachineRegisterInfo &MRI) {
  // Track which subreg indices we've already created entries for
  SmallSet<unsigned, 8> SeenSubRegIndices;

  for (const Register Reg : RegsToSpill) {
    for (MachineInstr &MI : llvm::make_early_inc_range(MRI.reg_bundles(Reg))) {
      if (MI.isDebugValue()) {
        LLVM_DEBUG(dbgs() << "Skipping debug value: " << MI);
        continue;
      }

      const auto [RegInfo, Ops] = getVirtRegInfoAndOps(MI, Reg);

      // Process each def operand to collect subreg indices
      for (const auto &Op : Ops) {
        MachineOperand &MO = Op.first->getOperand(Op.second);
        if (!MO.isDef())
          continue;

        unsigned SubRegIdx = MO.getSubReg();

        // Check if we've already created a SubRegSpillInfo for this subreg
        // index
        if (SeenSubRegIndices.count(SubRegIdx))
          continue;

        // Create a new SubRegSpillInfo for this unique subreg index
        LLVM_DEBUG(dbgs() << "Adding SubRegSpillInfo for subreg index: "
                          << SubRegIdx << '\n');
        SubRegSpillInfo Info{SubRegIdx, 0, nullptr, {}};
        SubRegSpillInfos.push_back(Info);
        SeenSubRegIndices.insert(SubRegIdx);
      }
    }
  }
}

void SpillInfo::calcStack(MachineRegisterInfo &MRI,
                          const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                          LiveStacks &LSS) {

  for (auto &Info : SubRegSpillInfos) {
    // Get register class from the original register being spilled
    const TargetRegisterClass *RC = MRI.getRegClass(OrigReg);
    if (Info.SubRegIdx) {
      RC = TRI.getSubRegisterClass(RC, Info.SubRegIdx);
    }

    Info.StackSlot = VRM.createSpillSlot(RC);
  }
}

void SpillInfo::update(const Register Reg, MachineRegisterInfo &MRI) {
  // Iterate over instructions using Reg.
  // Note: No changes are made to registers just yet
  for (MachineInstr &MI : MRI.reg_bundles(Reg)) {
    if (MI.isDebugValue()) {
      LLVM_DEBUG(dbgs() << "Skipping debug value: " << MI);
      continue;
    }

    // Analyze instruction.
    const auto [RegInfo, Ops] = getVirtRegInfoAndOps(MI, Reg);
    if (RegInfo.Writes) {
      LLVM_DEBUG(dbgs() << "Adding spill location: " << MI);
      SpillLocations.push_back({&MI, Reg});
    }

    if (RegInfo.Reads) {
      LLVM_DEBUG(dbgs() << "Adding reload location: " << MI);
      ReloadLocations.push_back({&MI, Reg});
    }
  }
}

void SpillInfo::updateLIS(ArrayRef<Register> Regs, LiveIntervals &LIS,
                          const bool SkipNoInterval) {
  for (auto Reg : Regs) {

    if (SkipNoInterval && !LIS.hasInterval(Reg))
      continue;

    if (LIS.hasInterval(Reg)) {
      LIS.removeInterval(Reg);
    }

    LIS.createAndComputeVirtRegInterval(Reg);
    LIS.shrinkToUses(&LIS.getInterval(Reg));
  }
}

void SpillInfo::updateLIS(MachineBasicBlock::iterator Begin,
                          MachineBasicBlock::iterator End, LiveIntervals &LIS) {
  LIS.InsertMachineInstrRangeInMaps(Begin, End);

  // Collect Defs
  SmallVector<Register, 8> Defs;
  for (const MachineInstr &MI : make_range(Begin, End)) {
    for (const MachineOperand &MO : MI.all_defs()) {
      const Register Reg = MO.getReg();
      Defs.push_back(Reg);
    }
  }
  updateLIS(Defs, LIS);
}

void SpillInfo::insertSpill(MachineInstr *MI, const Register ToSpill,
                            bool IsKill, MachineRegisterInfo &MRI,
                            const TargetInstrInfo &TII,
                            const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                            LiveIntervals &LIS) {
  MachineBasicBlock &MBB = *MI->getParent();
  MachineInstrSpan MIS(MI, &MBB);
  MachineBasicBlock::iterator SpillBefore = std::next(MI->getIterator());

  const TargetRegisterClass *OrigRC = MRI.getRegClass(ToSpill);

  for (auto &Info : SubRegSpillInfos) {
    int StackSlot = Info.StackSlot;
    const bool IsSubReg = Info.SubRegIdx != 0;

    // Find the operand that matches this SubRegIdx
    const TargetRegisterClass *RC =
        !Info.SubRegIdx ? OrigRC
                        : TRI.getSubRegisterClass(OrigRC, Info.SubRegIdx);
    assert(!IsSubReg || IsSubReg && OrigRC != RC &&
                            "Subreg RC should be different from OrigRC");

    // Create a new virtual register
    Register NewVReg = MRI.createVirtualRegister(RC);
    Info.SpillVRegs.push_back(NewVReg);

    // Create COPY: NewVReg = COPY OrigReg:subreg
    auto CopyBuilder = BuildMI(MBB, SpillBefore, MI->getDebugLoc(),
                               TII.get(TargetOpcode::COPY), NewVReg);
    CopyBuilder.addReg(ToSpill, getKillRegState(IsKill), Info.SubRegIdx);

    // Assign the new virtual register to the stack slot
    VRM.assignVirt2StackSlot(NewVReg, StackSlot);

    // Store the new virtual register to the stack slot
    TII.storeRegToStackSlot(MBB, SpillBefore, NewVReg, true, StackSlot, RC,
                            &TRI, Register());

    if (IsSubReg)
      NumSubRegSpills++;
    else
      NumSpills++;
  }

  updateLIS(std::next(MI->getIterator()), MIS.end(), LIS);

  LLVM_DEBUG(MBB.dump());
}

void SpillInfo::insertReload(MachineInstr *MI, Register ToBeReplacedReg,
                             MachineRegisterInfo &MRI,
                             const TargetInstrInfo &TII,
                             const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                             LiveIntervals &LIS) {
  // Collect MOs of the original register.
  SubregSpiller::VirtRegInfoAndOps VRIAndOps =
      getVirtRegInfoAndOps(*MI, ToBeReplacedReg);

  MachineBasicBlock &MBB = *MI->getParent();
  MachineInstrSpan MIS(MI, &MBB);

  // Create a new virtual register for the parent register
  const TargetRegisterClass *OrigRC = MRI.getRegClass(OrigReg);
  Register NewVReg = MRI.createVirtualRegister(OrigRC);

  for (unsigned I = 0; I < SubRegSpillInfos.size(); I++) {
    auto &Info = SubRegSpillInfos[I];
    unsigned SubRegIdx = Info.SubRegIdx;
    int StackSlot = Info.StackSlot;

    const bool IsSubReg = Info.SubRegIdx != 0;
    unsigned AdditionalFlag = IsSubReg && I == 0 ? getUndefRegState(true) : 0;

    // Determine register class for the subreg
    const TargetRegisterClass *RC =
        !SubRegIdx ? OrigRC : TRI.getSubRegisterClass(OrigRC, SubRegIdx);
    assert(!SubRegIdx || SubRegIdx && OrigRC != RC &&
                             "Subreg RC should be different from OrigRC");

    // Create temp register and assign to stack slot
    Register TempReg = MRI.createVirtualRegister(RC);
    VRM.assignVirt2StackSlot(TempReg, StackSlot);

    // Load from stack slot
    TII.loadRegFromStackSlot(MBB, MI, TempReg, StackSlot, RC, &TRI, Register());

    // Copy from the temporary to the parent register's subregister
    auto CopyMIBuilder =
        BuildMI(MBB, MI, MI->getDebugLoc(), TII.get(TargetOpcode::COPY))
            .addReg(NewVReg, RegState::Define | AdditionalFlag, SubRegIdx)
            .addReg(TempReg, RegState::Kill);
    LLVM_DEBUG(dbgs() << "Inserted: " << *CopyMIBuilder.getInstr());

    if (IsSubReg)
      NumSubRegReloads++;
    else
      NumReloads++;
  }
  // Replace Old Register with reloaded Copy Register (NewVReg)
  SpillerHelper::rewriteOperands(VRIAndOps.Ops, NewVReg);

  // Update LIS, now that all newly inserted Copy Regs have been attached.
  updateLIS(MIS.begin(), MI, LIS);
}

void SpillInfo::insertSpills(MachineRegisterInfo &MRI,
                             const TargetInstrInfo &TII,
                             const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                             LiveIntervals &LIS) {
  const bool IsKill = true;
  for (auto [MI, Reg] : SpillLocations) {
    LLVM_DEBUG(dbgs() << "Inserting Spill for " << printReg(Reg) << " : " << *MI
                      << "\n";);
    insertSpill(MI, Reg, IsKill, MRI, TII, TRI, VRM, LIS);
  }
}

void SpillInfo::insertReloads(MachineRegisterInfo &MRI,
                              const TargetInstrInfo &TII,
                              const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                              LiveIntervals &LIS) {
  for (auto [MI, Reg] : ReloadLocations) {
    LLVM_DEBUG(dbgs() << "Inserting Reload for " << printReg(Reg) << " : "
                      << *MI << "\n");
    insertReload(MI, Reg, MRI, TII, TRI, VRM, LIS);
  }
}

void SubRegSpillInfo::dump(const MachineRegisterInfo *MRI,
                           const TargetRegisterInfo *TRI) const {
  dbgs() << "    SubRegIdx: " << SubRegIdx;
  dbgs() << ", StackSlot: " << StackSlot;
  dbgs() << ", StackInt: " << StackInt;
  dbgs() << ", SpillVRegs: [";
  for (unsigned I = 0; I < SpillVRegs.size(); I++) {
    if (I > 0)
      dbgs() << ", ";
    dbgs() << printReg(SpillVRegs[I]);
    if (MRI && SpillVRegs[I].isVirtual()) {
      const TargetRegisterClass *RC = MRI->getRegClass(SpillVRegs[I]);
      dbgs() << " (" << TRI->getRegClassName(RC) << ")";
    }
  }
  dbgs() << "]\n";
}

void SpillInfo::dump() const {
  // Lambda to get MRI from any available instruction pointer
  auto GetMRI = [this]() -> const MachineRegisterInfo * {
    const MachineInstr *MI = nullptr;
    if (!SpillLocations.empty())
      MI = SpillLocations[0].first;
    else if (!ReloadLocations.empty())
      MI = ReloadLocations[0].first;

    if (MI)
      if (const MachineFunction *MF = MI->getMF())
        return &MF->getRegInfo();
    return nullptr;
  };

  const MachineRegisterInfo *MRI = GetMRI();
  const TargetRegisterInfo *TRI = MRI ? MRI->getTargetRegisterInfo() : nullptr;

  dbgs() << "SpillInfo for register: " << printReg(OrigReg, TRI, 0, MRI);
  if (MRI && OrigReg.isVirtual()) {
    const TargetRegisterClass *RC = MRI->getRegClass(OrigReg);
    dbgs() << ", RC: " << TRI->getRegClassName(RC);
  }

  dbgs() << "  SubRegSpillInfos (" << SubRegSpillInfos.size() << "):\n";
  for (const auto &Info : SubRegSpillInfos) {
    Info.dump(MRI, TRI);
  }

  dbgs() << "  SpillLocations (" << SpillLocations.size() << "):\n";
  for (const auto &[MI, Reg] : SpillLocations) {
    dbgs() << "    " << *MI;
  }

  dbgs() << "  ReloadLocations (" << ReloadLocations.size() << "):\n";
  for (const auto &[MI, _] : ReloadLocations) {
    dbgs() << "    " << *MI;
  }

  // Dump the MachineBasicBlock of SpillLocations and ReloadLocations. Only
  // dump one if they are the same MBB.
  const MachineBasicBlock *SpillMBB = nullptr;
  const MachineBasicBlock *ReloadMBB = nullptr;
  if (!SpillLocations.empty())
    SpillMBB = SpillLocations.front().first->getParent();
  if (!ReloadLocations.empty())
    ReloadMBB = ReloadLocations.front().first->getParent();

  if (SpillMBB && SpillMBB == ReloadMBB) {
    dbgs() << "  MBB (Spill/Reload): " << *SpillMBB << "\n";
  } else {
    if (SpillMBB)
      dbgs() << "  Spill MBB: " << *SpillMBB << "\n";
    if (ReloadMBB)
      dbgs() << "  Reload MBB: " << *ReloadMBB << "\n";
  }
  dbgs() << "\n";
}
