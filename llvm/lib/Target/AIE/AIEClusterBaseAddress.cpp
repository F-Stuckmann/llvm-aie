//===--- AIEClusterBaseAddress.cpp - Base Address Clustering --------------===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2023-2024 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
//
// AIE base address clustering to support post increment addressing.
//
// Cluster G_PTR_ADDs depending on the base address.
// Example:
//  Transform:
//    %1 = COPY $p1
//    %2 = G_CONSTANT i20 12
//    %3 = G_PTR_ADD %1, %2
//    G_LOAD %3
//    %5 = G_CONSTANT i20 16
//    %6 = G_PTR_ADD %1, %5
//    G_LOAD %6
//  Into:
//    %1 = COPY $p1
//    %2 = G_CONSTANT i20 12
//    %3 = G_PTR_ADD %1, %2
//    G_LOAD %3
//    %5 = G_CONSTANT i20 4
//    %6 = G_PTR_ADD %3, %5
//    G_LOAD %6
//
//  This will be later combined to
//    %1 = COPY $p1
//    %2 = G_CONSTANT i20 12
//    %3 = G_PTR_ADD %1, %2
//    %4 = G_CONSTANT i20 4
//    %_, %5 = G_AIE_POSTINC_LOAD %1, %4
//    G_LOAD %5
//
// TODO: As a preliminary implementation, we consider the ptr adds in only a
// single basic block. As such we try to avoid changing any ptr reg during
// clustering if we find that the base register of the ptr reg has uses later in
// the basic block. We need to implement a cross basic block approach where we
// are sure the clustering won't create any copies.
//===----------------------------------------------------------------------===//

#include "AIE.h"
#include "llvm/CodeGen/GlobalISel/CSEInfo.h"
#include "llvm/CodeGen/GlobalISel/CSEMIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/GenericMachineInstrs.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/InitializePasses.h"
#include <optional>
#include <set>

#define DEBUG_TYPE "aie-cluster-base-address"

using namespace llvm;

static const char AIE_CLUSTER_BASE_ADDRESS[] =
    "AIE Base Address Clustering Optimization";

static cl::opt<bool> EnableChainsForScalarLdSt(
    "aie-chain-addr-scl-ldst", cl::Hidden, cl::init(true),
    cl::desc("Enable ptradd chaining for scalar loads and stores."));

static cl::opt<bool> EnableChainsForVectorLdSt(
    "aie-chain-addr-vec-ldst", cl::Hidden, cl::init(true),
    cl::desc("Enable ptradd chaining for vector loads and stores."));

static cl::opt<bool> EnableChainsAcrossMultiBlocks(
    "aie-chain-addr-multi-block", cl::Hidden, cl::init(true),
    cl::desc("Enable ptradd chaining when Ptr is used across multiple MBBs."));

static cl::opt<bool> EnableInputPtrRestore(
    "aie-chain-addr-ptr-restore", cl::Hidden, cl::init(true),
    cl::desc(
        "Enable restoring pointer in case of usage across multiple MBBs."));

static cl::opt<bool>
    DetachCondLoadChain("aie-chain-addr-detach-cond-load-jump", cl::Hidden,
                        cl::init(true),
                        cl::desc("Disable ptradd chaining that feed "
                                 "loads that are used in conditional jumps."));

static cl::opt<bool>
    RestoreBrokenChains("aie-chain-addr-ptr-restore-broken-chain", cl::Hidden,
                        cl::init(true),
                        cl::desc("Restore Ptr if a Ptr Chain is broken."));

namespace {

LLT getLoadStoreType(const MachineInstr &MI, const MachineRegisterInfo &MRI) {
  const LLT SrcDestType{MRI.getType(MI.getOperand(0).getReg())};
  return SrcDestType;
}

/// Try and re-order PTR_ADD instructions to maximise the size of constant
/// PTR_ADD chains.
bool bundleConstIncrements(ArrayRef<MachineInstr *> PtrAdds,
                           const MachineRegisterInfo &MRI,
                           MachineIRBuilder &MIB,
                           GISelObserverWrapper &Observer) {
  bool Changed = false;

  // Look for the following sequence:
  // %0 = G_PTR_ADD %100, 64
  // %1 = G_PTR_ADD %100, %101
  // %2 = G_PTR_ADD %1, 32
  // And, if it is safe, swap the offsets to get:
  // %0 = G_PTR_ADD %100, 64
  // %1 = G_PTR_ADD %100, 32
  // %2 = G_PTR_ADD %1, %101
  for (MachineInstr *PtrAdd : PtrAdds) {
    assert(PtrAdd->getOpcode() == TargetOpcode::G_PTR_ADD);
    Register OutputPtr = PtrAdd->getOperand(0).getReg();
    Register OffsetReg = PtrAdd->getOperand(2).getReg();
    if (getIConstantVRegValWithLookThrough(OffsetReg, MRI) ||
        !MRI.hasOneNonDBGUser(OutputPtr))
      continue;

    // We found a non-constant PTRADD with a single user, now check if that
    // user is a constant PTRADD. If so, "swap" their offsets so that the
    // constant PTRADD appears first.
    MachineInstr &OutputUser = *MRI.use_instructions(OutputPtr).begin();
    if (OutputUser.getOpcode() != TargetOpcode::G_PTR_ADD)
      continue;
    Register SecondOffsetReg = OutputUser.getOperand(2).getReg();
    std::optional<ValueAndVReg> CstOffset =
        getIConstantVRegValWithLookThrough(SecondOffsetReg, MRI);
    if (!CstOffset)
      continue;

    // Everything fine, now swap the offsets
    LLVM_DEBUG(dbgs() << "Swapping offsets for:\n      " << *PtrAdd << "  and "
                      << OutputUser);
    Observer.changingInstr(*PtrAdd);
    MIB.setInstr(*PtrAdd);
    Register NewOffsetReg =
        MIB.buildConstant(LLT::scalar(20), CstOffset->Value.getSExtValue())
            .getReg(0);
    PtrAdd->getOperand(2).setReg(NewOffsetReg);
    Observer.changedInstr(*PtrAdd);
    Observer.changingInstr(OutputUser);
    OutputUser.getOperand(2).setReg(OffsetReg);
    OutputUser.clearFlag(MachineInstr::NoUWrap);
    Observer.changedInstr(OutputUser);
    Changed = true;
  }

  return Changed;
}

/// Helper class to keep track of the replacement Registers of OldReg for each
/// MachineBasicBlock. This map is recreated for each ptradd chain.
class MBBRegMap {
  // Base Register.
  Register OldReg;
  // For each MachineBasicBlock map the updated Register that exits the
  // MBB.
  std::unordered_map<MachineBasicBlock *, Register> MapBlockToOutgoingReg;

public:
  MBBRegMap() {}
  MBBRegMap(const Register OldReg) : OldReg(OldReg) {}

