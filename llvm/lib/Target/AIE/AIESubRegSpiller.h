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

#include "llvm/CodeGen/InlineSpiller.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include <memory>

namespace llvm {

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
  Register Reg;

  SmallVector<std::pair<MachineInstr *, unsigned>, 8> Ops;

  /// Defining operands of the original register.
  /// Each defining operand may require a separate stack slot if it defines
  /// a subregister or has different register class requirements.
  SmallVector<MachineOperand, 8> DefOps;

  /// Stack slots used to spill the original register.
  /// One stack slot is allocated for each defining operand in DefOps.
  SmallVector<unsigned, 8> StackSlots;

  /// LiveInterval objects for the stack slots.
  SmallVector<LiveInterval *, 8> StackInts;

  /// New virtual registers created for spilling.
  /// These temporary registers are used to transfer values between the
  /// original register and the stack slots.
  SmallVector<Register, 8> SpillVRegs;

  /// Instructions after which to insert spill stores.
  /// Spill stores are inserted immediately after the instruction that
  /// defines the value being spilled.
  SmallVector<MachineInstr *, 8> SpillLocations;

  /// Instructions before which to insert reload loads.
  /// Reload loads are inserted immediately before the instruction that
  /// uses the spilled value.
  SmallVector<MachineInstr *, 8> ReloadLocations;

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
  void insertSpill(MachineInstr *MI, bool IsKill, MachineRegisterInfo &MRI,
                   const TargetInstrInfo &TII, const TargetRegisterInfo &TRI,
                   VirtRegMap &VRM, LiveIntervals &LIS);

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
  Register insertReload(MachineInstr *MI, MachineRegisterInfo &MRI,
                        const TargetInstrInfo &TII,
                        const TargetRegisterInfo &TRI, VirtRegMap &VRM,
                        LiveIntervals &LIS);

  void updateVRegOps(ArrayRef<std::pair<MachineInstr *, unsigned>> Ops);

  /// Replace the virtual register in the operands with the given new virtual
  /// register.
  ///
  /// \param NewVReg The new virtual register to replace the old one
  void replaceVReg(Register NewVReg);

public:
  /// Constructor - Initialize SpillInfo for the given register.
  ///
  /// \param Reg The original register to be spilled
  SpillInfo(Register Reg) : Reg(Reg) {}

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
  Register getReg() const { return Reg; }

  /// Dump the SpillInfo for debugging purposes.
  /// Prints the register, defining operands, stack slots, spill virtual
  /// registers, and spill/reload locations.
  void dump() const;
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

public:
  /// Constructor - Initialize the AIE subreg spiller.
  ///
  /// \param Analyses Required analyses including LiveIntervals and LiveStacks
  /// \param MF Machine function being processed
  /// \param VRM Virtual register map
  /// \param VRAI Virtual register auxiliary info for weight calculation
  AIESubRegSpiller(const Spiller::RequiredAnalyses &Analyses,
                   MachineFunction &MF, VirtRegMap &VRM, VirtRegAuxInfo &VRAI);

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
