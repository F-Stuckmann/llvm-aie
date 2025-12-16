//===- AIESubRegSpiller.h - Custom AIE SubReg Spiller -----------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2025 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
//
// Custom AIE subreg spiller.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AIE_AIESUBREGSPILLER_H
#define LLVM_LIB_TARGET_AIE_AIESUBREGSPILLER_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/InlineSpiller.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

namespace llvm {

class LiveRegMatrix;

namespace SubregSpiller {
struct VirtRegInfoAndOps {
  VirtRegInfo RI;
  SmallVector<std::pair<MachineInstr *, unsigned>, 8> Ops;

  void dump(const MachineRegisterInfo *MRI,
            const TargetRegisterInfo *TRI) const;
};
} // namespace SubregSpiller

/// This structure groups together all information needed to spill and reload
/// a single subregister definition. Multiple SubRegSpillInfo entries may exist
/// for the same virtual register if it has multiple distinct subregister
/// writes.
struct SubRegSpillInfo {
  /// Subregister index being spilled.
  /// This identifies which subreg (e.g., subreg0, subreg1) this entry handles
  /// across all registers in RegsToSpill.
  unsigned SubRegIdx;

  /// Stack slot allocated for this spill.
  /// Created by VirtRegMap::createSpillSlot() based on the register class
  /// of the defining operand.
  unsigned StackSlot;

  /// LiveInterval for the stack slot
  LiveInterval *StackInt = nullptr;

  /// Temporary virtual registers created during spilling.
  /// These registers are used to transfer values between the original
  /// register and the stack slot. They are assigned to StackSlot by
  /// VirtRegMap and will be eliminated by the register allocator.
  SmallVector<Register, 8> SpillVRegs;

  /// SlotIndices of store instructions that spill to this stack slot.
  /// Used to compute precise stack interval liveness.
  SmallVector<SlotIndex, 4> SpillSlotIndices;

  /// SlotIndices of load instructions that reload from this stack slot.
  /// Used to compute precise stack interval liveness.
  SmallVector<SlotIndex, 4> ReloadSlotIndices;

  /// Print debug information for this SubRegSpillInfo.
  void dump(const MachineRegisterInfo *MRI,
            const TargetRegisterInfo *TRI) const;
};

using SpillMIAndReg = std::pair<MachineInstr *, Register>;

/// RenameTracker - Tracks register renames during spill/reload processing.
///
/// This class tracks {MachineInstr*, OldReg} -> NewVReg mappings to handle
/// tied operands correctly. When a reload renames a register, the spill
/// processing can look up the mapping to avoid double-renaming.
class RenameTracker {
  DenseMap<std::pair<MachineInstr *, Register>, Register> Renames;

public:
  /// Record that OldReg was renamed to NewVReg in the given instruction.
  void recordRename(MachineInstr *MI, Register OldReg, Register NewVReg);

  /// Look up if OldReg was already renamed in the given instruction.
  /// Returns the new register if found, or an invalid register otherwise.
  Register getRenamedReg(MachineInstr *MI, Register OldReg) const;
};

/// SpillInfo - Encapsulates all information needed to spill a single register.
///
/// This class tracks the original register being spilled, its defining
/// operands, the stack slots allocated for spilling, and the locations where
/// spill stores and reload loads should be inserted. It provides methods to
/// calculate stack slot assignments and insert the actual spill/reload
/// instructions.
///
/// The typical usage pattern is:
/// 1. Create a SpillInfo for a register
/// 2. Call update() to collect defining operands and spill/reload locations
/// 3. Call calcStack() to allocate stack slots for each defining operand
/// 4. Call insertSpills() and insertReloads() to insert the actual instructions
class SpillInfo {
  /// Original register that is being spilled.
  Register OrigReg;

  SmallVector<SubRegSpillInfo, 8> SubRegSpillInfos;

  /// Instructions after which to insert spill stores.
  /// Spill stores are inserted immediately after the instruction that
  /// defines the value being spilled.
  SmallVector<std::pair<MachineInstr *, Register>, 8> SpillLocations;

  /// Instructions before which to insert reload loads.
  /// Reload loads are inserted immediately before the instruction that uses the
  /// spilled value.
  SmallVector<std::pair<MachineInstr *, Register>, 8> ReloadLocations;

  /// Tracks register renames to handle tied operands.
  /// When a reload renames a register, the spill can look up the new name
  /// to avoid double-renaming.
  RenameTracker Renames;

