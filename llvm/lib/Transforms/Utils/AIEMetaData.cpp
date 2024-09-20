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
using namespace llvm;
PreservedAnalyses AIEMetaDataPass::run(Loop &L, LoopAnalysisManager &LAM,
                                       LoopStandardAnalysisResults &AR,
                                       LPMUpdater &U) {
  errs() << "Hello World! " << L.getHeader()->getParent()->getName() << "\n";
  return PreservedAnalyses::all();
}

// #include "llvm/Analysis/LoopInfo.h"
// #include "llvm/Analysis/LoopPass.h"
// #include "llvm/Analysis/ScalarEvolution.h"
// #include "llvm/IR/IRBuilder.h"
// #include "llvm/IR/Intrinsics.h"
// #include "llvm/InitializePasses.h"
// #include "llvm/Pass.h"
// #include "llvm/Transforms/Utils.h"
// #include "llvm/Transforms/Utils/LoopUtils.h"
// // #include "llvm/Transforms/Utils/UnrollLoop.h"

// using namespace llvm;
// namespace {

// struct AIEMetaData : LoopPass {
//   static char ID; // Pass ID, replacement for typeid
//   AIEMetaData() : LoopPass(ID) {}

//   void addAssumeToLoopPreheader(Loop *L, ScalarEvolution *SE,
//                                 uint64_t MinIterCount);

//   bool runOnLoop(Loop *L, LPPassManager &LPM);
// };

// } // end anonymous namespace

// char AIEMetaData::ID = 0;
// static RegisterPass<AIEMetaData>
//     X("aie-metadata", "Convert AIE specific Metadata to assumptions.");

// #define DEBUG_TYPE "aie-metadata"

// bool AIEMetaData::runOnLoop(Loop *L, LPPassManager &LPM) {
//   ScalarEvolution &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();

//   std::optional<int> MinIterCount = getMinIterCounts(L);

//   if (MinIterCount.has_value()) {
//     addAssumeToLoopPreheader(L, &SE, MinIterCount.value());
//     return true; // Indicate that the IR was modified
//   }

//   return false; // No changes made
// }

// // Function to add an assume in the loop preheader
// void AIEMetaData::addAssumeToLoopPreheader(Loop *L, ScalarEvolution *SE,
//                                            uint64_t MinIterCount) {
//   // Retrieve the Loop Preheader
//   BasicBlock *Preheader = L->getLoopPreheader();
//   if (!Preheader) {
//     errs() << "Loop does not have a preheader.\n";
//     return;
//   }

//   Module *M = Preheader->getModule();

//   // Identify the Loop Induction Variable and Limit
//   PHINode *InductionPHI = nullptr;
//   Value *LimitVar = nullptr;
//   ICmpInst::Predicate Pred;

//   // Find the canonical induction variable
//   for (PHINode &PN : L->getHeader()->phis()) {
//     const SCEV *S = SE->getSCEV(&PN);
//     if (const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(S)) {
//       if (AR->getLoop() == L) {
//         InductionPHI = &PN;
//         break;
//       }
//     }
//   }

//   if (!InductionPHI) {
//     errs() << "Failed to find the induction variable.\n";
//     return;
//   }

//   // Get the loop exiting block and condition
//   BasicBlock *ExitingBlock = L->getExitingBlock();
//   if (!ExitingBlock) {
//     assert(false && "Failed to find the exiting block.\n");
//     return;
//   }

//   BranchInst *BI = dyn_cast<BranchInst>(ExitingBlock->getTerminator());
//   if (!BI || !BI->isConditional()) {
//     assert(false && "Exiting block does not have a conditional branch.\n");
//   }

//   Value *Condition = BI->getCondition();
//   ICmpInst *ICmp = dyn_cast<ICmpInst>(Condition);
//   if (!ICmp) {
//     assert(false && "Loop exit condition is not an integer comparison.\n");
//   }

//   // Determine the induction variable and loop limit in the comparison
//   if (ICmp->getOperand(0) == InductionPHI) {
//     LimitVar = ICmp->getOperand(1);
//     Pred = ICmp->getPredicate();
//   } else if (ICmp->getOperand(1) == InductionPHI) {
//     LimitVar = ICmp->getOperand(0);
//     Pred = ICmp->getSwappedPredicate();
//   } else {
//     assert(false && "Induction variable not found in loop exit
//     condition.\n");
//   }

//   // Create the Assumption
//   // Get the starting value of the induction variable from the preheader
//   Value *InductionStart = InductionPHI->getIncomingValueForBlock(Preheader);
//   if (!InductionStart) {
//     assert(false && "Failed to get induction start value.\n");
//   }

//   IRBuilder<> Builder(Preheader->getTerminator());

//   // Ensure types match
//   if (InductionStart->getType() != LimitVar->getType()) {
//     InductionStart =
//         Builder.CreateSExtOrTrunc(InductionStart, LimitVar->getType());
//   }

//   // Calculate InductionStart + MinIterCount
//   ConstantInt *MinIterConst = dyn_cast<ConstantInt>(
//       ConstantInt::get(LimitVar->getType(), MinIterCount));
//   Value *MinValue = Builder.CreateAdd(InductionStart, MinIterConst);

//   // Depending on the predicate, adjust the comparison
//   Value *Cmp = nullptr;
//   switch (Pred) {
//   case ICmpInst::ICMP_SLT:
//   case ICmpInst::ICMP_ULT:
//     // Loop condition is InductionPHI < LimitVar
//     // So we assume LimitVar >= InductionStart + MinIterCount
//     Cmp = Builder.CreateICmpSGE(LimitVar, MinValue);
//     break;
//   case ICmpInst::ICMP_SGT:
//   case ICmpInst::ICMP_UGT:
//     // Loop condition is InductionPHI > LimitVar
//     // So we assume LimitVar <= InductionStart - MinIterCount
//     MinValue = Builder.CreateSub(InductionStart, MinIterConst);
//     Cmp = Builder.CreateICmpSLE(LimitVar, MinValue);
//     break;
//   case ICmpInst::ICMP_SLE:
//   case ICmpInst::ICMP_ULE:
//     // Loop condition is InductionPHI <= LimitVar
//     // So we assume LimitVar >= InductionStart + MinIterCount - 1
//     MinValue =
//         Builder.CreateSub(MinValue, ConstantInt::get(LimitVar->getType(),
//         1));
//     Cmp = Builder.CreateICmpSGE(LimitVar, MinValue);
//     break;
//   case ICmpInst::ICMP_SGE:
//   case ICmpInst::ICMP_UGE:
//     // Loop condition is InductionPHI >= LimitVar
//     // So we assume LimitVar <= InductionStart - MinIterCount + 1
//     MinValue =
//         Builder.CreateAdd(MinValue, ConstantInt::get(LimitVar->getType(),
//         -1));
//     Cmp = Builder.CreateICmpSLE(LimitVar, MinValue);
//     break;
//   default:
//     assert(false && "Unsupported loop predicate.\n");
//   }

//   // Insert the `llvm.assume` Call
//   Function *AssumeFn = Intrinsic::getDeclaration(M, Intrinsic::assume);
//   Builder.CreateCall(AssumeFn, Cmp);
// }
