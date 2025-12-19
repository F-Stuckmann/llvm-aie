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
#include "AIESuperRegUtils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/CodeGen/Register.h"
#include "llvm/CodeGen/Spiller.h"
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "regalloc"

static cl::opt<bool>
    SpillFullRegs("aie-subregspill-legacy", cl::Hidden, cl::init(false),
                  cl::desc("SubReg Spilling in legacy mode, spill full "
                           "registers instead of partial ones."));

STATISTIC(NumSubRegSpills, "Number of subregister spills inserted");
STATISTIC(NumSubRegReloads, "Number of subregister reloads inserted");
STATISTIC(NumFoldedCopies, "Number of COPYs folded in spill/reload sequences");

AIESubRegSpiller::AIESubRegSpiller(const Spiller::RequiredAnalyses &Analyses,
                                   MachineFunction &MF, VirtRegMap &VRM,
                                   VirtRegAuxInfo &VRAI, LiveRegMatrix &LRM)
    : InlineSpiller(Analyses, MF, VRM, VRAI), LRM(LRM) {
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
  LLVM_DEBUG(dbgs() << "Before SpillAll\n"; LIS.dump(); VRM.dump());
  // Skip if this register was already spilled (has a stack slot assigned).
  // We check Edit->getReg() (not Original) because:
  // - The base collectRegsToSpill() only collects "snippet" siblings (simple
  //   copies with limited usage), not ALL siblings sharing the same Original.
  // - Non-snippet siblings will come through spill() separately when the
  //   allocator fails to find a physical register for them.
  // - Checking Original's stack slot would incorrectly skip these non-snippet
  //   siblings since the Original gets a marker slot after the first sibling
  //   is processed.
  if (VRM.getStackSlot(Edit->getReg()) != VirtRegMap::NO_STACK_SLOT) {
    LLVM_DEBUG(dbgs() << "[SubRegSpiller] Skipping already-spilled register "
                      << printReg(Edit->getReg()) << "\n");
    return;
  }

  SpillInfo SI = collectSpillInfo();

  SI.calcStack(MRI, TRI, VRM, LSS);

  // todo: FIXME: perform optimizations

  LLVM_DEBUG(dbgs() << "[SubRegSpiller] SpillInfo: "; SI.dump());

  SI.insertReloads(MRI, TII, TRI, VRM, LIS);
  SI.insertSpills(MRI, TII, TRI, VRM, LIS);

  // Merge the stack intervals using actual spill/reload positions.
  // This must happen AFTER insertSpills/insertReloads so that SpillSlotIndices
  // and ReloadSlotIndices are populated. Enables StackSlotColoring to coalesce
  // non-overlapping stack slots with precise liveness information.
  SI.mergeStackIntervals(LIS, LSS);

  // Fold COPY chains in spill/reload sequences to reduce register pressure.
  // This must run before updateLIS to avoid issues with deleted instructions.
  SI.foldSpillCopies(MRI, TII, TRI, LIS);

  // Update LIS for all newly created registers. This is deferred until after
  // all spills/reloads are inserted so intervals are computed correctly
  // (especially for tied operands where reload and spill share the same reg).
  AIESuperRegUtils::repairLiveIntervals(SI.getRegsForLISUpdate(), LIS, VRM,
                                        LRM);

  SpillInfos.push_back(SI);
  LLVM_DEBUG(
      dbgs() << "[SubRegSpiller] After insertReloads and insertSpills:\n";
      LIS.dump());

  // Update LiveIntervals for the original register and the edited register.
  SmallSet<Register, 16> EditRegs;
  EditRegs.insert(SI.getReg());
  EditRegs.insert(Edit->getReg());
  AIESuperRegUtils::repairLiveIntervals(EditRegs, LIS, VRM, LRM);

  collectDeadDefs();
  eliminateDeadDefs();

  // The VReg being spilled has not yet been allocated to a Physical Register.
  // Due to a lack of high level methods we cannot tell RegAlloc to put the
  // Original VReg back on the allocation queue.
  // Therefore, we delete the spilled virtual register and create new VRegs
  // for the shorted LiveIntervals between Spill/Reload and Def/Use of the
  // original register. MRI will take care of notifying RegAlloc to enque the
  // new VRegs.
  deleteSpilledVirtualRegs();
  LLVM_DEBUG(dbgs() << "[SubRegSpiller] After deleteSpilledVirtualRegs:\n";
             LIS.dump(); VRM.dump());
}

