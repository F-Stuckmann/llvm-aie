//===-- AIEMetaData.cpp - Top-level interface for AIE -----------------*- C++
//-*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2024 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/AIEMetaData.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

#define DEBUG_TYPE "aie-metadata"

using namespace llvm;

void addAssumeToLoopPreheader(Loop &L, ScalarEvolution &SE, AssumptionCache &AC,
                              uint64_t MinIterCount, const DominatorTree &DT,
                              LLVMContext *Context);

PreservedAnalyses AIEMetaData::run(Loop &L, LoopAnalysisManager &AM,
                                   LoopStandardAnalysisResults &AR,
                                   LPMUpdater &U) {
  Context = &L.getHeader()->getParent()->getContext();
  ScalarEvolution &SE = AR.SE;
  DominatorTree DT = DominatorTree(*L.getHeader()->getParent());

  std::optional<int> MinIterCount = getMinIterCounts(&L);
  // since we assume that t

  // dump loop summary
  LLVM_DEBUG(dbgs() << "Preheader:");
  if (L.getLoopPreheader())
    LLVM_DEBUG(dbgs() << L.getLoopPreheader()->getName());
  LLVM_DEBUG(dbgs() << "\nHeader:");
  if (L.getHeader())
    LLVM_DEBUG(dbgs() << L.getHeader()->getName());
  LLVM_DEBUG(dbgs() << "\n");

  if (MinIterCount.has_value()) {
    LLVM_DEBUG(dbgs() << "Processing Loop Metadata of "
                      << L.getHeader()->getParent()->getName() << " "
                      << L.getName() << " (" << MinIterCount.value() << ")\n");
    LLVM_DEBUG(L.getHeader()->getParent()->dump(););
    addAssumeToLoopPreheader(L, SE, AR.AC, MinIterCount.value(), DT, Context);
  }

  LLVM_DEBUG(dbgs() << "Dumping Full Function:"
                    << L.getHeader()->getParent()->getName() << "\n";
             L.getHeader()->getParent()->dump(););
  return PreservedAnalyses::all();
}

bool isIncrement(const SCEV *S) {
  switch (S->getSCEVType()) {
  case scAddRecExpr: {
    const SCEVAddRecExpr *AR = cast<SCEVAddRecExpr>(S);
    assert(AR->getNumOperands() == 2 &&
           "Unknown Handling of more than 2 Operands");
    return cast<SCEVConstant>(*AR->getOperand(1)).getValue()->getSExtValue() >
           0;
    break;
  }

  default:
    assert(false && "Could not extract Iteration Variable from ");
  }
  return false;
}

Value *calcMinValue(const SCEV *S, int MinIterCount, LLVMContext *Context) {
  // get start point

  // increment it by
  int IncValue = 0;
  switch (S->getSCEVType()) {
  case scAddRecExpr: {
    const SCEVAddRecExpr *AR = cast<SCEVAddRecExpr>(S);
    assert(AR->getNumOperands() == 2 &&
           "Unknown Handling of more than 2 Operands");

    if (!isa<SCEVConstant>(*AR->getOperand(1))) {
      // FIXME: variable increments are not supported, since iteration direction
      // is unkown! return cast<SCEVUnknown>(*AR->getOperand(1)).getValue();

      LLVM_DEBUG(dbgs() << "could not extract Increment of SCEV "; S->dump());
      return nullptr;
    }

    IncValue =
        cast<SCEVConstant>(*AR->getOperand(1)).getValue()->getSExtValue();
    break;
  }

  default:
    assert(false && "Could not extract Iteration Variable from ");
  }

  int MaxValue = abs(IncValue * MinIterCount);
  // SGT is used to compare, so I must subtract 1
  MaxValue--;

  llvm::ConstantInt *ConstIncValue =
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Context), MaxValue, true);
  return static_cast<llvm::Value *>(ConstIncValue);
}

Value *getIterationVariable(const SCEV *S) {
  switch (S->getSCEVType()) {
  case scAddRecExpr: {
    const SCEVAddRecExpr *AR = cast<SCEVAddRecExpr>(S);
    if (!isa<SCEVUnknown>(*AR->getOperand(0)))
      return nullptr;

    assert(AR->getNumOperands() == 2 &&
           "Unknown Handling of more than 2 Operands");
    return cast<SCEVUnknown>(*AR->getOperand(0)).getValue();
  }

  default:
    assert(false && "Could not extract Iteration Variable from ");
  }
}