  Register getOldReg() const { return OldReg; }

  /// Update map between \p MBB and \p NewReg iff mapping does not already
  /// exist. \p ForceUpdate always updates the Register Map.
  bool updateMappedReg(MachineBasicBlock *MBB, const Register NewReg,
                       bool ForceUpdate = false) {
    if (MapBlockToOutgoingReg.count(MBB) && !ForceUpdate)
      return false;

    if (NewReg == OldReg)
      return false;

    LLVM_DEBUG(dbgs() << "    MBBRegMap: Updating bb." << MBB->getNumber()
                      << " with " << printReg(NewReg) << "\n");

    if (auto It = MapBlockToOutgoingReg.find(MBB);
        It != MapBlockToOutgoingReg.end() && It->second == NewReg)
      return false; // Nothing to update

    MapBlockToOutgoingReg.insert_or_assign(MBB, NewReg);
    return true;
  }

  /// \return the alias of OldReg flowing out of \p MBB . If there is no alias,
  /// return invalid register.
  Register getNewReg(MachineBasicBlock *MBB) const {
    auto It = MapBlockToOutgoingReg.find(MBB);
    if (It == MapBlockToOutgoingReg.end())
      return 0;

    return It->second;
  }

  /// \return the representative (or alias) of OldReg flowing out of \p MBB
  Register getReg(MachineBasicBlock *MBB) const {
    if (Register NewReg = getNewReg(MBB))
      return NewReg;
    return OldReg;
  }

  void dump() const {
    dbgs() << "RegMap for " << printReg(OldReg) << "\n";
    for (auto [MBBCount, OutgoingReg] : MapBlockToOutgoingReg) {
      dbgs() << "bb." << *MBBCount << " = " << printReg(OutgoingReg) << "\n";
    }
  }

  /// \return set of all alias registers in the MBBRegMap
  std::set<Register> getNewRegs() const {
    std::set<Register> NewRegs;
    for (auto It : MapBlockToOutgoingReg)
      NewRegs.emplace(It.second);

    return NewRegs;
  }
};

/// Helper class to keep track of original Registers (OldRegs) and the newly
/// inserted Registers that have the same value as the original Register.
/// There can be multiple OldRegs in this Map.
// Note: This lives for the whole MachineFunction.
class ReplacedRegMap {
  // Mapping between a newly inserted Register and the original Register.
  std::map<Register, Register> NewRegToOldReg;
  // Mapping between original Register and all its alias Registers.
  std::map<Register, std::set<Register>> OldRegAliasRegs;

public:
  /// Add \p RegMap to the already replaced Register mappings.
  void addMapping(MBBRegMap RegMap) {
    Register OrigReg = getOrigReg(RegMap.getOldReg());
    if (!OrigReg) {
      OrigReg = RegMap.getOldReg();
    }
    for (Register NewReg : RegMap.getNewRegs()) {
      if (!NewReg)
        continue;

      NewRegToOldReg.insert({NewReg, OrigReg});
      auto It = OldRegAliasRegs.find(OrigReg);
      if (It == OldRegAliasRegs.end()) {
        OldRegAliasRegs.insert({OrigReg, {NewReg}});
        continue;
      }
      It->second.emplace(NewReg);
    }
  }

  /// \return Original Register of \p NewReg .
  Register getOrigReg(const Register NewReg) const {
    auto It = NewRegToOldReg.find(NewReg);
    if (It == NewRegToOldReg.end())
      return 0;

    return It->second;
  }

  /// \return set of \p NewReg and all its alias Registers
  std::set<Register> getAliasRegs(const Register NewReg) const {
    Register OrigReg = getOrigReg(NewReg);
    if (!OrigReg)
      return {NewReg};

    auto AliasRegs = OldRegAliasRegs.at(OrigReg);
    AliasRegs.emplace(NewReg);
    return AliasRegs;
  }

  void dump() const {
    dbgs() << "Replaced Register Mapping:\n";
    for (auto It : OldRegAliasRegs) {
      for (auto MappedReg : It.second)
        dbgs() << printReg(It.first) << " -> " << printReg(MappedReg) << "\n";
    }
  }
};

class AIEClusterBaseAddress : public MachineFunctionPass {
  // Mapping between a single old Ptr Register and the to-be inserted new ptr
  // Registers.
  MBBRegMap CurrentChainRegBlockMap;
  // Mapping between all the newly inserted Ptr registers and the original
  // Ptr registers.
  ReplacedRegMap GlobalRegBlockMap;
  // Caching Reachable MBBs. Key is the MBB of interest and the Value is the set
  // of reachable MBBs of the key.
  std::map<MachineBasicBlock *, std::set<MachineBasicBlock *>> MFReachableMBB;

public:
  static char ID;
  AIEClusterBaseAddress() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  StringRef getPassName() const override { return AIE_CLUSTER_BASE_ADDRESS; }

  using RegUseMap = std::map<Register, SmallVector<MachineInstr *, 8>>;

private:
  MachineRegisterInfo *MRI = nullptr;
  MachineDominatorTree *MDT = nullptr;

  bool processBasicBlock(MachineBasicBlock &MBB, MachineIRBuilder &MIB,
                         GISelObserverWrapper &Observer);

  // Get all candidates, i.e. groups of G_PTR_ADDs in the same
  // basic block that shares the same input pointer.
  RegUseMap collectPtrUses(MachineBasicBlock &MBB);

  // Evaluate if we consider a group of G_PTR_ADDs as a candidate to
  // create a chain.
  bool shouldSkipChaining(Register PtrReg,
                          const SmallVector<MachineInstr *, 8> &Instrs,
                          MachineBasicBlock &MBB);

  // Build a chain (or set of chains) of G_PTR_ADDs. We consider as
  // chain a linear sequence of linked G_PTR_ADDs, tied to output and
  // input pointers.
  bool buildChain(const SmallVector<MachineInstr *, 8> &Instrs,
                  MachineBasicBlock &MBB, bool RevertPtrAddressChanges,
                  MachineIRBuilder &MIB, GISelObserverWrapper &Observer);

