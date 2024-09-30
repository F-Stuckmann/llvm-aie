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
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

#define DEBUG_TYPE "aie-metadata"

using namespace llvm;

void addAssumeToLoopPreheader(Loop &L, ScalarEvolution &SE, AssumptionCache &AC,
                              uint64_t MinIterCount, LLVMContext *Context);

PreservedAnalyses AIEMetaData::run(Loop &L, LoopAnalysisManager &AM,
                                   LoopStandardAnalysisResults &AR,
                                   LPMUpdater &U) {
  Context = &L.getHeader()->getParent()->getContext();
  ScalarEvolution &SE = AR.SE;

  std::optional<int> MinIterCount = getMinIterCounts(&L);
  // since we assume that t

  if (L.getLoopPreheader())
    LLVM_DEBUG(dbgs() << "Found Preheader: " << L.getLoopPreheader()->getName()
                      << "\n");
  LLVM_DEBUG(dbgs() << "Preheader conditional check:"
                    << cast<BranchInst>(L.getLoopPreheader()->getTerminator())
                           ->isConditional()
                    << "\n");
  if (cast<BranchInst>(L.getLoopPreheader()->getTerminator())->isConditional())
    LLVM_DEBUG(dbgs() << "Terminator Condition :";
               cast<BranchInst>(L.getLoopPreheader()->getTerminator())
                   ->getCondition()
                   ->dump();
               dbgs() << "\n");

  if (MinIterCount.has_value()) {
    LLVM_DEBUG(dbgs() << "Processing Loop Metadata of " << L.getName() << "\n");
    addAssumeToLoopPreheader(L, SE, AR.AC, MinIterCount.value(), Context);
  }

  LLVM_DEBUG(dbgs() << "Preheader conditional check:"
                    << cast<BranchInst>(L.getLoopPreheader()->getTerminator())
                           ->isConditional()
                    << "\n");
  if (cast<BranchInst>(L.getLoopPreheader()->getTerminator())->isConditional())
    LLVM_DEBUG(dbgs() << "Terminator Condition :";
               cast<BranchInst>(L.getLoopPreheader()->getTerminator())
                   ->getCondition()
                   ->dump();
               dbgs() << "\n");

  LLVM_DEBUG(dbgs() << "Dumping Full Function:\n";
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
  // Get the loop exiting block and condition
  BasicBlock *ExitingBlock = L.getExitingBlock();
  if (!ExitingBlock) {
    assert(false && "Failed to find the exiting block.\n");
    return nullptr;
  }

  BranchInst *BI = dyn_cast<BranchInst>(ExitingBlock->getTerminator());
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
  // Determine the induction variable and loop limit in the comparison
  Value *LimitVar = nullptr;
  if (ICmp->getOperand(0) == InductionPHI) {
    LimitVar = ICmp->getOperand(1);
  } else if (ICmp->getOperand(1) == InductionPHI) {
    LimitVar = ICmp->getOperand(0);
  } else {
    LLVM_DEBUG(
        dbgs() << "Induction variable not found in loop exit condition.\n";
        L.getHeader()->dump(); dbgs() << "\n"; BI->dump());
    return nullptr; // assert(false && "Induction variable not found in loop
                    // exit condition.\n");
  }

  return LimitVar;
}

// Function to add an assume in the loop preheader
void addAssumeToLoopPreheader(Loop &L, ScalarEvolution &SE, AssumptionCache &AC,
                              uint64_t MinIterCount, LLVMContext *Context) {
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
    S = SE.getSCEV(&PN);

    if (const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(S)) {
      if (AR->getLoop() == &L) {
        LLVM_DEBUG(dbgs() << "Found Induction Phi "; S->dump());
        InductionPHI = &PN;
        break;
      }
    }
    LLVM_DEBUG(dbgs() << "Phi "; S->dump());
  }

  Value *MinValue = calcMinValue(S, MinIterCount, Context);
  if (!MinValue) {
    return;
  }

  Value *IterVar = nullptr;
  if (isIncrement(S)) {
    IterVar = getIterationVariable(L, InductionPHI);
  } else {
    IterVar = getIterationVariable(S);
  }

  if (!IterVar) {
    LLVM_DEBUG(
        dbgs()
        << "Could not find Iteration Variable. Will not process Metadata\n");
    return;
  }

  LLVM_DEBUG(dbgs() << "Minimum Value : "; MinValue->dump());
  LLVM_DEBUG(dbgs() << "Iteration Variable : "; IterVar->dump());

  IRBuilder<> Builder(Preheader->getTerminator());
  Value *Cmp = nullptr;
  Cmp = Builder.CreateICmpSGT(IterVar, MinValue);

  LLVM_DEBUG(dbgs() << "Inserting Condition:"; MinValue->dump();
             dbgs() << "With Comparator:"; Cmp->dump());

  // Insert the `llvm.assume` Call
  Function *AssumeFn = Intrinsic::getDeclaration(M, Intrinsic::assume);
  CallInst *Call = Builder.CreateCall(AssumeFn, Cmp);
  Call->setTailCall(true);
  AC.registerAssumption(dyn_cast<AssumeInst>(Call));
}
