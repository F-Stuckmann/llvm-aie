//===- InlineSpiller.h - Inline Spiller -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2025 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
//
// The inline spiller modifies the machine function directly instead of
// inserting spills and restores in VirtRegMap.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_INLINESPILLER_H
#define LLVM_CODEGEN_INLINESPILLER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/Spiller.h"
#include <memory>

namespace llvm {

class LiveInterval;
class LiveIntervals;
class LiveStacks;
class MachineBasicBlock;
class MachineDominatorTree;
class MachineFunction;
class MachineInstr;
class MachineRegisterInfo;
class TargetInstrInfo;
class TargetRegisterInfo;
class VirtRegAuxInfo;
class VirtRegMap;
class VNInfo;
class MachineBlockFrequencyInfo;

// Forward declaration for implementation details
class InsertPointAnalysis;

/// HoistSpillHelper - Manages spill hoisting optimization.
///
/// This class is responsible for identifying and hoisting spills to less
/// frequently executed basic blocks. It analyzes spills with equal values
/// and attempts to merge them by hoisting to common dominator blocks,
/// reducing overall spill frequency based on block execution probabilities.
///
/// Key optimization strategies:
/// - Identifies mergeable spills (spills with equal values from same def)
/// - Uses dominator tree analysis to find valid hoisting locations
/// - Considers block frequencies to minimize dynamic spill cost
/// - Removes redundant spills in the same basic block
///
/// The hoisting algorithm:
/// 1. Collect spills with equal values into mergeable sets
/// 2. Remove redundant spills within same BB
/// 3. Walk dominator tree bottom-up to find hoisting opportunities
/// 4. Compare subtree spill cost vs hoisting to current block
/// 5. Hoist when beneficial based on block frequency analysis
class HoistSpillHelper : private LiveRangeEdit::Delegate {
  /// Reference to the machine function being processed.
  MachineFunction &MF;
  LiveIntervals &LIS;
  LiveStacks &LSS;
  MachineDominatorTree &MDT;
  VirtRegMap &VRM;
  MachineRegisterInfo &MRI;
  const TargetInstrInfo &TII;
  const TargetRegisterInfo &TRI;
  const MachineBlockFrequencyInfo &MBFI;

  /// Helper for finding safe insertion points in basic blocks.
  std::unique_ptr<InsertPointAnalysis> IPA;

  /// Map from StackSlot to the LiveInterval of the original register.
  /// Note the LiveInterval of the original register may have been deleted
  /// after it is spilled. We keep a copy here to track the range where
  /// spills can be moved.
  DenseMap<int, std::unique_ptr<LiveInterval>> StackSlotToOrigLI;

  /// Map from pair of (StackSlot and Original VNI) to a set of spills which
  /// have the same stackslot and have equal values defined by Original VNI.
  /// These spills are mergeable and are hoist candidates.
  using MergeableSpillsMap =
      MapVector<std::pair<int, VNInfo *>, SmallPtrSet<MachineInstr *, 16>>;
  MergeableSpillsMap MergeableSpills;

  /// This is the map from original register to a set containing all its
  /// siblings. To hoist a spill to another BB, we need to find out a live
  /// sibling there and use it as the source of the new spill.
  DenseMap<Register, SmallSetVector<Register, 16>> Virt2SiblingsMap;

  /// Check if \p BB is a valid candidate for hoisting a spill from \p OrigLI
  /// with value \p OrigVNI. If valid, \p LiveReg is set to the live sibling
  /// register at the insertion point.
  bool isSpillCandBB(LiveInterval &OrigLI, VNInfo &OrigVNI,
                     MachineBasicBlock &BB, Register &LiveReg);

  /// Remove redundant spills within the same basic block from \p Spills.
  /// Redundant spills are added to \p SpillsToRm, and \p SpillBBToSpill maps
  /// each BB to the spill to keep.
  void rmRedundantSpills(
      SmallPtrSet<MachineInstr *, 16> &Spills,
      SmallVectorImpl<MachineInstr *> &SpillsToRm,
      DenseMap<MachineDomTreeNode *, MachineInstr *> &SpillBBToSpill);