void AIESubRegSpiller::collectDeadDefs() {
  // Collect dead definitions from RegsToSpill.
  // A def is dead if its LiveInterval segment ends at the dead slot [R, D).
  // We manually handle this because computeDeadValues/addRegisterDead doesn't
  // find defs inside bundled instructions - it only searches bundle headers.
  for (Register Reg : RegsToSpill) {
    if (!LIS.hasInterval(Reg))
      continue;
    LiveInterval &LI = LIS.getInterval(Reg);
    for (VNInfo *VNI : LI.valnos) {
      if (VNI->isUnused() || VNI->isPHIDef())
        continue;
      LiveRange::iterator I = LI.FindSegmentContaining(VNI->def);
      if (I == LI.end() || I->end != VNI->def.getDeadSlot())
        continue;
      // This is a dead def - segment ends at dead slot
      MachineInstr *MI = LIS.getInstructionFromIndex(VNI->def);
      if (!MI)
        continue;
      // Use MIBundleOperands to mark dead defs inside bundles.
      // addRegisterDead() only searches the bundle header's operands.
      for (MIBundleOperands MO(*MI); MO.isValid(); ++MO) {
        if (MO->isReg() && MO->isDef() && MO->getReg() == Reg)
          MO->setIsDead();
      }
      if (MI->allDefsAreDead()) {
        LLVM_DEBUG(dbgs() << "[SubRegSpiller] Dead def: " << *MI);
        DeadDefs.push_back(MI);
      }
    }
  }

  LLVM_DEBUG({
    if (!DeadDefs.empty()) {
      dbgs() << "[SubRegSpiller] dead defs Found: " << DeadDefs.size() << "\n";
      for (const MachineInstr *MI : DeadDefs)
        dbgs() << "  " << *MI;
    }
  });
}

SpillInfo AIESubRegSpiller::collectSpillInfo() const {
  LLVM_DEBUG(dbgs() << "[SubRegSpiller] Collecting Spill info for "
                    << printReg(Edit->getReg())
                    << " Orig: " << printReg(VRM.getOriginal(Edit->getReg()))
                    << " # Regs " << RegsToSpill.size() << "\n");

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
  // FIXME: What happens if RegsToSpill does not contain all the Registers?
  // Track which subreg indices we've already created entries for
  SmallSet<unsigned, 8> SeenSubRegIndices;

  for (const Register Reg : RegsToSpill) {
    for (MachineInstr &MI : llvm::make_early_inc_range(MRI.reg_bundles(Reg))) {
      if (MI.isDebugValue()) {
        LLVM_DEBUG(dbgs() << "Skipping debug value: " << MI);
        continue;
      }
      if (SpillFullRegs) {
        SubRegSpillInfos.push_back({});
        return;
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
        SubRegSpillInfos.emplace_back(SubRegIdx);

        SeenSubRegIndices.insert(SubRegIdx);
      }
    }
  }
}