  // Evaluate if we should break the chain construction.
  // Criteria:
  //  * Unknown offsets.
  //  * Pointer shared between load(s) and store(s).
  bool shouldBreakChain(MachineInstr *MIA, MachineInstr *MIB,
                        const std::optional<int64_t> &OffsetA,
                        const std::optional<int64_t> &OffsetB);

  // Return true if the instructions are used by both loads and stores.
  bool hasMixedLoadStoreUse(SmallVector<MachineInstr *, 2> Instrs);

  // Get a set of all reachable MBBs from a given MBB.
  // Loops are handled using the ReachableMBBs set, once we encounter any
  // reachable MBB from a particular MBB, we store it in the set and continue if
  // we find it again. Lastly, we remove the current MBB from the set in case it
  // comes up in the successive basic blocks.
  std::set<MachineBasicBlock *> findReachableMBBs(MachineBasicBlock *MBB);

  /// \return whether \p Reg is used in reachable MBBs of \p MBB .
  bool isRegUsedInSuccessiveMBBs(MachineBasicBlock *MBB, Register Reg);

  /// Replace \p OldReg with \p NewReg in \p MI .
  bool replaceReg(MachineInstr &MI, Register OldReg, Register NewReg,
                  GISelObserverWrapper &Observer);

  /// Return a set of Load Instructions whose results are used in the path of
  /// the conditional branch of \p MBB .
  std::set<MachineInstr *>
  getLoadsFeedingCondBranch(MachineBasicBlock &MBB) const;

  /// \return whether PtrAdd is used in a Load Instruction that feeds a
  /// Conditional Jump.
  /// Example:
  /// %1 = G_PTR_ADD %0, xx
  /// %2 = G_LOAD %1
  /// %3 = G_ICMP %2, xx
  /// G_BRCOND %3
  bool isPtrAddUsedByLoadBranchCondition(
      MachineInstr *PtrAdd, std::set<MachineInstr *> &LoadsFeedingCondBranch);

  /// \return Phi Node in \p MBB that uses the Old Ptr Reg
  GPhi *findOldPtrRegPhi(MachineBasicBlock &MBB);

  /// \return Newly created skeleton Phi Node in \p MBB with all incoming
  /// registers set to the original Register (OldReg) of the ChainMapping
  GPhi *buildSkeletonPhiMI(MachineBasicBlock &MBB, MachineIRBuilder &MIB,
                           GISelObserverWrapper &Observer);

  /// Update \p Phi according to IncomingValues in \p ChainRegBlockMap
  void updateIncomingPhiValues(GPhi &Phi, const MBBRegMap ChainRegBlockMap,
                               GISelObserverWrapper &Observer);

  /// \return Phi Node of \p MBB that combines \p IncomingReg . If a new Phi has
  /// to be created, do so. Update Phis incoming values with \p ChainRegBlockMap
  GPhi *findOrCreatePhi(const Register IncomingReg, MachineBasicBlock &MBB,
                        MBBRegMap ChainRegBlockMap, MachineIRBuilder &MIB,
                        GISelObserverWrapper &Observer);

  /// Update RegMappings with Incoming Regs from existing Phi nodes in \p MF
  bool updatePhiRegMapping(MachineFunction &MF);

  /// \return whether \p MBB needs a PHI node based on incoming values from
  /// MBBRegMapping
  bool isPhiNeeded(MachineBasicBlock &MBB);

  /// Create Phi when RegMapping shows diverging incoming Registers for a MBB in
  /// \p MF . The Old Ptr is defined in the MachineInstr \p PtrDefMI .
  bool createPhisForRegMapping(MachineFunction &MF, MachineInstr *PtrDefMI,
                               MachineIRBuilder &MIB,
                               GISelObserverWrapper &Observer);

  /// Replace Register within \p MF with mapped Registers.
  void replaceRegs(MachineFunction &MF, MachineIRBuilder &MIB,
                   GISelObserverWrapper &Observer);

  /// Replace Base Pointer with restored Pointers in \p MF .
  void replacePtrGlobally(MachineFunction &MF, MachineIRBuilder &MIB,
                          GISelObserverWrapper &Observer);

  /// \return the sum of \p Offset and \p PtrAdd offset. If \p Subtract is true
  /// subtract Offset from \p PtrAdd offset.
  std::optional<int64_t> getUpdatedOffset(MachineInstr *PtrAdd,
                                          const std::optional<int64_t> &Offset,
                                          bool Subtract = false) const;

  /// \return whether \p Reg in \p MBB should be restored.
  bool isRestoreCandidate(const Register Reg, MachineBasicBlock &MBB);

  /// Replace \p OldReg with \p NewReg in \p MBB starting from Instruction
  /// \p SameMBBReplacementStart . Update the RegisterMapping of \p MBB and its
  /// successors. If a Mapping already exists it only gets updated when
  /// \p ForceRegUpdate is set. \return whether a register or a register mapping
  /// was changed.
  bool
  replaceDominatedUsesInMBB(const Register OldReg, const Register NewReg,
                            MachineBasicBlock::iterator SameMBBReplacementStart,
                            MachineBasicBlock *MBB, bool ForceRegUpdate,
                            MachineIRBuilder &MIB,
                            GISelObserverWrapper &Observer);

  /// Restore \p InputPtrReg by inserting a G_PTR_ADD with \p Offset after
  /// \p PtrAddMI and replacing \p InputPtrRegs uses with the new Ptr.
  /// \return New Base Pointer.
  Register restorePtrInMBB(const Register InputPtrReg, MachineInstr &PtrAddMI,
                           const int64_t Offset,
                           MachineBasicBlock::iterator SameMBBReplacementStart,
                           MachineIRBuilder &MIB,
                           GISelObserverWrapper &Observer);

  /// Update Register Mapping for \p MBB and its successors with \p NewReg .
  /// If existing Mapping exists, only update the mapping if \p ForceUpdate is
  /// set.
  /// Note: Successors are only updated, if they only have a single
  /// predecessor. Multi-predecessor Successors are handled by inserting Phi
  /// nodes.
  bool propagateNewRegister(const Register NewReg, MachineBasicBlock *MBB,
                            bool ForceUpdate);
};

void AIEClusterBaseAddress::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<MachineModuleInfoWrapperPass>();
  AU.addRequired<GISelCSEAnalysisWrapperPass>();
  AU.addRequired<TargetPassConfig>();
  AU.addRequired<MachineDominatorTreeWrapperPass>();
  // We do not actually invalidate any Required Pass Results.
  AU.setPreservesAll();
}

