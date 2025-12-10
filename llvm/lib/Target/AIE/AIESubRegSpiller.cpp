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

void RenameTracker::recordRename(MachineInstr *MI, Register OldReg,
                                 Register NewVReg) {
  Renames[{MI, OldReg}] = NewVReg;
}

Register RenameTracker::getRenamedReg(MachineInstr *MI, Register OldReg) const {
  auto It = Renames.find({MI, OldReg});
  if (It != Renames.end())
    return It->second;
  return Register();
}

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

  LLVM_DEBUG(dbgs() << "[SubRegSpiller] SpillInfo: "; SI.dump());

  SI.insertReloads(MRI, TII, TRI, VRM, LIS);
  SI.insertSpills(MRI, TII, TRI, VRM, LIS);

  // Update LIS for all newly created registers. This is deferred until after
  // all spills/reloads are inserted so intervals are computed correctly
  // (especially for tied operands where reload and spill share the same reg).
  SI.updateLIS(SI.getRegsForLISUpdate(), LIS);

  SpillInfos.push_back(SI);
  LLVM_DEBUG(
      dbgs() << "[SubRegSpiller] After insertReloads and insertSpills:\n";
      LIS.dump());

  // Update LiveIntervals for the original register and the edited register.
  SmallVector<Register, 2> EditRegs = {SI.getReg()};
  if (Edit->getReg() != SI.getReg())
    EditRegs.push_back(Edit->getReg());
  SI.updateLIS(EditRegs, LIS, true);

  // The VReg being spilled has not yet been allocated to a Physical Register.
  // Due to a lack of high level methods we cannot tell RegAlloc to put the
  // Original VReg back on the allocation queue.
  // Therefore, we delete the spilled virtual register and create new VRegs
  // for the shorted LiveIntervals between Spill/Reload and Def/Use of the
  // original register. MRI will take care of notifying RegAlloc to enque the
  // new VRegs.
  deleteSpilledVirtualRegs();
  LLVM_DEBUG(dbgs() << "[SubRegSpiller] After deleteSpilledVirtualRegs:\n";
             LIS.dump());
}

