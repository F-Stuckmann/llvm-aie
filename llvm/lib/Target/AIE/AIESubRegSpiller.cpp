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
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
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

void AIESubRegSpiller::spillAll() {
  SpillInfo SI = collectSpillInfo();

  SI.calcStack(MRI, TRI, VRM, LSS);

  // todo: FIXME: perform optimizations

  LLVM_DEBUG(SI.dump()); // MRI will be auto-fetched from
                         // SpillLocations/ReloadLocations

  SI.insertReloads(MRI, TII, TRI, VRM, LIS);
  SI.insertSpills(MRI, TII, TRI, VRM, LIS);
  SpillInfos.push_back(SI);
  RegsToSpill.emplace_back(SI.getReg());
  LLVM_DEBUG(MF.dump());
}

SpillInfo AIESubRegSpiller::collectSpillInfo() const {
  SpillInfo SI(Original);
  for (Register Reg : RegsToSpill) {
    SI.update(Reg, MRI);
  }
  return SI;
}

void SpillInfo::calcStack(MachineRegisterInfo &MRI,
                          const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                          LiveStacks &LSS) {

  for (auto &Info : SubRegSpillInfos) {
    // get Original register from the defining operand
    const TargetRegisterClass *RC = MRI.getRegClass(Info.DefOp.getReg());
    if (Info.DefOp.getSubReg()) {
      RC = TRI.getSubClassWithSubReg(RC, Info.DefOp.getSubReg());
    }

    Info.StackSlot = VRM.createSpillSlot(RC);
  }
}

void SpillInfo::updateVRegOps(
    ArrayRef<std::pair<MachineInstr *, unsigned>> Ops) {
  // Save Operands of Register in this->Ops
  this->Ops.append(Ops.begin(), Ops.end());

  // Update SubRegSpillInfos with write definitions from the given operands.
  for (const auto &Op : Ops) {
    MachineOperand &MO = Op.first->getOperand(Op.second);
    if (!MO.isDef())
      continue;

    const bool FoundDefMO =
        llvm::any_of(SubRegSpillInfos, [&](const auto &Info) {
          return Info.DefOp.isIdenticalTo(MO);
        });
    if (FoundDefMO) {
      LLVM_DEBUG(dbgs() << "Write def already exists: " << MO << '\n');
      continue;
    }

    LLVM_DEBUG(dbgs() << "Adding write def: " << MO << '\n');
    SubRegSpillInfo Info{MO, 0, nullptr, {}};
    SubRegSpillInfos.push_back(Info);
  }
}

void SpillInfo::update(Register Reg, MachineRegisterInfo &MRI) {
  // Iterate over instructions using Reg.
  for (MachineInstr &MI : llvm::make_early_inc_range(MRI.reg_bundles(Reg))) {
    if (MI.isDebugValue()) {
      LLVM_DEBUG(dbgs() << "Skipping debug value: " << MI);
      continue;
    }

    // Analyze instruction.
    SmallVector<std::pair<MachineInstr *, unsigned>, 8> Ops;
    VirtRegInfo RI = AnalyzeVirtRegInBundle(MI, Reg, &Ops);

    if (RI.Writes) {
      // Save the defining operands of the write.
      updateVRegOps(Ops);
      LLVM_DEBUG(dbgs() << "Adding spill location: " << MI);
      SpillLocations.push_back(&MI);
    }

    if (RI.Reads) {
      LLVM_DEBUG(dbgs() << "Adding reload location: " << MI);
      ReloadLocations.push_back(&MI);
    }
  }
}

void SpillInfo::insertSpill(MachineInstr *MI, bool IsKill,
                            MachineRegisterInfo &MRI,
                            const TargetInstrInfo &TII,
                            const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                            LiveIntervals &LIS) {
  MachineBasicBlock &MBB = *MI->getParent();
  MachineInstrSpan MIS(MI, &MBB);
  MachineBasicBlock::iterator SpillBefore = std::next(MI->getIterator());

  for (auto &Info : SubRegSpillInfos) {
    Register OrigReg = Info.DefOp.getReg();
    int StackSlot = Info.StackSlot;
    const TargetRegisterClass *RC = MRI.getRegClass(OrigReg);

    const bool IsSubReg = Info.DefOp.getSubReg() != 0;

    // Create a new virtual register
    Register NewVReg = MRI.createVirtualRegister(RC);
    Info.SpillVRegs.push_back(NewVReg);

    // Create COPY: NewVReg = COPY OrigReg
    BuildMI(MBB, SpillBefore, MI->getDebugLoc(), TII.get(TargetOpcode::COPY),
            NewVReg)
        .addReg(OrigReg, getKillRegState(IsKill));

    // Assign the new virtual register to the stack
    // slot
    VRM.assignVirt2StackSlot(NewVReg, StackSlot);

    // Store the new virtual register to the stack
    // slot
    TII.storeRegToStackSlot(MBB, SpillBefore, NewVReg, true, StackSlot, RC,
                            &TRI, Register());

    if (IsSubReg)
      NumSubRegSpills++;
    else
      NumSpills++;
  }

  MachineBasicBlock::iterator Spill = std::next(MI->getIterator());
  LIS.InsertMachineInstrRangeInMaps(Spill, MIS.end());

  LLVM_DEBUG(MBB.dump());
}