bool AIEClusterBaseAddress::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "MF: " << MF.getName() << "\n");
  // reset caches
  GlobalRegBlockMap = {};
  MFReachableMBB.clear();

  MRI = &MF.getRegInfo();
  MDT = &getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();
  TargetPassConfig &TPC = getAnalysis<TargetPassConfig>();
  // Enable CSE.
  GISelCSEAnalysisWrapper &Wrapper =
      getAnalysis<GISelCSEAnalysisWrapperPass>().getCSEWrapper();
  auto *CSEInfo = &Wrapper.get(TPC.getCSEConfig());
  std::unique_ptr<MachineIRBuilder> Builder =
      CSEInfo ? std::make_unique<CSEMIRBuilder>()
              : std::make_unique<MachineIRBuilder>();
  Builder->setMF(MF);
  MachineIRBuilder &MIB = *Builder;
  // Set Observer
  GISelObserverWrapper Observer;
  if (CSEInfo) {
    Observer.addObserver(CSEInfo);
    MIB.setChangeObserver(Observer);
  }

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    Changed |= processBasicBlock(MBB, MIB, Observer);
  }
  return Changed;
}

bool AIEClusterBaseAddress::processBasicBlock(MachineBasicBlock &MBB,
                                              MachineIRBuilder &MIB,
                                              GISelObserverWrapper &Observer) {
  LLVM_DEBUG(dbgs() << "Processing bb." << MBB.getNumber() << "\n");

  bool Changed = false;

  // Get all G_PTR_ADDs that use the same pointer.
  RegUseMap RegAndUses = collectPtrUses(MBB);

  // Optimise instruction order
  for (auto &RegAndUse : reverse(RegAndUses)) {
    // Chaining acceptance criteria.
    SmallVector<MachineInstr *, 8> &Instrs = RegAndUse.second;
    if (shouldSkipChaining(RegAndUse.first, Instrs, MBB))
      continue;

    ArrayRef<MachineInstr *> PtrAdds = RegAndUse.second;
    Changed |= bundleConstIncrements(PtrAdds, *MRI, MIB, Observer);
  }

  // Create chains, when profitable.
  for (auto [PtrReg, Instrs] : RegAndUses) {

    // Chaining acceptance criteria.
    if (shouldSkipChaining(PtrReg, Instrs, MBB))
      continue;

    const bool RevertPtrAddressChanges =
        EnableInputPtrRestore && isRegUsedInSuccessiveMBBs(&MBB, PtrReg);

    // Build chain, breaking it (or restarting it) when necessary
    Changed |= buildChain(Instrs, MBB, RevertPtrAddressChanges, MIB, Observer);
  }
  return Changed;
}

AIEClusterBaseAddress::RegUseMap
AIEClusterBaseAddress::collectPtrUses(MachineBasicBlock &MBB) {
  // Initialize Load cond Branch
  std::set<MachineInstr *> LoadsFeedingCondBranch;
  if (DetachCondLoadChain)
    LoadsFeedingCondBranch = getLoadsFeedingCondBranch(MBB);

  RegUseMap RegAndUses;
  for (MachineInstr &MI : MBB) {
    // Only consider G_PTR_ADDs
    if (MI.getOpcode() != TargetOpcode::G_PTR_ADD)
      continue;

    // If G_PTR_ADDs feeds a conditional branch through a load instruction,
    // ignore PtrAdd in chain collection. Otherwise the load will be placed last
    // in a postinc chain and thus delay the conditional branch decision.
    if (!LoadsFeedingCondBranch.empty() &&
        isPtrAddUsedByLoadBranchCondition(&MI, LoadsFeedingCondBranch))
      continue;

    RegAndUses[MI.getOperand(1).getReg()].push_back(&MI);
  }
  return RegAndUses;
}

bool AIEClusterBaseAddress::shouldSkipChaining(
    Register PtrReg, const SmallVector<MachineInstr *, 8> &Instrs,
    MachineBasicBlock &MBB) {

  if (Instrs.size() <= 1 || (!EnableChainsAcrossMultiBlocks &&
                             isRegUsedInSuccessiveMBBs(&MBB, PtrReg)))
    return true;

  return false;
}

/// Recursively search bottom up for Load instrs in the use chain of \p MI .
/// Stop the search when Exiting \p MBB .  The first time this function is
/// called, \p MI should be a conditional branch instruction. Return all found
/// Load MachineInstr in
/// \p LoadsFeedingInstrs .
void findLoadsFeedingInstr(MachineInstr &MI, MachineBasicBlock *MBB,
                           std::set<MachineInstr *> &LoadsFeedingInstrs,
                           MachineRegisterInfo &MRI) {
  for (MachineOperand &MO : MI.uses()) {
    if (!MO.isReg())
      continue;

    Register UseReg = MO.getReg();
    if (!UseReg.isVirtual())
      continue;

    auto *UseMI = MRI.getUniqueVRegDef(UseReg);
    if (!UseMI)
      continue;

    if (UseMI->getParent() != MBB || UseMI->isPHI())
      continue;

    if (UseMI->mayLoad()) {
      LoadsFeedingInstrs.emplace(UseMI);
      LLVM_DEBUG(dbgs() << "Found Feeding Load " << *UseMI);
    }

    findLoadsFeedingInstr(*UseMI, MBB, LoadsFeedingInstrs, MRI);
  }
}

std::set<MachineInstr *>
AIEClusterBaseAddress::getLoadsFeedingCondBranch(MachineBasicBlock &MBB) const {
  assert(MRI);
  std::set<MachineInstr *> LoadsFeedingCondBranch;
  for (auto &MI : make_range(MBB.getFirstTerminator(), MBB.end())) {
    if (MI.isConditionalBranch()) {
      findLoadsFeedingInstr(MI, &MBB, LoadsFeedingCondBranch, *MRI);
      break;
    }
  }

  return LoadsFeedingCondBranch;
}