Value *getIterationVariable(const Loop &L, const PHINode *InductionPHI) {

  BranchInst *BI = dyn_cast<BranchInst>(L.getExitingBlock()->getTerminator());
  if (!BI || !BI->isConditional()) {
    assert(false && "Exiting block does not have a conditional branch.\n");
  }

  Value *Condition = BI->getCondition();
  ICmpInst *ICmp = dyn_cast<ICmpInst>(Condition);
  LLVM_DEBUG(dbgs() << "Branch Instruction Found: "; ICmp->dump();
             dbgs() << "\n");
  if (!ICmp) {
    assert(false && "Loop exit condition is not an integer comparison.\n");
  }

  Value *InductionVar;
  for (uint I = 0; I < ICmp->getNumOperands(); I++) {
    if (ICmp->getOperand(I) != InductionVar)
      InductionVar = ICmp->getOperand(I);
  }

  if (!InductionVar) {
    LLVM_DEBUG(
        dbgs() << "Induction variable not found in loop exit condition.\n";
        L.getHeader()->dump(); dbgs() << "\n"; BI->dump());
    return nullptr; // assert(false && "Induction variable not found in loop
                    // exit condition.\n");
  }

  return InductionVar;
}

// Function to add an assume in the loop preheader
void addAssumeToLoopPreheader(Loop &L, ScalarEvolution &SE, AssumptionCache &AC,
                              uint64_t MinIterCount, const DominatorTree &DT,
                              LLVMContext *Context) {
  // Retrieve the Loop Preheader
  BasicBlock *Preheader = L.getLoopPreheader();
  if (!Preheader) {
    errs() << "Loop does not have a preheader.\n";
    return;
  }

  Module *M = Preheader->getModule();

  // Identify the Loop Induction Variable and Limit
  PHINode *InductionPHI = nullptr;

  // Find the canonical induction variable
  const SCEV *S;
  for (PHINode &PN : L.getHeader()->phis()) {
    if (!SE.isSCEVable(PN.getType()))
      continue;
    S = SE.getSCEV(&PN);

    if (const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(S)) {
      if (AR->getLoop() == &L) {
        LLVM_DEBUG(dbgs() << "Found SCEV "; S->dump());
        InductionPHI = &PN;
        break;
      }
    }
    LLVM_DEBUG(dbgs() << "Phi "; S->dump());
  }
  LLVM_DEBUG(dbgs() << "          Induction Variable "; InductionPHI->dump(););

  Value *MinValue = calcMinValue(S, MinIterCount, Context);
  if (!MinValue) {
    return;
  }

  Value *MaxBoundry = nullptr;
  if (isIncrement(S)) {
    MaxBoundry = getIterationVariable(L, InductionPHI);
  } else {
    MaxBoundry = getIterationVariable(S);
  }

  if (!MaxBoundry) {
    LLVM_DEBUG(dbgs() << "AIEMetadata-Warning: Could not find Iteration "
                         "Variable. Will not process Metadata\n");
    return;
  }

  if (isa<Constant>(MaxBoundry)) {
    LLVM_DEBUG(dbgs() << "Iteration Variable (Max value) is an integer and "
                         "therefore no assumption "
                         "has to be added!");
    return;
  }

  LLVM_DEBUG(dbgs() << "Minimum Value : "; MinValue->dump());
  LLVM_DEBUG(dbgs() << "Max Value : "; MaxBoundry->dump());

  IRBuilder<> Builder(Preheader->getTerminator());

  // if Limit Instruction is not in the preheader, add it so that the assertion
  // has a defined start point.
  if (isa<Instruction>(MaxBoundry)) {
    Instruction *LimitInstruction = dyn_cast<Instruction>(MaxBoundry);
    if (LimitInstruction->getParent() != Preheader &&
        !DT.dominates(LimitInstruction->getParent(), Preheader)) {
      if (dyn_cast<BranchInst>(Preheader->getTerminator())->isUnconditional() &&
          L.hasLoopInvariantOperands(LimitInstruction)) {
        assert(LimitInstruction->isSafeToRemove());
        LLVM_DEBUG(dbgs() << "Moving Max Value (" << LimitInstruction
                          << ") to Preheader: " << Preheader->getName()
                          << "\n");
        // what happens if the uses are defined in the BB?
        LimitInstruction->moveBefore(Preheader->getTerminator());
      } else {
        LLVM_DEBUG(
            dbgs() << "AIEMetadata-Warning: cannot hoist LimitInstruciton to "
                      "Preheader, will abort \n");

        return;
      }
    }
  }

  Value *Cmp = nullptr;
  Cmp = Builder.CreateICmpSGT(MaxBoundry, MinValue);

  LLVM_DEBUG(dbgs() << "Inserting Condition:"; MinValue->dump();
             dbgs() << "With Comparator:"; Cmp->dump());

  // Insert the `llvm.assume` Call
  Function *AssumeFn = Intrinsic::getDeclaration(M, Intrinsic::assume);
  CallInst *Call = Builder.CreateCall(AssumeFn, Cmp);
  Call->setTailCall(true);
  AC.registerAssumption(dyn_cast<AssumeInst>(Call));
}