  ///  Registers that need LiveInterval updates after all
  ///  spills/reloads are
  /// inserted. We defer LIS computation until all instructions are in place
  /// to ensure intervals are computed correctly for tied operands. Reason: Once
  /// a Dead flag is added, it is not possible to remove it afterwards
  ///  with a LI update! Therefore, only update LIS once all regs have been
  ///  added.
  SmallVector<Register, 16> RegsForLISUpdate;

  /// All instructions inserted during spill/reload insertion.
  /// Includes COPYs and memory operations for use by foldSpillCopies().
  SmallVector<MachineInstr *, 16> ModifiedAndInsertedMIs;

  /// Insert a spill store instruction after the given instruction.
  /// Creates a COPY from the original register to a new virtual register,
  /// then stores that virtual register to the appropriate stack slot.
  ///
  /// \param MI Iterator pointing to the instruction after which to insert
  /// \param IsKill Whether the source register is killed by this spill
  /// \param MRI Machine register info
  /// \param TII Target instruction info
  /// \param TRI Target register info
  /// \param VRM Virtual register map
  /// \param LIS Live intervals
  void insertSpill(MachineInstr *MI, const Register ToSpill, bool IsKill,
                   MachineRegisterInfo &MRI, const TargetInstrInfo &TII,
                   const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                   LiveIntervals &LIS);

  /// Insert a reload load instruction before the given instruction.
  /// Loads from stack slots into temporary virtual registers, then copies
  /// those values to subregisters of a new parent register.
  ///
  /// \param MI Instruction before which to insert the reload
  /// \param MRI Machine register info
  /// \param TII Target instruction info
  /// \param TRI Target register info
  /// \param VRM Virtual register map
  /// \param LIS Live intervals
  void insertReload(MachineInstr *MI, Register ToBeReplacedReg,
                    MachineRegisterInfo &MRI, const TargetInstrInfo &TII,
                    const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                    LiveIntervals &LIS);

  void updateLIS(MachineBasicBlock::iterator Begin,
                 MachineBasicBlock::iterator End, LiveIntervals &LIS,
                 const bool ConsiderBeginInLISUpdate = false);

public:
  /// Constructor - Initialize SpillInfo for the given register.
  ///
  /// \param Reg The original register to be spilled
  SpillInfo(Register Reg) : OrigReg(Reg) {}

  void updateDefSubRegs(ArrayRef<Register> RegsToSpill,
                        const MachineRegisterInfo &MRI);

  /// Collect spill and reload information for the given register.
  /// Analyzes all instructions using the register to identify write definitions
  /// (spill locations) and read uses (reload locations).
  ///
  /// \param Reg The register to analyze
  /// \param MRI Machine register info
  void update(const Register Reg, MachineRegisterInfo &MRI);

  /// Calculate and allocate stack slots for spilling.
  /// Creates one stack slot for each defining operand, taking into account
  /// register classes and subregister indices.
  ///
  /// \param MRI Machine register info
  /// \param TRI Target register info
  /// \param VRM Virtual register map
  /// \param LSS Live stacks
  void calcStack(MachineRegisterInfo &MRI, const TargetRegisterInfo &TRI,
                 VirtRegMap &VRM, LiveStacks &LSS);

  /// Insert all spill store instructions at the collected spill locations.
  /// For each spill location, creates a COPY and store instruction to save
  /// the register value to the stack.
  ///
  /// \param MRI Machine register info
  /// \param TII Target instruction info
  /// \param TRI Target register info
  /// \param VRM Virtual register map
  /// \param LIS Live intervals
  void insertSpills(MachineRegisterInfo &MRI, const TargetInstrInfo &TII,
                    const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                    LiveIntervals &LIS);

  /// Insert all reload load instructions at the collected reload locations.
  /// For each reload location, creates load and COPY instructions to restore
  /// the register value from the stack.
  ///
  /// \param MRI Machine register info
  /// \param TII Target instruction info
  /// \param TRI Target register info
  /// \param VRM Virtual register map
  /// \param LIS Live intervals
  void insertReloads(MachineRegisterInfo &MRI, const TargetInstrInfo &TII,
                     const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                     LiveIntervals &LIS);

  /// Get the original register being spilled.
  ///
  /// \return The original register
  Register getReg() const { return OrigReg; }

  /// Get the registers that need LiveInterval updates.
  ///
  /// \return Array of registers needing LIS updates
  ArrayRef<Register> getRegsForLISUpdate() const { return RegsForLISUpdate; }