SpillInfo AIESubRegSpiller::collectSpillInfo() const {
  LLVM_DEBUG(dbgs() << "[SubRegSpiller] Collecting Spill info for "
                    << RegsToSpill.size() << " regs\n");

  // todo: is Edit->getReg() the same as the first RegsToSpill?
  SpillInfo SI(Original);
  SI.updateDefSubRegs(RegsToSpill, MRI);

  for (Register Reg : RegsToSpill) {
    LLVM_DEBUG(dbgs() << "  updating Reg " << printReg(Reg) << "\n");
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
    SpillMIAndReg Entry = {&MI, Reg};

    if (llvm::is_contained(SpillLocations, Entry))
      // Tied VRegs are encountered multiple times, we only have to add them
      // once.
      continue;

    if (RegInfo.Writes) {
      LLVM_DEBUG(dbgs() << "Adding spill location: " << MI);
      SpillLocations.push_back(Entry);
    }

    if (RegInfo.Reads) {
      LLVM_DEBUG(dbgs() << "Adding reload location: " << MI);
      ReloadLocations.push_back(Entry);
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
    LLVM_DEBUG(dbgs() << "Updated LIS for reg: " << printReg(Reg) << "\n";
               LIS.getInterval(Reg).dump());
  }
}

void SpillInfo::updateLIS(MachineBasicBlock::iterator Begin,
                          MachineBasicBlock::iterator End, LiveIntervals &LIS,
                          const bool ConsiderBeginInLISUpdate) {
  LLVM_DEBUG(dbgs() << "Updating LIS for range:\n";);
  LIS.InsertMachineInstrRangeInMaps(Begin, End);

  const MachineBasicBlock::iterator StartDefMI =
      ConsiderBeginInLISUpdate ? std::prev(Begin) : Begin;

  // Collect Defs
  SmallVector<Register, 8> Defs;
  for (const MachineInstr &MI : make_range(StartDefMI, End)) {
    LLVM_DEBUG(dbgs() << "    " << MI;);
    for (const MachineOperand &MO : MI.all_defs()) {
      LLVM_DEBUG(dbgs() << "        " << MO << "\n";);
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

  const TargetRegisterClass *OrigRC = MRI.getRegClass(OrigReg);
  Register NewVReg;
  bool IsTiedCase = false;

  // Check if this register was already renamed by insertReload (tied case)
  if (Register RenamedReg = Renames.getRenamedReg(MI, ToSpill);
      RenamedReg.isValid()) {
    LLVM_DEBUG(dbgs() << "Tied instruction case: using already renamed reg "
                      << printReg(RenamedReg) << "\n");
    NewVReg = RenamedReg;
    IsTiedCase = true;
    // Skip rewriteOperands - already done by insertReload
  } else {
    // Collect MOs of the original register
    SubregSpiller::VirtRegInfoAndOps VRIAndOps =
        getVirtRegInfoAndOps(*MI, ToSpill);
    // Create a new virtual register for the parent register
    NewVReg = MRI.createVirtualRegister(OrigRC);
    RegsForLISUpdate.push_back(NewVReg);
    // Replace the original def operand with the new register
    SpillerHelper::rewriteOperands(VRIAndOps.Ops, NewVReg);
  }

  for (auto &Info : SubRegSpillInfos) {
    int StackSlot = Info.StackSlot;
    const bool IsSubReg = Info.SubRegIdx != 0;

    // Find the operand that matches this SubRegIdx
    const TargetRegisterClass *RC =
        !Info.SubRegIdx ? OrigRC
                        : TRI.getSubRegisterClass(OrigRC, Info.SubRegIdx);
    assert(!IsSubReg || IsSubReg && OrigRC != RC &&
                            "Subreg RC should be different from OrigRC");

    Register RegToStore{0};
    bool StoreIsKill = IsKill;

    if (IsSubReg) {
      // Create a new virtual register
      Register TempVReg = MRI.createVirtualRegister(RC);
      Info.SpillVRegs.push_back(TempVReg);
      RegsForLISUpdate.push_back(TempVReg);

      // Create COPY: TempVReg = COPY NewVReg:subreg
      auto CopyBuilder = BuildMI(MBB, SpillBefore, MI->getDebugLoc(),
                                 TII.get(TargetOpcode::COPY), TempVReg);
      CopyBuilder.addReg(NewVReg, getKillRegState(IsKill), Info.SubRegIdx);

      RegToStore = TempVReg;
      StoreIsKill = true;
      NumSubRegSpills++;
    } else {
      RegToStore = NewVReg;
      // todo: remove this codepath, use regular inlinespiller
      NumSpills++;
    }

    // In tied case without subreg, NewVReg was already assigned by reload.
    bool AlreadyAssigned = IsTiedCase && !IsSubReg;
    if (!AlreadyAssigned)
      VRM.assignVirt2StackSlot(RegToStore, StackSlot);

    // Store the register to the stack slot
    TII.storeRegToStackSlot(MBB, SpillBefore, RegToStore, StoreIsKill,
                            StackSlot, RC, &TRI, Register());
  }

  // Register new instructions in LIS maps, but defer interval computation
  // until all spills/reloads are inserted (handled in spillAll).
  LIS.InsertMachineInstrRangeInMaps(std::next(MI->getIterator()), MIS.end());
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
  RegsForLISUpdate.push_back(NewVReg);

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

    Register RegToLoad{0};
    if (IsSubReg) {
      // Create temp register and assign to stack slot
      RegToLoad = MRI.createVirtualRegister(RC);
      RegsForLISUpdate.push_back(RegToLoad);
      NumSubRegReloads++;
    } else {
      // todo: remove this codepath, use regular inlinespiller
      RegToLoad = NewVReg;
      NumReloads++;
    }
    VRM.assignVirt2StackSlot(RegToLoad, StackSlot);

    // Load from stack slot
    TII.loadRegFromStackSlot(MBB, MI, RegToLoad, StackSlot, RC, &TRI,
                             Register());

    if (IsSubReg) {
      // Copy from the temporary to the parent register's subregister
      auto CopyMIBuilder =
          BuildMI(MBB, MI, MI->getDebugLoc(), TII.get(TargetOpcode::COPY))
              .addReg(NewVReg, RegState::Define | AdditionalFlag, SubRegIdx)
              .addReg(RegToLoad, RegState::Kill);
      LLVM_DEBUG(dbgs() << "Inserted: " << *CopyMIBuilder.getInstr());
    }
  }
  // Replace Old Register with reloaded Copy Register (NewVReg)
  SpillerHelper::rewriteOperands(VRIAndOps.Ops, NewVReg);

  // Record the rename so insertSpill can find it for tied operands
  Renames.recordRename(MI, ToBeReplacedReg, NewVReg);

  // Register new instructions in LIS maps, but defer interval computation
  // until all spills/reloads are inserted (handled in spillAll).
  LIS.InsertMachineInstrRangeInMaps(MIS.begin(), MI->getIterator());
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
}