bool AIEClusterBaseAddress::isPtrAddUsedByLoadBranchCondition(
    MachineInstr *PtrAdd, std::set<MachineInstr *> &LoadsFeedingCondBranch) {
  assert(PtrAdd->getOpcode() == TargetOpcode::G_PTR_ADD);

  // Is G_PTR_ADD feeding a Load instruction?
  const Register DefReg = PtrAdd->getOperand(0).getReg();
  if (MRI->use_nodbg_empty(DefReg))
    return false;

  auto UseBegin = MRI->use_instr_nodbg_begin(DefReg);
  MachineInstr *LoadMI = &*UseBegin;
  if (!LoadMI->mayLoad())
    return false;

  const bool LoadFeedCondBranch = LoadsFeedingCondBranch.count(LoadMI);
  LLVM_DEBUG(if (LoadFeedCondBranch) dbgs()
                 << "Found Load feeding Cond Branch attached to " << *PtrAdd;);

  return LoadFeedCondBranch;
}

GPhi *AIEClusterBaseAddress::findOldPtrRegPhi(MachineBasicBlock &MBB) {
  const Register IncomingReg = CurrentChainRegBlockMap.getOldReg();
  for (MachineInstr &MI : MBB.phis()) {
    // Find Phi Node with IncomingReg
    auto *Phi = dyn_cast<GPhi>(&MI);
    for (unsigned Idx = 0; Idx < Phi->getNumIncomingValues(); ++Idx) {
      // Match any of IncomingRegs alias register in Phi node.
      for (auto Reg : GlobalRegBlockMap.getAliasRegs(IncomingReg)) {
        if (Phi->getIncomingValue(Idx) == Reg)
          return Phi; // Found Phi with IncomingReg
      }
    }
  }
  return nullptr;
}

GPhi *
AIEClusterBaseAddress::buildSkeletonPhiMI(MachineBasicBlock &MBB,
                                          MachineIRBuilder &MIB,
                                          GISelObserverWrapper &Observer) {
  const Register ReferenceReg = CurrentChainRegBlockMap.getOldReg();
  // Set Insertion Point to MBB beginning
  MIB.setInsertPt(MBB, MBB.begin());

  // Create Phi Instruction.
  const Register NewBasePtrReg =
      MRI->createGenericVirtualRegister(MRI->getType(ReferenceReg));
  auto PHIBuilder = MIB.buildInstr(TargetOpcode::G_PHI).addDef(NewBasePtrReg);

  // Assign Incoming Values with default values.
  for (MachineBasicBlock *Pred : MBB.predecessors()) {
    PHIBuilder.addUse(ReferenceReg);
    PHIBuilder.addMBB(Pred);
  }

  GPhi *Phi = dyn_cast<GPhi>(PHIBuilder.getInstr());
  LLVM_DEBUG(dbgs() << "Phi Node Created in bb." << MBB.getNumber() << " "
                    << *Phi);

  // Notify Observer
  Observer.createdInstr(*Phi);
  // Notify Machine Function Properties, that the MF has PHI nodes.
  MBB.getParent()->getProperties().reset(
      MachineFunctionProperties::Property::NoPHIs);

  return Phi;
}

void AIEClusterBaseAddress::updateIncomingPhiValues(
    GPhi &Phi, const MBBRegMap ChainRegBlockMap,
    GISelObserverWrapper &Observer) {
  LLVM_DEBUG(dbgs() << "Updating " << Phi);
  Observer.changingInstr(Phi);
  for (unsigned Idx = 0; Idx < Phi.getNumIncomingValues(); ++Idx) {
    MachineBasicBlock *IncomingMBB = Phi.getIncomingBlock(Idx);

    const Register NewReg = ChainRegBlockMap.getNewReg(IncomingMBB);
    if (!NewReg)
      continue; // No new mapping available, so nothing to replace

    Phi.getOperand(Idx * 2 + 1).setReg(NewReg);
    LLVM_DEBUG(dbgs() << printReg(Phi.getIncomingValue(Idx)) << " to "
                      << printReg(NewReg) << " in bb."
                      << IncomingMBB->getNumber() << "\n");
  }
  Observer.changedInstr(Phi);
}

GPhi *AIEClusterBaseAddress::findOrCreatePhi(const Register IncomingReg,
                                             MachineBasicBlock &MBB,
                                             MBBRegMap ChainRegBlockMap,
                                             MachineIRBuilder &MIB,
                                             GISelObserverWrapper &Observer) {

  GPhi *Phi = findOldPtrRegPhi(MBB);
  if (Phi) {
    LLVM_DEBUG(dbgs() << "Skipping, found existing Phi node " << *Phi);
    return Phi;
  }

  // Create new Phi node.
  Phi = buildSkeletonPhiMI(MBB, MIB, Observer);

  // Update Register Mapping
  const Register DefReg = Phi->getOperand(0).getReg();
  ChainRegBlockMap.updateMappedReg(&MBB, DefReg);
  return Phi;
}

bool AIEClusterBaseAddress::updatePhiRegMapping(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "Updating existing Reg Mappings from Phis\n");
  bool Changed = false;

  for (auto &MBB : MF) {
    GPhi *Phi = findOldPtrRegPhi(MBB);
    if (!Phi)
      continue;

    // Update MBBRegMap with existing Phi node registers
    LLVM_DEBUG(dbgs() << "Found existing Phi " << *Phi);
    for (unsigned Idx = 0; Idx < Phi->getNumIncomingValues(); ++Idx) {
      MachineBasicBlock *IncomingMBB = Phi->getIncomingBlock(Idx);
      const Register IncomingValue = Phi->getIncomingValue(Idx);

      // If the MBB already has an updated outgoing value from a Restore
      // G_PTR_ADD, no need to update the Registermapping.
      Changed |=
          CurrentChainRegBlockMap.updateMappedReg(IncomingMBB, IncomingValue,
                                                  /*ForceUpdate=*/false);
    }
  }
  LLVM_DEBUG(dbgs() << "Phi Changed = " << Changed << "\n");
  return Changed;
}

bool AIEClusterBaseAddress::isPhiNeeded(MachineBasicBlock &MBB) {
  std::optional<Register> CurrentIncomingReg;
  for (auto *PredMBB : MBB.predecessors()) {
    Register IncomingReg = CurrentChainRegBlockMap.getReg(PredMBB);
    LLVM_DEBUG(dbgs() << "bb." << PredMBB->getNumber()
                      << " NewReg = " << printReg(IncomingReg) << "\n");

    if (CurrentIncomingReg && *CurrentIncomingReg != IncomingReg)
      return true; // Multiple different Incoming Registers, ergo PHI needed.

    if (!CurrentIncomingReg)
      CurrentIncomingReg = IncomingReg;
  }
  return false;
}