  /// Dump the SpillInfo for debugging purposes.
  /// Prints the register, defining operands, stack slots, spill virtual
  /// registers, and spill/reload locations.
  void dump() const;

  void updateLIS(ArrayRef<Register> Regs, LiveIntervals &LIS,
                 const bool SkipNoInterval = false);

  /// Fold COPYs in spill/reload sequences using register propagation.
  /// Iterates over InsertedMIs and for each COPY, replaces all uses of the
  /// destination with the source register (if register classes are compatible).
  ///
  /// This handles both spill and reload cases symmetrically:
  /// - COPY -> Store: store's use of Dst becomes Src
  /// - Load -> COPY: uses of Dst become Src (the loaded reg)
  ///
  /// \param MRI Machine register info
  /// \param TII Target instruction info
  /// \param TRI Target register info
  /// \param LIS Live intervals
  void foldSpillCopies(MachineRegisterInfo &MRI, const TargetInstrInfo &TII,
                       const TargetRegisterInfo &TRI, LiveIntervals &LIS);

  /// Merge the live ranges of spilled registers into their stack intervals.
  /// This enables StackSlotColoring to coalesce non-overlapping stack slots.
  ///
  /// For each SubRegSpillInfo, creates a stack interval segment from the
  /// earliest spill (store) to the latest reload (load) position. This
  /// provides precise liveness tracking based on actual instruction positions.
  /// Must be called AFTER insertSpills/insertReloads so that SpillSlotIndices
  /// and ReloadSlotIndices are populated.
  ///
  /// \param LIS Live intervals
  /// \param LSS Live stacks (for VNInfo allocation)
  void mergeStackIntervals(LiveIntervals &LIS, LiveStacks &LSS);
};

/// AIESubRegSpiller - AIE-specific register spiller.
///
/// This class extends the base Spiller functionality to provide AIE-specific
/// spilling strategies. It handles the spilling of virtual registers to stack
/// slots, including special handling for "snippets" - small live ranges that
/// are created by live range splitting and can be profitably spilled to
/// tighten register pressure.
///
/// Key features:
/// - Identifies and spills snippet registers alongside the main register
/// - Creates SpillInfo objects to track all spill/reload operations
/// - Integrates with LiveIntervals to maintain accurate liveness information
/// - Works with VirtRegMap to manage stack slot assignments
///
/// The spilling process:
/// 1. collectRegsToSpill() - Identify all registers (including snippets) to
/// spill
/// 2. collectSpillInfo() - Gather information about spill/reload locations
/// 3. spillAll() - Allocate stack slots and insert spill/reload instructions
class AIESubRegSpiller : public InlineSpiller {
  /// Collection of SpillInfo objects created during spilling.
  /// Each SpillInfo tracks the spill/reload operations for one register.
  SmallVector<SpillInfo, 8> SpillInfos;

  /// Live register matrix for tracking physical register interference.
  LiveRegMatrix &LRM;

public:
  /// Constructor - Initialize the AIE subreg spiller.
  ///
  /// \param Analyses Required analyses including LiveIntervals and LiveStacks
  /// \param MF Machine function being processed
  /// \param VRM Virtual register map
  /// \param VRAI Virtual register auxiliary info for weight calculation
  /// \param LRM Live register matrix
  AIESubRegSpiller(const Spiller::RequiredAnalyses &Analyses,
                   MachineFunction &MF, VirtRegMap &VRM, VirtRegAuxInfo &VRAI,
                   LiveRegMatrix &LRM);

protected:
  /// Perform the actual spilling of all collected registers.
  /// Creates a SpillInfo object, calculates stack slot assignments, and
  /// inserts all spill/reload instructions.
  void spillAll() override;

  /// Collect spill information for all registers in RegsToSpill.
  /// Creates a SpillInfo object and populates it with defining operands,
  /// spill locations, and reload locations for all registers being spilled.
  ///
  /// \return SpillInfo containing all collected information
  SpillInfo collectSpillInfo() const;

  /// Insert spill store instructions for the given SpillInfo.
  /// Wrapper method that delegates to SpillInfo::insertSpills().
  ///
  /// \param SI SpillInfo describing where to insert spills
  void insertSpills(SpillInfo &SI);

  /// Insert reload load instructions for the given SpillInfo.
  /// Wrapper method that delegates to SpillInfo::insertReloads().
  ///
  /// \param SI SpillInfo describing where to insert reloads
  void insertReloads(SpillInfo &SI);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_AIE_AIESUBREGSPILLER_H