  /// Compute a top-down traversal order of the dominator tree for hoisting
  /// from \p Root, considering \p Spills. The visit order is stored in
  /// \p Orders, redundant spills in \p SpillsToRm, and mappings in
  /// \p SpillsToKeep and \p SpillBBToSpill.
  void getVisitOrders(
      MachineBasicBlock *Root, SmallPtrSet<MachineInstr *, 16> &Spills,
      SmallVectorImpl<MachineDomTreeNode *> &Orders,
      SmallVectorImpl<MachineInstr *> &SpillsToRm,
      DenseMap<MachineDomTreeNode *, unsigned> &SpillsToKeep,
      DenseMap<MachineDomTreeNode *, MachineInstr *> &SpillBBToSpill);

  /// Perform the actual spill hoisting optimization for \p OrigLI with value
  /// \p OrigVNI. Processes \p Spills and populates \p SpillsToRm with spills
  /// to remove and \p SpillsToIns with new hoisted spill locations.
  void runHoistSpills(LiveInterval &OrigLI, VNInfo &OrigVNI,
                      SmallPtrSet<MachineInstr *, 16> &Spills,
                      SmallVectorImpl<MachineInstr *> &SpillsToRm,
                      DenseMap<MachineBasicBlock *, unsigned> &SpillsToIns);

public:
  /// Initialize the hoisting helper with \p Analyses for machine function
  /// \p mf and virtual register map \p vrm.
  HoistSpillHelper(const Spiller::RequiredAnalyses &Analyses,
                   MachineFunction &mf, VirtRegMap &vrm);

  ~HoistSpillHelper();

  /// Add \p Spill instruction to the set of mergeable spills for
  /// \p StackSlot and \p Original register.
  void addToMergeableSpills(MachineInstr &Spill, int StackSlot,
                            unsigned Original);

  /// Remove spill instruction \p Spill for \p StackSlot from the set of
  /// mergeable spills. Returns true if the spill was found and removed.
  bool rmFromMergeableSpills(MachineInstr &Spill, int StackSlot);

  /// Main entry point for spill hoisting optimization.
  /// Processes all mergeable spills and hoists them to less frequently
  /// executed locations when beneficial.
  void hoistAllSpills();

  /// Callback invoked when virtual register \p Old is cloned to \p New during
  /// live range editing. Ensures the cloned register inherits the same physical
  /// register or stack slot assignment as the original.
  void LRE_DidCloneVirtReg(Register New, Register Old) override;
};

/// InlineSpiller - Main spiller implementation.
///
/// This class implements the inline spilling algorithm, which directly
/// modifies the machine function to insert spills and reloads. Unlike
/// other spilling approaches, it doesn't defer to VirtRegMap.
///
/// Key features:
/// - Rematerialization: Attempts to recompute values instead of spilling
/// - Snippet spilling: Identifies and spills small live ranges together
/// - Memory operand folding: Folds stack accesses into instructions
/// - Spill hoisting: Uses HoistSpillHelper to optimize spill placement
/// - Stack slot sharing: Shares slots between split siblings
///
/// The spilling process:
/// 1. collectRegsToSpill() - Identify main register and snippets to spill
/// 2. reMaterializeAll() - Attempt rematerialization to avoid spills
/// 3. spillAll() - Insert actual spills and reloads for remaining registers
/// 4. postOptimization() - Hoist spills to less frequent locations
class InlineSpiller : public Spiller {
protected:
  MachineFunction &MF;
  LiveIntervals &LIS;
  LiveStacks &LSS;
  VirtRegMap &VRM;
  MachineRegisterInfo &MRI;
  const TargetInstrInfo &TII;
  const TargetRegisterInfo &TRI;

  /// Current live range edit being processed.
  /// Set during spill() and used throughout the spilling process.
  LiveRangeEdit *Edit = nullptr;

  /// Live interval for the assigned stack slot.
  LiveInterval *StackInt = nullptr;

  /// Stack slot index assigned to the original register.
  /// Shared among all split siblings.
  int StackSlot;

  /// The original register being spilled (before any splitting).
  /// All sibling registers created by splitting share the same original.
  Register Original;

  /// All registers to spill to StackSlot, including the main register.
  /// This includes the primary register and any snippet registers identified
  /// during collectRegsToSpill().
  SmallVector<Register, 8> RegsToSpill;

  /// All registers that were replaced by the spiller through other methods,
  /// e.g., rematerialization. These registers do not require actual
  /// spill/reload instructions.
  SmallVector<Register, 8> RegsReplaced;