bool AIEClusterBaseAddress::createPhisForRegMapping(
    MachineFunction &MF, MachineInstr *PtrDefMI, MachineIRBuilder &MIB,
    GISelObserverWrapper &Observer) {
  LLVM_DEBUG(dbgs() << "Creating new Phi Nodes\n");
  bool Changed = false;

  const Register OrigPtrReg = CurrentChainRegBlockMap.getOldReg();
  for (auto &MBB : MF) {
    if (MBB.pred_size() <= 1)
      continue;

    if (!isPhiNeeded(MBB)) {
      // Incoming values to MBB are equal, no Phi needed.
      const Register IncomingReg =
          CurrentChainRegBlockMap.getNewReg((*MBB.pred_begin()));
      LLVM_DEBUG(dbgs() << "No need to Insert Phi in bb." << MBB.getNumber()
                        << " propagating " << printReg(IncomingReg)
                        << " to successors\n");
      if (!IncomingReg)
        continue;

      // Replace all OrigPtrReg with IncomingReg
      Changed |= replaceDominatedUsesInMBB(OrigPtrReg, IncomingReg, MBB.begin(),
                                           &MBB, false, MIB, Observer);
      continue;
    }

    MachineInstr *FirstMI = &*MBB.instr_begin();
    if (PtrDefMI == FirstMI || !MDT->dominates(PtrDefMI, FirstMI)) {
      LLVM_DEBUG(dbgs() << "Not inserting PHi node, bb." << MBB.getNumber()
                        << " is not dominated by PtrDefMI\n");
      continue;
    }

    GPhi *Phi = findOrCreatePhi(OrigPtrReg, MBB, CurrentChainRegBlockMap, MIB,
                                Observer);
    assert(Phi);

    // Replace OrigPtrReg with the newly created Phi Def.
    // Do not update Phi itself.
    const Register PhiDefReg = Phi->getOperand(0).getReg();
    Changed |=
        replaceDominatedUsesInMBB(OrigPtrReg, PhiDefReg, MBB.getFirstNonPHI(),
                                  &MBB, false, MIB, Observer);
  }
  LLVM_DEBUG(CurrentChainRegBlockMap.dump());
  LLVM_DEBUG(dbgs() << "Creating Phis for Mapping Changed = " << Changed
                    << "\n");
  return Changed;
}

void AIEClusterBaseAddress::replaceRegs(MachineFunction &MF,
                                        MachineIRBuilder &MIB,
                                        GISelObserverWrapper &Observer) {
  LLVM_DEBUG(dbgs() << "Replace OldReg with NewReg in each MBB.\n");
  const Register OrigPtrReg = CurrentChainRegBlockMap.getOldReg();
  for (auto [Idx, MBB] : enumerate(MF)) {
    if (MBB.pred_empty())
      continue;

    // Replace OrigPtrReg with Incoming Reg from the previous MBB.
    if (MBB.pred_size() == 1) {
      MachineBasicBlock *IncomingMBB = *MBB.pred_begin();
      const Register IncomingReg =
          CurrentChainRegBlockMap.getNewReg(IncomingMBB);
      if (!IncomingReg)
        continue;

      // No Phi nodes, so start replacing from the beginning of the MBB.
      replaceDominatedUsesInMBB(OrigPtrReg, IncomingReg, MBB.begin(), &MBB,
                                /*ForceRegUpdate=*/false, MIB, Observer);
      continue;
    }

    // Insert Phi node to combine incoming Registers.
    if (GPhi *Phi = findOldPtrRegPhi(MBB)) {
      // Not every MBB with multiple predecessors may have a phi node (e.g.
      // InputPtr was not modified by MBBs predecessors).
      updateIncomingPhiValues(*Phi, CurrentChainRegBlockMap, Observer);
    }
  }
}

void AIEClusterBaseAddress::replacePtrGlobally(MachineFunction &MF,
                                               MachineIRBuilder &MIB,
                                               GISelObserverWrapper &Observer) {
  LLVM_DEBUG(GlobalRegBlockMap.dump());

  Register OldPtrReg = CurrentChainRegBlockMap.getOldReg();
  Register OrigOldPtrReg = GlobalRegBlockMap.getOrigReg(OldPtrReg);
  MachineInstr *PtrDefMI = OrigOldPtrReg ? MRI->getVRegDef(OrigOldPtrReg)
                                         : MRI->getVRegDef(OldPtrReg);
  LLVM_DEBUG(dbgs() << "Old PtrDef: " << *PtrDefMI);

  bool Changed = true;
  // Update Phi Register Mappings and create Phis where necessary.
  //
  // Only a Phi node can modify the outgoing register of its MBB, potentially
  // restarting the propagation loop. However, after a Phi node is inserted and
  // its value propagates, it only modifies its *incoming* registers. Since
  // incoming register changes don't trigger further Phi node insertions, the
  // algorithm is guaranteed to converge.
  while (Changed) {
    // Phi Reg Mapping may change:
    // - initial phi mapping (assign incoming registers)
    // - only if a new Phi is created.
    Changed = updatePhiRegMapping(MF);

    // If a phi updates an outgoing register, another Phi may be inserted. But
    // if existing phis do not change, there can be no newly created Phi node.
    Changed |= createPhisForRegMapping(MF, PtrDefMI, MIB, Observer);
  }

  // RegisterMapping is stable. Now replace all the OldRegs with the new
  // mapped Registers.
  replaceRegs(MF, MIB, Observer);

  // Update MF persistent RegMap with newly created Register mappings.
  GlobalRegBlockMap.addMapping(CurrentChainRegBlockMap);
}

std::optional<int64_t>
AIEClusterBaseAddress::getUpdatedOffset(MachineInstr *PtrAdd,
                                        const std::optional<int64_t> &Offset,
                                        bool Subtract) const {
  assert(PtrAdd->getOpcode() == TargetOpcode::G_PTR_ADD);
  if (!Offset)
    return {};

  std::optional<ValueAndVReg> CstOffset =
      getIConstantVRegValWithLookThrough(PtrAdd->getOperand(2).getReg(), *MRI);
  if (!CstOffset)
    return {};

  const int64_t NewOffset = CstOffset->Value.getSExtValue();
  const int64_t FinalOffset =
      Subtract ? NewOffset - *Offset : NewOffset + *Offset;
  return FinalOffset;
}