void SpillInfo::calcStack(MachineRegisterInfo &MRI,
                          const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                          LiveStacks &LSS) {

  for (auto &Info : SubRegSpillInfos) {

    auto ExistingStackSlot = VRM.getStackSlot(OrigReg);

    // Create new slot
    const TargetRegisterClass *RC = MRI.getRegClass(OrigReg);
    if (Info.SubRegIdx)
      RC = TRI.getSubRegisterClass(RC, Info.SubRegIdx);

    Info.StackSlot = ExistingStackSlot != VirtRegMap::NO_STACK_SLOT
                         ? ExistingStackSlot
                         : VRM.createSpillSlot(RC);

    // Create the stack interval for StackSlotColoring. The value number is
    // added later by mergeStackIntervals() only if there are segments to merge.
    Info.StackInt = &LSS.getOrCreateInterval(Info.StackSlot, RC);

    LLVM_DEBUG(dbgs() << "Creating spill slot for " << printReg(OrigReg)
                      << " subreg " << Info.SubRegIdx << " Spill Slot: ";
               MachineOperand::printStackObjectReference(dbgs(), Info.StackSlot,
                                                         /*IsFixed=*/false,
                                                         /*Name=*/"");
               dbgs() << "\n");
  }

  // Mark OrigReg as spilled by assigning it to a stack slot.
  // This allows early-exit on subsequent spill attempts for the same register.
  // FIXME: This is a hack to get the marker slot for the original register. The
  // original register may be spilled via individual Subregs and thus actually
  // map to multiple stack slots.
  if (!SubRegSpillInfos.empty() &&
      VRM.getStackSlot(OrigReg) == VirtRegMap::NO_STACK_SLOT) {
    const int MarkerSlot = SubRegSpillInfos[0].StackSlot;
    VRM.assignVirt2StackSlot(OrigReg, MarkerSlot);
    LLVM_DEBUG({
      dbgs() << "Assigned marker stack slot ";
      MachineOperand::printStackObjectReference(dbgs(), MarkerSlot,
                                                /*IsFixed=*/false, /*Name=*/"");
      dbgs() << " to OrigReg " << printReg(OrigReg) << "\n";
    });
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

    if (llvm::is_contained(SpillLocations, Entry) ||
        llvm::is_contained(ReloadLocations, Entry))
      // Tied VRegs are encountered multiple times, we only have to add them
      // once.
      continue;

    auto HasDeadDef = [](const auto &Ops) {
      return llvm::any_of(
          Ops, [](const std::pair<MachineInstr *, unsigned> &Op) {
            const MachineOperand &MO = Op.first->getOperand(Op.second);
            return MO.isDef() && MO.isDead();
          });
    };

    if (RegInfo.Writes && !HasDeadDef(Ops)) {
      SpillLocations.push_back(Entry);
    }

    if (RegInfo.Reads) {
      ReloadLocations.push_back(Entry);
    }
  }
}