  /// All COPY instructions to/from snippets.
  /// They are ignored since both operands refer to the same stack slot.
  /// For bundled copies, this will only include the first header copy.
  SmallPtrSet<MachineInstr *, 8> SnippetCopies;

  /// Values that failed to rematerialize at some point.
  /// Tracks VNInfos whose defining instructions must be kept.
  SmallPtrSet<VNInfo *, 8> UsedValues;

  /// Dead defs generated during spilling.
  /// These will be eliminated at the end of spilling.
  SmallVector<MachineInstr *, 8> DeadDefs;

  /// Helper object for performing spill hoisting optimization.
  HoistSpillHelper HSpiller;

  /// Live range weight calculator.
  /// Used to recalculate register class and allocation hints after spilling.
  VirtRegAuxInfo &VRAI;

public:
  /// Initialize the inline spiller with \p Analyses for machine function \p MF,
  /// virtual register map \p VRM, and auxiliary info \p VRAI for weight
  /// calculation.
  InlineSpiller(const Spiller::RequiredAnalyses &Analyses, MachineFunction &MF,
                VirtRegMap &VRM, VirtRegAuxInfo &VRAI);

  /// Destructor.
  ~InlineSpiller() override = default;

  /// Spill the live range \p edit describing the register to spill and any
  /// new registers created during splitting.
  /// This is the main entry point for spilling a register. It collects all
  /// registers to spill (including snippets), attempts rematerialization,
  /// and inserts spill/reload instructions as needed.
  void spill(LiveRangeEdit &edit) override;

  /// \return Array of registers that were spilled to stack slots.
  ArrayRef<Register> getSpilledRegs() override { return RegsToSpill; }

  /// \return Array of registers that were replaced without spilling.
  /// These are registers handled through rematerialization or other
  /// optimization techniques that avoid actual memory traffic.
  ArrayRef<Register> getReplacedRegs() override { return RegsReplaced; }

  /// Perform post-spilling optimizations.
  /// Currently invokes spill hoisting to move spills to less frequently
  /// executed locations.
  void postOptimization() override;

protected:
  /// \return whether live interval \p SnipLI is a snippet that should be
  /// spilled. A snippet is a tiny live range with only a single instruction
  /// using it (besides copies to/from the main register or spills/fills).
  /// Spilling snippets enables memory operand folding and tightens live ranges.
  ///
  /// Criteria for snippets:
  /// - Must be in a single basic block
  /// - At most 2 value numbers (not counting statepoint defs)
  /// - Only one real use instruction (copies and stack ops don't count)
  bool isSnippet(const LiveInterval &SnipLI);

  /// Collect all registers that should be spilled together.
  /// Starting with the main register (Edit->getReg()), identifies all snippet
  /// registers that should be spilled alongside it. Snippets are small live
  /// ranges connected by COPY instructions.
  void collectRegsToSpill();

  /// Check if register \p Reg is scheduled to be spilled.
  bool isRegToSpill(Register Reg) { return is_contained(RegsToSpill, Reg); }

  /// Check if \p Reg is a sibling of the original register.
  /// Siblings are virtual registers created by splitting that share the same
  /// original register.
  bool isSibling(Register Reg);

  /// Attempt to hoist a \p SpillLI within the same basic block where
  /// \p CopyMI defines it and kills its source. \return whether the spill was
  /// successfully hoisted past the source definition.
  /// This can eliminate interference between the source and other values.
  bool hoistSpillInsideBB(LiveInterval &SpillLI, MachineInstr &CopyMI);

  /// Remove redundant spills of a value \p VNI in live interval \p LI, since
  /// these spills are already on the stack.
  /// When a value is known to be in a stack slot, eliminate redundant
  /// spills and propagate the information through sibling copies.
  void eliminateRedundantSpills(LiveInterval &LI, VNInfo *VNI);

  /// Mark value \p VNI in live interval \p LI as used (cannot be
  /// rematerialized). This function propagates through snippet copies and PHI
  /// definitions to mark all related values as used.
  void markValueUsed(LiveInterval *LI, VNInfo *VNI);

  /// \return whether we can guarantee register assignment after rematerializing
  /// \p VReg at instruction \p MI.
  /// Some pseudo-instructions (like STATEPOINT) may have more operands than
  /// available physical registers. This checks if rematerialization would
  /// create an unassignable situation.
  bool canGuaranteeAssignmentAfterRemat(Register VReg, MachineInstr &MI);