bool AIEClusterBaseAddress::buildChain(
    const SmallVector<MachineInstr *, 8> &Instrs, MachineBasicBlock &MBB,
    bool RevertPtrAddressChanges, MachineIRBuilder &MIB,
    GISelObserverWrapper &Observer) {
  assert(Instrs.size() > 1);

  // Init Register mapping for the input pointer
  const Register InputPtr = Instrs[0]->getOperand(1).getReg();
  CurrentChainRegBlockMap = {InputPtr};
  RevertPtrAddressChanges =
      RevertPtrAddressChanges ? isRestoreCandidate(InputPtr, MBB) : false;

  bool Changed = false;
  std::optional<int64_t> AccumulatedOffset{0};
  for (unsigned I = 0; I < Instrs.size() - 1; I++) {
    MachineInstr *MI = Instrs[I];
    assert(MI->getParent() == &MBB);
    MachineInstr *MINext = Instrs[I + 1];

    const std::optional<int64_t> TmpAccOffset =
        getUpdatedOffset(MI, AccumulatedOffset);
    const std::optional<int64_t> NewNextOffset =
        getUpdatedOffset(MINext, TmpAccOffset, /*Subtract=*/true);
    // Evaluate if we should restart the chain from the base
    // pointer. This is necessary when we deal with unknown offsets
    // (not constants) and desirable when we share pointers between
    // loads and stores (avoiding dependencies).
    if (shouldBreakChain(MI, MINext, TmpAccOffset, NewNextOffset)) {
      if (RevertPtrAddressChanges && RestoreBrokenChains) {
        LLVM_DEBUG(dbgs() << "Breaking Chain at " << *MI
                          << "New Chain starts with " << *MINext << "\n");

        // If chain is broken by MI, use last known Offset Value
        // (AccumulatedOffset) to restore the pointer, since TmpAccOffset will
        // be invalid.
        const std::optional<int64_t> Offset =
            TmpAccOffset ? TmpAccOffset : AccumulatedOffset;
        // Replace all Occurrences of InputPtr past broken chain (MINext) with
        // the new BasePtr
        restorePtrInMBB(InputPtr, *MI, *Offset, MINext->getIterator(), MIB,
                        Observer);
      }
      AccumulatedOffset = {0};
      continue;
    }
    AccumulatedOffset = TmpAccOffset;

    MIB.setInsertPt(MBB, MINext->getIterator());

    Register NewOffsetReg =
        MIB.buildConstant(LLT::scalar(20), *NewNextOffset).getReg(0);

    Observer.changingInstr(*MINext);
    MINext->getOperand(1).setReg(MI->getOperand(0).getReg());
    MINext->getOperand(2).setReg(NewOffsetReg);
    Observer.changedInstr(*MINext);
    Changed = true;
  }

  if (RevertPtrAddressChanges) {
    // Accumulated Offset is missing the last G_PTR_ADD Offset, update it
    // accordingly
    const std::optional<int64_t> FinalOffset =
        getUpdatedOffset(Instrs.back(), AccumulatedOffset);
    // If chain is broken by Instrs.back(), use last known Offset Value
    // (AccumulatedOffset) to restore the pointer, since FinalOffset will be
    // invalid.
    const std::optional<int64_t> Offset =
        FinalOffset ? FinalOffset : AccumulatedOffset;

    // Do not replace any MIs within the MBB, since the Restore G_PTR_ADD is
    // the last use of the InputPtr
    auto StartReplacingMIs = MBB.end();
    restorePtrInMBB(InputPtr, *Instrs.back(), *Offset, StartReplacingMIs, MIB,
                    Observer);

    replacePtrGlobally(*MBB.getParent(), MIB, Observer);
  }

  return Changed;
}

bool AIEClusterBaseAddress::isRestoreCandidate(const Register Reg,
                                               MachineBasicBlock &MBB) {
  auto *DefMI = MRI->getVRegDef(Reg);
  if (!DefMI) {
    LLVM_DEBUG(dbgs() << "No Restore Candidate " << printReg(Reg) << " in bb."
                      << MBB.getNumber() << " : Could not find Def\n");
    return false;
  }

  return true;
}

bool AIEClusterBaseAddress::shouldBreakChain(
    MachineInstr *MIA, MachineInstr *MIB, const std::optional<int64_t> &OffsetA,
    const std::optional<int64_t> &OffsetB) {

  // If one of the offsets is not constant, it is better to break the chain.
  if (!OffsetA || !OffsetB)
    return true;

  return hasMixedLoadStoreUse({MIA, MIB});
}

bool AIEClusterBaseAddress::hasMixedLoadStoreUse(
    SmallVector<MachineInstr *, 2> Instrs) {
  unsigned LoadCount = 0;
  unsigned StoreCount = 0;
  for (MachineInstr *MI : Instrs) {
    const Register PtrReg = MI->getOperand(0).getReg();
    for (const MachineInstr &UseMI : MRI->use_instructions(PtrReg)) {
      if (!UseMI.mayLoadOrStore())
        continue;
      if (UseMI.mayLoad())
        LoadCount++;
      else
        StoreCount++;
      const LLT MemType = getLoadStoreType(UseMI, *MRI);
      // If desired, we also can break the chain between pairs of
      // pointers that are used to load/store vectors and/or scalars.
      if ((!EnableChainsForScalarLdSt && MemType.isScalar()) ||
          (!EnableChainsForVectorLdSt && MemType.isVector()))
        return true;
    }
  }
  return (LoadCount > 0 && StoreCount > 0);
}

std::set<MachineBasicBlock *>
AIEClusterBaseAddress::findReachableMBBs(MachineBasicBlock *MBB) {
  // Look up if MBB is already in the Cache
  auto It = MFReachableMBB.find(MBB);
  if (It != MFReachableMBB.end())
    return It->second;

  std::set<MachineBasicBlock *> ReachableMBBs;
  SmallVector<MachineBasicBlock *, 8> Worklist;
  Worklist.append(MBB->succ_begin(), MBB->succ_end());
  while (!Worklist.empty()) {
    MachineBasicBlock *CurrMBB = Worklist.pop_back_val();
    if (!ReachableMBBs.insert(CurrMBB).second)
      continue;
    Worklist.append(CurrMBB->succ_begin(), CurrMBB->succ_end());
  }
  // Remove the starting MBB from the ReachableMBBs set since we don't want to
  // be too pessimistic as to not consider uses in the current basic block.
  ReachableMBBs.erase(MBB);

  // Update Reachable Cache
  MFReachableMBB.insert({MBB, ReachableMBBs});
  return ReachableMBBs;
}