void SpillInfo::insertSpill(MachineInstr *MI, const Register ToSpill,
                            bool IsKill, MachineRegisterInfo &MRI,
                            const TargetInstrInfo &TII,
                            const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                            LiveIntervals &LIS) {
  MachineBasicBlock &MBB = *MI->getParent();

  // Use getBundleEnd to safely get an iterator past the entire bundle.
  // If MI is inside a bundle, std::next(MI->getIterator()) might point to
  // another bundled instruction, which cannot be converted to a bundle
  // iterator.
  const MachineBasicBlock::instr_iterator BundleEndIt =
      getBundleEnd(MI->getIterator());
  const MachineBasicBlock::iterator SpillBefore(BundleEndIt);

  // MachineInstrSpan must be created with a bundle iterator, not a bundled MI.
  MachineInstrSpan MIS(SpillBefore, &MBB);

  // Track store instructions per SubRegSpillInfo for SlotIndex computation.
  // We collect them during the loop and compute SlotIndexes after
  // InsertMachineInstrRangeInMaps adds them to the SlotIndexes.
  SmallVector<std::pair<SubRegSpillInfo *, MachineInstr *>, 4>
      StoreInstructions;

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
    VRM.setIsSplitFromReg(NewVReg, OrigReg);
    assert(VRM.getOriginal(NewVReg) == OrigReg &&
           "Temp register should share Original with spilled register");
    addRegForLISUpdate(NewVReg);
    // Replace the original def operand with the new register
    SpillerHelper::rewriteOperands(VRIAndOps.Ops, NewVReg);
  }

  // Compute which lanes are alive at the def slot. We only want to spill
  // subregs that are actually defined at this instruction. SubRegSpillInfos
  // contains all subregs defined across ALL instructions, but at this specific
  // spill location only a subset may be defined.
  LaneBitmask LiveLanesAtDef = LaneBitmask::getNone();
  if (LIS.hasInterval(ToSpill)) {
    const LiveInterval &LI = LIS.getInterval(ToSpill);
    const SlotIndex DefIdx = LIS.getInstructionIndex(*MI).getRegSlot();
    if (LI.hasSubRanges()) {
      for (const LiveInterval::SubRange &SR : LI.subranges()) {
        if (SR.liveAt(DefIdx))
          LiveLanesAtDef |= SR.LaneMask;
      }
    } else if (LI.liveAt(DefIdx)) {
      LiveLanesAtDef = LaneBitmask::getAll();
    }
  }

  for (auto &Info : SubRegSpillInfos) {
    const bool IsSubReg = Info.SubRegIdx != 0;

    // Skip subregs that are not alive at this spill point. This can happen
    // when different instructions define different subsets of subregs.
    if (IsSubReg) {
      const LaneBitmask SubRegLM = TRI.getSubRegIndexLaneMask(Info.SubRegIdx);
      if ((LiveLanesAtDef & SubRegLM).none()) {
        LLVM_DEBUG(dbgs() << "Skipping spill for dead subreg index "
                          << Info.SubRegIdx << "\n");
        continue;
      }
    }

    int StackSlot = Info.StackSlot;

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
      VRM.setIsSplitFromReg(TempVReg, OrigReg);
      assert(VRM.getOriginal(TempVReg) == OrigReg &&
             "Temp register should share Original with spilled register");
      Info.SpillVRegs.push_back(TempVReg);
      addRegForLISUpdate(TempVReg);

      // Create COPY: TempVReg = COPY NewVReg:subreg
      auto CopyBuilder = BuildMI(MBB, SpillBefore, MI->getDebugLoc(),
                                 TII.get(TargetOpcode::COPY), TempVReg);
      CopyBuilder.addReg(NewVReg, getKillRegState(IsKill), Info.SubRegIdx);

      RegToStore = TempVReg;
      StoreIsKill = true;
      NumSubRegSpills++;
    } else {
      RegToStore = NewVReg;
      // Track NewVReg for stack interval merging
      Info.SpillVRegs.push_back(NewVReg);
      // todo: remove this codepath, use regular inlinespiller
      NumSpills++;
    }

    // In tied case without subreg, NewVReg was already assigned by reload.
    bool AlreadyAssigned = IsTiedCase && !IsSubReg;
    if (!AlreadyAssigned)
      VRM.assignVirt2StackSlot(RegToStore, StackSlot);

    // Store the register to the stack slot
    // Note: storeRegToStackSlot may insert additional COPYs for certain
    // register classes (e.g., spill_eS_to_eR needs a COPY to eR first)
    TII.storeRegToStackSlot(MBB, SpillBefore, RegToStore, StoreIsKill,
                            StackSlot, RC, &TRI, Register());

    // Track the store instruction for later SlotIndex computation.
    // The store instruction is the one just inserted before SpillBefore.
    StoreInstructions.push_back({&Info, &*std::prev(SpillBefore)});
  }

  // Track all newly inserted instructions AND SpillInstr for COPY folding
  // This includes COPYs created by storeRegToStackSlot internally.
  // Start from bundle start to include all bundled instructions.
  for (auto It = getBundleStart(MI->getIterator()); It != BundleEndIt; ++It) {
    ModifiedAndInsertedMIs.push_back(&*It);
  }
  // MIS.begin() points to the first newly inserted instruction (if any).
  for (auto It = MIS.begin(); It != SpillBefore; ++It)
    ModifiedAndInsertedMIs.push_back(&*It);

  // Register new instructions in LIS maps, but defer interval computation
  // until all spills/reloads are inserted (handled in spillAll).
  // MIS.begin() points to the first newly inserted instruction.
  LIS.InsertMachineInstrRangeInMaps(MIS.begin(), SpillBefore);

  // Now that instructions are in LIS maps, get their SlotIndexes.
  for (auto &[InfoPtr, StoreMI] : StoreInstructions) {
    SlotIndex StoreIdx = LIS.getInstructionIndex(*StoreMI).getRegSlot();
    InfoPtr->SpillSlotIndices.push_back(StoreIdx);
  }
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

  // Use getBundleStart to safely get an iterator to the bundle head.
  // If MI is inside a bundle, we must insert before the entire bundle.
  const MachineBasicBlock::iterator InsertBefore(
      getBundleStart(MI->getIterator()));
  MachineInstrSpan MIS(InsertBefore, &MBB);

  // Track load instructions per SubRegSpillInfo for SlotIndex computation.
  // We collect them during the loop and compute SlotIndexes after
  // InsertMachineInstrRangeInMaps adds them to the SlotIndexes.
  SmallVector<std::pair<SubRegSpillInfo *, MachineInstr *>, 4> LoadInstructions;

  // Create a new virtual register for the parent register
  const TargetRegisterClass *OrigRC = MRI.getRegClass(OrigReg);
  Register NewVReg = MRI.createVirtualRegister(OrigRC);
  VRM.setIsSplitFromReg(NewVReg, OrigReg);
  assert(VRM.getOriginal(NewVReg) == OrigReg &&
         "Temp register should share Original with spilled register");
  addRegForLISUpdate(NewVReg);

  // Compute which lanes are alive at the use slot. We only want to reload
  // subregs that are actually used at this instruction. SubRegSpillInfos
  // contains all subregs defined across ALL instructions, but at this specific
  // reload location only a subset may be needed.
  LaneBitmask LiveLanesAtUse = LaneBitmask::getNone();
  if (LIS.hasInterval(ToBeReplacedReg)) {
    const LiveInterval &LI = LIS.getInterval(ToBeReplacedReg);
    const SlotIndex UseIdx = LIS.getInstructionIndex(*MI).getRegSlot(true);
    if (LI.hasSubRanges()) {
      for (const LiveInterval::SubRange &SR : LI.subranges()) {
        if (SR.liveAt(UseIdx))
          LiveLanesAtUse |= SR.LaneMask;
      }
    } else if (LI.liveAt(UseIdx)) {
      LiveLanesAtUse = LaneBitmask::getAll();
    }
  }

  bool FirstSubReg = true;
  for (unsigned I = 0; I < SubRegSpillInfos.size(); I++) {
    auto &Info = SubRegSpillInfos[I];
    unsigned SubRegIdx = Info.SubRegIdx;
    int StackSlot = Info.StackSlot;

    const bool IsSubReg = Info.SubRegIdx != 0;

    // Skip subregs that are not alive at this reload point. This can happen
    // when different instructions define/use different subsets of subregs.
    if (IsSubReg) {
      const LaneBitmask SubRegLM = TRI.getSubRegIndexLaneMask(SubRegIdx);
      if ((LiveLanesAtUse & SubRegLM).none()) {
        LLVM_DEBUG(dbgs() << "Skipping reload for dead subreg index "
                          << SubRegIdx << "\n");
        continue;
      }
    }

    unsigned AdditionalFlag =
        IsSubReg && FirstSubReg ? getUndefRegState(true) : 0;
    // Set FirstSubReg to false for BOTH full reg reload and subreg reload.
    // This ensures subsequent subreg COPYs don't incorrectly use undef flag
    // after a full register has been loaded.
    FirstSubReg = false;

    // Determine register class for the subreg
    const TargetRegisterClass *RC =
        !SubRegIdx ? OrigRC : TRI.getSubRegisterClass(OrigRC, SubRegIdx);
    assert(!SubRegIdx || SubRegIdx && OrigRC != RC &&
                             "Subreg RC should be different from OrigRC");

    Register RegToLoad{0};
    if (IsSubReg) {
      // Create temp register and assign to stack slot
      RegToLoad = MRI.createVirtualRegister(RC);
      VRM.setIsSplitFromReg(RegToLoad, OrigReg);
      assert(VRM.getOriginal(RegToLoad) == OrigReg &&
             "Temp register should share Original with spilled register");
      addRegForLISUpdate(RegToLoad);
      NumSubRegReloads++;
    } else {
      // todo: remove this codepath, use regular inlinespiller
      RegToLoad = NewVReg;
      NumReloads++;
    }
    VRM.assignVirt2StackSlot(RegToLoad, StackSlot);

    // Load from stack slot
    // Note: loadRegFromStackSlot may insert additional COPYs for certain
    // register classes
    TII.loadRegFromStackSlot(MBB, InsertBefore, RegToLoad, StackSlot, RC, &TRI,
                             Register());

    // Track the load instruction for later SlotIndex computation.
    // The load instruction is the one just inserted before InsertBefore.
    LoadInstructions.push_back({&Info, &*std::prev(InsertBefore)});

    if (IsSubReg) {
      // Copy from the temporary to the parent register's subregister
      auto CopyMIBuilder =
          BuildMI(MBB, InsertBefore, MI->getDebugLoc(),
                  TII.get(TargetOpcode::COPY))
              .addReg(NewVReg, RegState::Define | AdditionalFlag, SubRegIdx)
              .addReg(RegToLoad, RegState::Kill);
      LLVM_DEBUG(dbgs() << "Inserted: " << *CopyMIBuilder.getInstr());
    }
  }
  // Replace Old Register with reloaded Copy Register (NewVReg)
  SpillerHelper::rewriteOperands(VRIAndOps.Ops, NewVReg);

  // Track all newly inserted instructions for COPY folding
  // This includes COPYs created by loadRegFromStackSlot internally
  for (auto It = MIS.begin(); It != InsertBefore.getInstrIterator(); ++It)
    ModifiedAndInsertedMIs.push_back(&*It);
  // Add all instructions in the bundle (InsertBefore is already bundle start)
  for (auto It = InsertBefore.getInstrIterator();
       It != getBundleEnd(MI->getIterator()); ++It)
    ModifiedAndInsertedMIs.push_back(&*It);

  // Record the rename so insertSpill can find it for tied operands
  Renames.recordRename(MI, ToBeReplacedReg, NewVReg);

  // Register new instructions in LIS maps, but defer interval computation
  // until all spills/reloads are inserted (handled in spillAll).
  LIS.InsertMachineInstrRangeInMaps(MIS.begin(),
                                    InsertBefore.getInstrIterator());

  // Now that instructions are in LIS maps, get their SlotIndexes.
  for (auto &[InfoPtr, LoadMI] : LoadInstructions) {
    SlotIndex LoadIdx = LIS.getInstructionIndex(*LoadMI).getRegSlot();
    InfoPtr->ReloadSlotIndices.push_back(LoadIdx);
  }
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
    LLVM_DEBUG(dbgs() << "After insertSpill:\n"; MI->getParent()->dump());
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
  dbgs() << ", StackSlot: ";
  MachineOperand::printStackObjectReference(dbgs(), StackSlot,
                                            /*IsFixed=*/false, /*Name=*/"");
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