Register SpillInfo::insertReload(MachineInstr *MI, MachineRegisterInfo &MRI,
                                 const TargetInstrInfo &TII,
                                 const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                                 LiveIntervals &LIS) {
  MachineBasicBlock &MBB = *MI->getParent();
  MachineInstrSpan MIS(MI, &MBB);

  // Create a new virtual register
  const TargetRegisterClass *RC = MRI.getRegClass(OrigReg);
  Register NewVReg = MRI.createVirtualRegister(RC);

  for (unsigned I = 0; I < SubRegSpillInfos.size(); I++) {
    auto &Info = SubRegSpillInfos[I];
    Register OrigReg = Info.DefOp.getReg();
    unsigned SubRegIdx = Info.DefOp.getSubReg();
    int StackSlot = Info.StackSlot;

    const bool IsSubReg = Info.DefOp.getSubReg() != 0;
    unsigned AdditionalFlag = IsSubReg && I == 0 ? getUndefRegState(true) : 0;

    // Assign the new virtual register to the stack slot
    const TargetRegisterClass *SubRC = MRI.getRegClass(OrigReg);
    Register TempReg = MRI.createVirtualRegister(SubRC);
    VRM.assignVirt2StackSlot(TempReg, StackSlot);

    TII.loadRegFromStackSlot(MBB, MI, TempReg, StackSlot, SubRC, &TRI,
                             Register());

    // Copy from the temporary to the parent register's subregister
    auto CopyMIBuilder =
        BuildMI(MBB, MI, MI->getDebugLoc(), TII.get(TargetOpcode::COPY))
            .addReg(NewVReg, RegState::Define | AdditionalFlag, SubRegIdx)
            .addReg(TempReg, RegState::Kill);
    LLVM_DEBUG(dbgs() << "Inserted: " << *CopyMIBuilder.getInstr());

    // Ops.push_back(std::make_pair(CopyMIBuilder.getInstr(), 0));

    if (IsSubReg)
      NumSubRegReloads++;
    else
      NumReloads++;
  }

  LIS.InsertMachineInstrRangeInMaps(MIS.begin(), MI);

  LLVM_DEBUG(MBB.dump());
  return NewVReg;
}

void SpillInfo::replaceVReg(Register NewVReg) {
  // Todo: will i not replace Uses before spilling?
  for (const auto &OpPair : Ops) {
    MachineOperand &MO = OpPair.first->getOperand(OpPair.second);
    LLVM_DEBUG(dbgs() << "Replacing virtual register: " << printReg(MO.getReg())
                      << " in " << *OpPair.first << " with "
                      << printReg(NewVReg) << "\n";);
    MO.setReg(NewVReg);
    if (!MO.isUse())
      continue;

    if (!OpPair.first->isRegTiedToDefOperand(OpPair.second))
      MO.setIsKill();
  }
  return;
}

void SpillInfo::insertSpills(MachineRegisterInfo &MRI,
                             const TargetInstrInfo &TII,
                             const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                             LiveIntervals &LIS) {
  const bool IsKill = true;
  for (MachineInstr *MI : SpillLocations) {
    insertSpill(MI, IsKill, MRI, TII, TRI, VRM, LIS);
  }
}

void SpillInfo::insertReloads(MachineRegisterInfo &MRI,
                              const TargetInstrInfo &TII,
                              const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                              LiveIntervals &LIS) {
  for (MachineInstr *MI : ReloadLocations) {
    Register NewVReg = insertReload(MI, MRI, TII, TRI, VRM, LIS);
    replaceVReg(NewVReg);
  }
}

void SubRegSpillInfo::dump(const MachineRegisterInfo *MRI,
                           const TargetRegisterInfo *TRI) const {
  dbgs() << "    DefOp Reg: " << printReg(DefOp.getReg());
  if (MRI && DefOp.getReg().isVirtual()) {
    const TargetRegisterClass *RC = MRI->getRegClass(DefOp.getReg());
    dbgs() << ", RC: " << TRI->getRegClassName(RC);
  }
  dbgs() << ", SubReg: " << DefOp.getSubReg();
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
      MI = SpillLocations[0];
    else if (!ReloadLocations.empty())
      MI = ReloadLocations[0];

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

  dbgs() << "  Ops (" << Ops.size() << "):\n";
  for (const auto &Op : Ops) {
    dbgs() << "    " << printReg(Op.second) << " " << *Op.first;
  }

  dbgs() << "  SubRegSpillInfos (" << SubRegSpillInfos.size() << "):\n";
  for (const auto &Info : SubRegSpillInfos) {
    Info.dump(MRI, TRI);
  }

  dbgs() << "  SpillLocations (" << SpillLocations.size() << "):\n";
  for (const auto *MI : SpillLocations) {
    dbgs() << "    " << *MI;
  }

  dbgs() << "  ReloadLocations (" << ReloadLocations.size() << "):\n";
  for (const auto *MI : ReloadLocations) {
    dbgs() << "    " << *MI;
  }

  // Dump the MachineBasicBlock of SpillLocations and ReloadLocations. Only dump
  // one if they are the same MBB.
  const MachineBasicBlock *SpillMBB = nullptr;
  const MachineBasicBlock *ReloadMBB = nullptr;
  if (!SpillLocations.empty())
    SpillMBB = SpillLocations.front()->getParent();
  if (!ReloadLocations.empty())
    ReloadMBB = ReloadLocations.front()->getParent();

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