bool AIEClusterBaseAddress::isRegUsedInSuccessiveMBBs(MachineBasicBlock *MBB,
                                                      Register Reg) {
  std::set<MachineBasicBlock *> ReachableMBBs = findReachableMBBs(MBB);
  for (auto AltReg : GlobalRegBlockMap.getAliasRegs(Reg)) {
    for (MachineInstr &Use : MRI->use_nodbg_instructions(AltReg)) {
      if (ReachableMBBs.count(Use.getParent()))
        return true;
    }
  }

  return false;
}

bool AIEClusterBaseAddress::replaceReg(MachineInstr &MI, Register OldReg,
                                       Register NewReg,
                                       GISelObserverWrapper &Observer) {
  assert(NewReg);
  LLVM_DEBUG(dbgs() << "  bb." << MI.getParent()->getNumber() << " Replacing "
                    << printReg(OldReg) << " with " << printReg(NewReg) << "in "
                    << MI);
  if (OldReg == NewReg)
    return false;

  bool Changed = false;
  for (MachineOperand &MO : MI.uses()) {
    if (MO.isReg() && MO.getReg() == OldReg) {
      if (!Changed)
        Observer.changingInstr(MI);
      MO.setReg(NewReg);
      Changed = true;
    }
  }

  if (Changed)
    Observer.changedInstr(MI);
  return Changed;
}

bool AIEClusterBaseAddress::replaceDominatedUsesInMBB(
    const Register OldReg, const Register NewReg,
    MachineBasicBlock::iterator SameMBBReplacementStart, MachineBasicBlock *MBB,
    bool ForceRegUpdate, MachineIRBuilder &MIB,
    GISelObserverWrapper &Observer) {
  assert(NewReg);

  // Collect Instructions that may need Register replacement.
  std::set<const MachineInstr *> MIsSameMBB;
  for (auto It = SameMBBReplacementStart; It != MBB->end(); ++It)
    MIsSameMBB.insert(&*It);

  // Collect all the uses of OldReg for replacement. Replacing the registers
  // inplace corrupts iterating through use_nodbg_instructions. This leads to
  // only replacing the first occurrence, instead of every occurrence.
  std::vector<MachineInstr *> ReplaceRegMIs;
  for (MachineInstr &Use : MRI->use_nodbg_instructions(OldReg)) {
    if (Use.getParent() != MBB || !MIsSameMBB.count(&Use))
      continue;

    ReplaceRegMIs.emplace_back(&Use);
  }

  // Replace OldReg with NewReg
  bool Changed = false;
  for (auto *MI : ReplaceRegMIs)
    Changed |= replaceReg(*MI, OldReg, NewReg, Observer);

  // Map InputPtrReg to newly created Register of MBB
  Changed |= propagateNewRegister(NewReg, MBB, ForceRegUpdate);
  return Changed;
}

bool AIEClusterBaseAddress::propagateNewRegister(const Register NewReg,
                                                 MachineBasicBlock *MBB,
                                                 bool ForceUpdate) {

  LLVM_DEBUG(dbgs() << "    Updating bb." << MBB->getNumber() << " with "
                    << printReg(NewReg) << "\n");
  bool Changed =
      CurrentChainRegBlockMap.updateMappedReg(MBB, NewReg, ForceUpdate);

  // propagate Register mapping to single-predecessor successors.
  for (auto *SuccMBB : MBB->successors())
    if (SuccMBB->pred_size() == 1 && SuccMBB != MBB)
      Changed |= propagateNewRegister(NewReg, SuccMBB, ForceUpdate);

  return Changed;
}

Register AIEClusterBaseAddress::restorePtrInMBB(
    const Register InputPtrReg, MachineInstr &PtrAddMI, const int64_t Offset,
    MachineBasicBlock::iterator SameMBBReplacementStart, MachineIRBuilder &MIB,
    GISelObserverWrapper &Observer) {
  assert(PtrAddMI.getOpcode() == TargetOpcode::G_PTR_ADD);

  if (Offset == 0) {
    LLVM_DEBUG(dbgs() << "bb." << PtrAddMI.getParent()->getNumber()
                      << " skipping, no Offset to restore\n");
    return 0;
  }

  // Create G_PTR_ADD to reverse modifications introduced by address chaining.
  auto CreatePtrAdd = [&](MachineInstr &LastPtrAdd) -> MachineInstr * {
    auto InsertionPoint = std::next(LastPtrAdd.getIterator());

    MIB.setInsertPt(*PtrAddMI.getParent(), InsertionPoint);

    auto LastPtrMO = LastPtrAdd.getOperand(0);

    Register NewOffsetReg =
        MIB.buildConstant(LLT::scalar(20), -Offset).getReg(0);
    const LLT PtrType{MRI->getType(LastPtrMO.getReg())};
    Register ResultPtrReg = MRI->createGenericVirtualRegister(PtrType);

    // Create new G_Ptr_ADD
    return MIB.buildPtrAdd(ResultPtrReg, LastPtrMO, NewOffsetReg);
  };

  MachineInstr *RestorePtrAdd = CreatePtrAdd(PtrAddMI);
  const Register RestorePtrReg = RestorePtrAdd->getOperand(0).getReg();

  MachineBasicBlock *MBB = RestorePtrAdd->getParent();

  LLVM_DEBUG(dbgs() << "Replacing Uses in MBB " << MBB->getNumber()
                    << " changing " << printReg(InputPtrReg) << " to "
                    << printReg(RestorePtrReg) << "\n");

  // Force Register Map updates. Broken Chains will already update register
  // mapping, but only the last update is relevant. Thus forceUpdate is
  // required.
  replaceDominatedUsesInMBB(InputPtrReg, RestorePtrReg, SameMBBReplacementStart,
                            MBB,
                            /*ForceRegUpdate=*/true, MIB, Observer);

  return RestorePtrReg;
}

} // namespace

char AIEClusterBaseAddress::ID = 0;
INITIALIZE_PASS_BEGIN(AIEClusterBaseAddress, DEBUG_TYPE,
                      AIE_CLUSTER_BASE_ADDRESS, false, false)
INITIALIZE_PASS_DEPENDENCY(GISelCSEAnalysisWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineModuleInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_END(AIEClusterBaseAddress, DEBUG_TYPE, AIE_CLUSTER_BASE_ADDRESS,
                    false, false)

namespace llvm {
MachineFunctionPass *createAIEClusterBaseAddress() {
  return new AIEClusterBaseAddress();
}
} // namespace llvm