void SpillInfo::mergeStackIntervals(LiveIntervals &LIS, LiveStacks &LSS) {
  LLVM_DEBUG(dbgs() << "[mergeStackIntervals] Processing "
                    << SubRegSpillInfos.size() << " SubRegSpillInfos\n");

  for (auto &Info : SubRegSpillInfos) {
    if (!Info.StackInt)
      continue;

    // Need at least one spill AND one reload to have a valid range.
    // If either is empty, the stack slot isn't actually used in a meaningful
    // way (e.g., dead spill or unreachable reload).
    if (Info.SpillSlotIndices.empty() || Info.ReloadSlotIndices.empty()) {
      LLVM_DEBUG(dbgs() << "  Skipping SubRegIdx " << Info.SubRegIdx
                        << ": spills=" << Info.SpillSlotIndices.size()
                        << ", reloads=" << Info.ReloadSlotIndices.size()
                        << "\n");
      continue;
    }

    // Stack slot is live from earliest access to latest access.
    // This covers spill-reload-spill patterns (e.g., in loops) where a spill
    // may occur after the last reload. Example:
    //   1. Spill at slot 1252 (first store)
    //   2. Reload at slot 2468 (load)
    //   3. Spill at slot 2632 (second store - AFTER the reload!)
    // Without including spill indices in End, the range would be [1252, 2468)
    // which misses the second spill at 2632, causing "stores to dead spill
    // slot" errors.
    // Use getDeadSlot() because live intervals are half-open [Start, End) and
    // we need the interval to INCLUDE the last access point.
    // FIXME: Can we use multiple intervals and not the worst case range?
    SlotIndex Start = *llvm::min_element(Info.SpillSlotIndices);
    const SlotIndex MaxReload = *llvm::max_element(Info.ReloadSlotIndices);
    const SlotIndex MaxSpill = *llvm::max_element(Info.SpillSlotIndices);
    const SlotIndex MaxAccess = std::max(MaxReload, MaxSpill);
    SlotIndex End = MaxAccess.getDeadSlot();

    // Ensure valid range (Start < End). If the last reload happens before
    // the first spill (unusual but possible with tied operands), skip.
    if (Start >= End)
      continue;

    // Check if this stack interval already has segments (reusing stack slot)
    VNInfo *VNI = nullptr;
    if (!Info.StackInt->empty()) {
      // Try to find an existing segment that overlaps or is adjacent to the
      // new range. If found, reuse its VNInfo to properly extend the interval.
      for (const LiveInterval::Segment &Seg : Info.StackInt->segments) {
        // Check if segments overlap or are adjacent (within one slot)
        // Segments are half-open [start, end), so adjacent means:
        // - New range starts before/at existing end, or
        // - New range ends after/at existing start
        if (Start <= Seg.end && End >= Seg.start) {
          VNI = Seg.valno;
          LLVM_DEBUG(dbgs() << "  Reusing VNInfo from existing segment ["
                            << Seg.start << ", " << Seg.end << ") for range ["
                            << Start << ", " << End << ")\n");
          break;
        }
      }

      // If no overlapping segment found, create a new VNInfo
      // addSegment will still merge if segments end up overlapping
      if (!VNI) {
        VNI =
            Info.StackInt->getNextValue(SlotIndex(), LSS.getVNInfoAllocator());
        LLVM_DEBUG(dbgs() << "  Creating new VNInfo for range [" << Start
                          << ", " << End << ")\n");
      }
    } else {
      // Empty interval - create the first VNInfo
      VNI = Info.StackInt->getNextValue(SlotIndex(), LSS.getVNInfoAllocator());
    }

    // Add the segment. addSegment will automatically merge with overlapping
    // segments, extending the interval as needed.
    Info.StackInt->addSegment(LiveInterval::Segment(Start, End, VNI));

    LLVM_DEBUG(dbgs() << "Stack int [" << Start << ", " << End
                      << "): " << *Info.StackInt << '\n');
  }
}