  /// Attempt to rematerialize \p VirtReg before instruction \p MI instead of
  /// reloading. Tries to recompute the value by re-executing the defining
  /// instruction rather than loading from a stack slot. Falls back to memory
  /// folding if the original instruction can fold as a load. \return true if
  /// rematerialization or folding succeeded.
  bool reMaterializeFor(LiveInterval &VirtReg, MachineInstr &MI);

  /// Attempt to rematerialize all uses of spilled registers.
  void reMaterializeAll();

  /// Check if stack access instruction \p MI for register \p Reg is redundant
  /// and can be removed. If MI is a load or store of StackSlot for Reg, and
  /// it's redundant (e.g., loading a value that's already in the register),
  /// remove it. \return true if the instruction was removed.
  bool coalesceStackAccess(MachineInstr *MI, Register Reg);

  /// Attempt to fold stack slot references into instruction operands specified
  /// by \p Ops. If \p LoadMI is provided, fold that load instead of the stack
  /// slot. \return true if folding succeeded.
  /// Tries to convert explicit loads/stores into memory operands of the
  /// using/defining instruction, reducing register pressure and code size.
  bool foldMemoryOperand(ArrayRef<std::pair<MachineInstr *, unsigned>> Ops,
                         MachineInstr *LoadMI = nullptr);

  /// Insert a reload instruction for \p VReg at slot index \p Idx before
  /// instruction \p MI.
  void insertReload(Register VReg, SlotIndex Idx,
                    MachineBasicBlock::iterator MI);

  /// Insert a spill instruction for \p VReg after instruction \p MI. If
  /// \p isKill is true, the register is killed by this spill.
  void insertSpill(Register VReg, bool isKill, MachineBasicBlock::iterator MI);

  /// Insert spills and reloads around all uses of register \p Reg.
  /// For each use of Reg, either folds the stack access into the instruction,
  /// rematerializes the value, or inserts explicit reload/spill instructions.
  void spillAroundUses(Register Reg);

  /// Spill all registers remaining after rematerialization.
  virtual void spillAll();
};

namespace SpillerHelper {
/// Check if \p MI is a COPY to or from \p Reg using \p TII. Returns the
/// other register in the copy, or an invalid register if not a copy.
Register isCopyOf(const MachineInstr &MI, Register Reg,
                  const TargetInstrInfo &TII);

/// Check for a copy bundle involving \p Reg starting at \p FirstMI using
/// \p TII.
/// Handles bundled COPY instructions as formed by SplitKit. All copies
/// in the bundle must involve the same pair of registers.
/// \return the other register in the copy bundle if \p Reg is in the copy
/// bundle, or an invalid register if not a copy bundle.
Register isCopyOfBundle(const MachineInstr &FirstMI, Register Reg,
                        const TargetInstrInfo &TII);

/// Get live intervals for all virtual registers defined by \p MI using
/// \p LIS.
/// Ensures that LiveIntervals has tracking information for all virtual
/// register definitions in the instruction.
void getVDefInterval(const MachineInstr &MI, LiveIntervals &LIS);

/// \return wether instruction \p Def defines a real (non-undef) value.
/// Returns false if Def is an IMPLICIT_DEF with a subregister, indicating
/// the value is actually undefined and doesn't need to be spilled.
bool isRealSpill(const MachineInstr &Def);

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
/// Dump instruction range [ \p B, \p E) with slot indexes from \p LIS,
/// prefixed by \p header. If \p VReg is specified, show early-clobber slots.
void dumpMachineInstrRangeWithSlotIndex(MachineBasicBlock::iterator B,
                                        MachineBasicBlock::iterator E,
                                        LiveIntervals const &LIS,
                                        const char *const header,
                                        Register VReg = Register());
#endif
} // end namespace SpillerHelper

// Shared statistics for spilling operations
extern llvm::Statistic NumSpilledRanges;
extern llvm::Statistic NumSnippets;
extern llvm::Statistic NumSpills;
extern llvm::Statistic NumSpillsRemoved;
extern llvm::Statistic NumReloads;
extern llvm::Statistic NumReloadsRemoved;
extern llvm::Statistic NumFolded;
extern llvm::Statistic NumFoldedLoads;
extern llvm::Statistic NumRemats;

} // end namespace llvm

#endif // LLVM_CODEGEN_INLINESPILLER_H
