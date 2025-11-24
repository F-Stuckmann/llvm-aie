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

  SI.insertSpills(MRI, TII, TRI, VRM, LIS);
  SI.insertReloads(MRI, TII, TRI, VRM, LIS);
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

  for (auto DefOp : DefOps) {
    // get Original register from the defining operand
    const TargetRegisterClass *RC = MRI.getRegClass(DefOp.getReg());
    if (DefOp.getSubReg()) {
      RC = TRI.getSubClassWithSubReg(RC, DefOp.getSubReg());
    }

    StackSlots.push_back(VRM.createSpillSlot(RC));
  }
}

void SpillInfo::updateVRegOps(
    ArrayRef<std::pair<MachineInstr *, unsigned>> Ops) {
  // Save Operands of Register in this->Ops
  this->Ops.clear();
  this->Ops.append(Ops.begin(), Ops.end());

  // Update DefOps with write definitions from the given operands.
  for (const auto &Op : Ops) {
    MachineOperand &MO = Op.first->getOperand(Op.second);
    if (MO.isDef()) {
      // Use isIdenticalTo for comparison instead of is_contained, since
      // MachineOperand does not have operator== defined for direct container
      // search.
      bool Found = false;
      for (const auto &DefOp : DefOps) {
        if (DefOp.isIdenticalTo(MO)) {
          Found = true;
          break;
        }
      }
      if (!Found) {
        LLVM_DEBUG(dbgs() << "Adding write def: " << MO << '\n');
        DefOps.push_back(MO);
      } else {
        LLVM_DEBUG(dbgs() << "Write def already exists: " << MO << '\n');
      }
    }
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

  for (unsigned I = 0; I < StackSlots.size(); I++) {
    Register OrigReg = DefOps[I].getReg();
    int StackSlot = StackSlots[I];
    const TargetRegisterClass *RC = MRI.getRegClass(OrigReg);

    const bool IsSubReg = DefOps[I].getSubReg() != 0;

    // Create a new virtual register
    Register NewVReg = MRI.createVirtualRegister(RC);
    SpillVRegs.push_back(NewVReg);

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
  const TargetRegisterClass *RC = MRI.getRegClass(Reg);
  Register NewVReg = MRI.createVirtualRegister(RC);

  for (unsigned I = 0; I < StackSlots.size(); I++) {
    Register OrigReg = DefOps[I].getReg();
    unsigned SubRegIdx = DefOps[I].getSubReg();
    int StackSlot = StackSlots[I];

    const bool IsSubReg = DefOps[I].getSubReg() != 0;
    unsigned AdditionalFlag = IsSubReg && I == 0 ? getUndefRegState(true) : 0;

    // Assign the new virtual register to the stack slot
    const TargetRegisterClass *SubRC = MRI.getRegClass(OrigReg);
    Register TempReg = MRI.createVirtualRegister(SubRC);
    VRM.assignVirt2StackSlot(TempReg, StackSlot);

    TII.loadRegFromStackSlot(MBB, MI, TempReg, StackSlot, SubRC, &TRI,
                             Register());

    // Copy from the temporary to the parent register's subregister
    BuildMI(MBB, MI, MI->getDebugLoc(), TII.get(TargetOpcode::COPY))
        .addReg(NewVReg, RegState::Define | AdditionalFlag, SubRegIdx)
        .addReg(TempReg, RegState::Kill);

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

  dbgs() << "SpillInfo for register: " << printReg(Reg);
  if (MRI && Reg.isVirtual()) {
    const TargetRegisterClass *RC = MRI->getRegClass(Reg);
    dbgs() << ", RC: " << TRI->getRegClassName(RC);
  }

  dbgs() << "  Ops (" << Ops.size() << "):\n";
  for (const auto &Op : Ops) {
    dbgs() << "    " << printReg(Op.second) << " " << *Op.first << "\n";
  }

  dbgs() << "  DefOps (" << DefOps.size() << "):\n";
  for (const auto &MO : DefOps) {
    dbgs() << "    Reg: " << printReg(MO.getReg());
    if (MRI && MO.getReg().isVirtual()) {
      const TargetRegisterClass *RC = MRI->getRegClass(MO.getReg());
      dbgs() << ", RC: " << TRI->getRegClassName(RC);
    }
    dbgs() << ", SubReg: " << MO.getSubReg() << "\n";
  }

  dbgs() << "  StackSlots (" << StackSlots.size() << "):\n";
  for (auto StackSlot : StackSlots) {
    dbgs() << "    StackSlot: " << StackSlot << "\n";
  }

  dbgs() << "  SpillVRegs (" << SpillVRegs.size() << "):\n";
  for (auto VReg : SpillVRegs) {
    dbgs() << "    " << printReg(VReg);
    if (MRI && VReg.isVirtual()) {
      const TargetRegisterClass *RC = MRI->getRegClass(VReg);
      dbgs() << ", RC: " << TRI->getRegClassName(RC);
    }
    dbgs() << "\n";
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