void SpillInfo::foldSpillCopies(MachineRegisterInfo &MRI,
                                const TargetInstrInfo &TII,
                                const TargetRegisterInfo &TRI,
                                LiveIntervals &LIS) {
  LLVM_DEBUG(dbgs() << "[SubRegSpiller] Folding COPYs in spill/reload "
                       "sequences (symmetric)\n");

  if (ModifiedAndInsertedMIs.empty())
    return;

  SmallPtrSet<MachineInstr *, 8> InstsToDelete;
  SmallVector<Register, 8> RegsToRemove;
  SmallVector<Register, 8> SrcRegsExtended;
  // Physical registers and their new use indices for live interval extension
  SmallVector<std::pair<MCPhysReg, SmallVector<SlotIndex, 4>>, 4>
      PhysRegExtensions;

  // Helper to check if Src can replace Dst in all uses
  auto CanReplaceInAllUses = [&](Register Dst, Register Src) -> bool {
    // Get the register class of Src
    const TargetRegisterClass *SrcRC = Src.isVirtual()
                                           ? MRI.getRegClass(Src)
                                           : TRI.getMinimalPhysRegClass(Src);

    for (MachineOperand &MO : MRI.use_operands(Dst)) {
      MachineInstr *UserMI = MO.getParent();
      unsigned OpIdx = UserMI->getOperandNo(&MO);

      // Get the required register class for this operand
      const TargetRegisterClass *ReqRC =
          UserMI->getRegClassConstraint(OpIdx, &TII, &TRI);

      if (ReqRC) {
        // Check compatibility
        if (Src.isVirtual()) {
          if (!ReqRC->hasSubClassEq(SrcRC))
            return false;
        } else {
          // Physical register - check if it's in the required class
          if (!ReqRC->contains(Src))
            return false;
        }
      }
    }
    return true;
  };

  // Iterate until no more folding can be done (handles COPY chains)
  bool Changed = true;
  while (Changed) {
    Changed = false;

    // Process all inserted instructions - symmetric handling
    for (MachineInstr *MI : ModifiedAndInsertedMIs) {
      // Skip if already marked for deletion
      if (InstsToDelete.contains(MI))
        continue;

      // Check if this is a COPY
      auto CopyInfo = TII.isCopyInstr(*MI);
      if (!CopyInfo)
        continue;

      Register Src = CopyInfo->Source->getReg();
      Register Dst = CopyInfo->Destination->getReg();

      // We can only fold if Dst is virtual (we'll replace its uses)
      if (!Dst.isVirtual())
        continue;

      // We can only fold if Dst has exactly one definition (the COPY itself).
      // If there are other defs (e.g., partial subreg defs), those would be
      // left referencing a register with no live interval after we delete
      // the COPY.
      if (!MRI.hasOneDef(Dst))
        continue;

      // Skip COPYs with subreg destination - these are partial definitions
      // that define only part of the destination register. Folding them would
      // incorrectly propagate the source register to uses that expect the full
      // destination register, potentially across blocks where the source isn't
      // defined.
      if (CopyInfo->Destination->getSubReg())
        continue;

      // Only fold if all uses of Dst are in the same basic block as the COPY.
      // Folding across blocks can break dominance relationships, especially
      // with loops where a use in the next iteration would reference a
      // register defined later in the current iteration.
      const MachineBasicBlock *CopyMBB = MI->getParent();
      bool AllUsesLocal = llvm::all_of(
          MRI.use_operands(Dst), [CopyMBB](const MachineOperand &MO) {
            return MO.getParent()->getParent() == CopyMBB;
          });
      if (!AllUsesLocal)
        continue;

      // For virtual Src registers, verify that Src's definition dominates all
      // uses of Dst. If Src is defined in the same block as the uses, the def
      // must come before all uses in program order. Use SlotIndexes to compare.
      if (Src.isVirtual()) {
        // Use getUniqueVRegDef to handle registers with multiple definitions
        // (e.g., loop-carried values). If there's no unique def, skip folding.
        MachineInstr *SrcDef = MRI.getUniqueVRegDef(Src);
        if (!SrcDef)
          continue;

        // Get slot index of Src's definition
        SlotIndex SrcDefIdx = LIS.getInstructionIndex(*SrcDef);

        // Check all uses of Dst - after replacement they become uses of Src
        bool SrcDominatesAllUses =
            llvm::all_of(MRI.use_operands(Dst), [&](const MachineOperand &MO) {
              SlotIndex UseIdx = LIS.getInstructionIndex(*MO.getParent());
              // Src's def must come before this use
              return SrcDefIdx < UseIdx;
            });

        if (!SrcDominatesAllUses)
          continue;
      }

      // Check if we can replace all uses of Dst with Src
      if (!CanReplaceInAllUses(Dst, Src))
        continue;

      LLVM_DEBUG(dbgs() << "  Folding COPY: " << *MI << "    Replacing "
                        << printReg(Dst, &TRI) << " with "
                        << printReg(Src, &TRI) << "\n");

      // Collect the instructions that will be modified (for physical reg LI
      // update). Must be done before the replacement loop modifies the uses.
      SmallVector<MachineInstr *, 4> ModifiedInsts;
      for (MachineOperand &MO : MRI.use_operands(Dst))
        ModifiedInsts.push_back(MO.getParent());

      // Replace all uses of Dst with Src
      for (MachineOperand &MO :
           llvm::make_early_inc_range(MRI.use_operands(Dst))) {
        MO.setReg(Src);
        // Preserve kill flag from the COPY source if this is the last use
        if (CopyInfo->Source->isKill() && MRI.use_empty(Src))
          MO.setIsKill(true);
      }

      InstsToDelete.insert(MI);
      RegsToRemove.push_back(Dst);

      // Track source register that was extended with new uses.
      // When folding `Dst = COPY Src` by replacing uses of Dst with Src:
      // - Dst becomes dead (no uses, def deleted) -> remove its LiveInterval
      // - Src gains NEW uses (inherited from Dst) -> must extend its
      // LiveInterval
      if (Src.isVirtual()) {
        SrcRegsExtended.push_back(Src);
      } else {
        // For physical registers, collect the new use indices for later
        // extension. Virtual registers are handled via repairLiveIntervals.
        SmallVector<SlotIndex, 4> NewUseIndices;
        for (MachineInstr *UserMI : ModifiedInsts) {
          if (UserMI == MI)
            continue; // Skip COPY itself (will be deleted)
          NewUseIndices.push_back(
              LIS.getInstructionIndex(*UserMI).getRegSlot());
        }
        PhysRegExtensions.emplace_back(Src.asMCReg(), std::move(NewUseIndices));
      }
      ++NumFoldedCopies;
      Changed = true;
    }
  }

  // Delete folded COPYs
  for (MachineInstr *MI : InstsToDelete) {
    LLVM_DEBUG(dbgs() << "  Deleting: " << *MI);
    LIS.RemoveMachineInstrFromMaps(*MI);
    MI->eraseFromParent();
  }

  // Extend physical register live intervals to cover the new uses
  for (auto &[PhysReg, NewUseIndices] : PhysRegExtensions) {
    for (MCRegUnit Unit : TRI.regunits(PhysReg)) {
      LiveRange &LR = LIS.getRegUnit(Unit);
      LIS.extendToIndices(LR, NewUseIndices);
    }
  }

  // Update LIS for removed virtual registers
  for (Register Reg : RegsToRemove) {
    if (Reg.isVirtual() && LIS.hasInterval(Reg))
      LIS.removeInterval(Reg);
    RegsForLISUpdate.erase(Reg);
  }

  // Add source registers that were extended to the list for LIS update
  for (Register Reg : SrcRegsExtended)
    addRegForLISUpdate(Reg);
}
