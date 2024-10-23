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

#define DEBUG_TYPE "aie-metadata"

using namespace llvm;

bool AIEMetaData::hasAssumption(Value *Header) {
  for (auto A : AC->assumptions()) {
    if (A.Assume && isa<Instruction>(A.Assume)) {
      Instruction *I = dyn_cast<Instruction>(A.Assume);
      if (I->getParent() == Header) {
        return true;
      }
    }
  }
  return false;
}

PreservedAnalyses AIEMetaData::run(Loop &L, LoopAnalysisManager &AM,
                                   LoopStandardAnalysisResults &AR,
                                   LPMUpdater &U) {
  SE = &AR.SE;
  AC = &AR.AC;
  DT = &AR.DT;
  extractMetaData(L);
  return PreservedAnalyses::all();
}

// check loop rotation conditions from
bool isRotatable(const Loop *L) {
  BranchInst *BI = dyn_cast<BranchInst>(L->getHeader()->getTerminator());
  return L->isLoopExiting(L->getHeader()) && (BI && BI->isConditional());
}

bool AIEMetaData::extractMetaData(Loop &L) {
  this->L = &L;
  Context = &L.getHeader()->getParent()->getContext();

  std::optional<int> MinIterCount = getMinIterCounts(&L);

  // dump loop summary
  LLVM_DEBUG(dbgs() << "Preheader:");
  if (L.getLoopPreheader())
    LLVM_DEBUG(dbgs() << L.getLoopPreheader()->getName());
  LLVM_DEBUG(dbgs() << "\nHeader:");
  if (L.getHeader())
    LLVM_DEBUG(dbgs() << L.getHeader()->getName());
  LLVM_DEBUG(dbgs() << "\n");

  if (MinIterCount.has_value() && MinIterCount.value() > 0) {

    if (!isRotatable(this->L)) {
      LLVM_DEBUG(dbgs() << "Processing Loop Metadata: "
                        << L.getHeader()->getParent()->getName() << " "
                        << L.getName() << " (" << MinIterCount.value()
                        << ")\nAborting Metadata due to not rotatable!\n");
      return false;
    }

    LLVM_DEBUG(L.getHeader()->getParent()->dump(););

    addAssumeToLoopHeader(MinIterCount.value(), Context);
    LLVM_DEBUG(dbgs() << "Dumping Full Function:"
                      << L.getHeader()->getParent()->getName() << "\n";
               L.getHeader()->getParent()->dump(););
    return true;
  }

  return false;
}

bool AIEMetaData::isIncrement(const SCEV *S) {
  switch (S->getSCEVType()) {
  case scAddRecExpr: {
    const SCEVAddRecExpr *AR = cast<SCEVAddRecExpr>(S);
    assert(AR->getNumOperands() == 2 &&
           "Unknown Handling of more than 2 Operands");
    Increment =
        cast<SCEVConstant>(*AR->getOperand(1)).getValue()->getSExtValue() > 0;
    return Increment;
    break;
  }

  default:
    assert(false && "Could not extract Iteration Variable from ");
  }
  Increment = false;
  return Increment;
}

// Value *AIEMetaData::getMinBound()

Value *AIEMetaData::calcMinLoopIter(const SCEV *S, int MinIterCount,
                                    LLVMContext *Context) {
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

  int MaxValue = std::abs(IncValue * MinIterCount);
  // SGT is used to compare, so I must subtract 1
  // FIXME: only decrement if the condition is lt (on EQ dont do this)
  if (Increment)
    MaxValue--;

  llvm::ConstantInt *ConstIncValue =
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Context), MaxValue, true);
  return static_cast<llvm::Value *>(ConstIncValue);
}

Value *getMaxBoundryDec(const SCEV *S) {
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

Value *AIEMetaData::getValue(Value *V) {
  if (PHINode *MaxPHI = dyn_cast_or_null<PHINode>(V)) {
    bool HasSecondOp = MaxPHI->getNumOperands() > 1;
    // Is a function argument, no need to check domination relationship
    if (!isa<Instruction>(MaxPHI->getOperand(0))) {
      return MaxPHI->getOperand(0);
    }
    // Is a function argument, no need to check domination relationship
    if (HasSecondOp && !isa<Instruction>(MaxPHI->getOperand(1))) {
      return MaxPHI->getOperand(1);
    }
    // Check if the Op0 is from a previous block, then this is the correct value
    if (DT->dominates(dyn_cast<Instruction>(MaxPHI->getOperand(0))->getParent(),
                      L->getHeader())) {
      return MaxPHI->getOperand(0);
    }
    if (HasSecondOp &&
        DT->dominates(dyn_cast<Instruction>(MaxPHI->getOperand(1))->getParent(),
                      L->getHeader())) {
      return MaxPHI->getOperand(1);
    }
    return nullptr;
  }
  return V;
}

Value *AIEMetaData::getBoundries() {

  BranchInst *BI = dyn_cast<BranchInst>(L->getExitingBlock()->getTerminator());
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

  CmpInst::Predicate Pred = ICmp->getPredicate();
  Value *Op0 = ICmp->getOperand(0);
  Value *Op1 = ICmp->getOperand(1);
  if (ICmpInst::isLT(Pred)) {
    MinValue = getValue(Op0);
    MaxBoundry = getValue(Op1);
  } else if (Pred == CmpInst::Predicate::ICMP_EQ) {
    // assume the upper loop bound does not change, so that the max bound has no
    // SCEV.
    if (SE->isSCEVable(Op0->getType()) && SE->getExistingSCEV(Op0) &&
        SE->isSCEVable(Op1->getType()) && SE->getExistingSCEV(Op1)) {
      MinValue = nullptr;
      MaxBoundry = nullptr;
    }
    if (SE->isSCEVable(Op0->getType()) && SE->getExistingSCEV(Op0)) {
      MinValue = getValue(Op0);
      MaxBoundry = getValue(Op1);
    } else {
      MinValue = getValue(Op1);
      MaxBoundry = getValue(Op0);
    }

  } else {
    MinValue = getValue(Op0);
    MaxBoundry = getValue(Op1);
  }
  LLVM_DEBUG(dbgs() << "MinValue = "; MinValue->dump());
  LLVM_DEBUG(dbgs() << "MaxValue = "; MaxBoundry->dump());

  return MaxBoundry;
}

const SCEV *AIEMetaData::getSCEV() const {
  if (SE->isSCEVable(LoopBound0->getType())) {
    const SCEV *S = SE->getSCEV(LoopBound0);
    if (S && S->getSCEVType() == SCEVTypes::scAddRecExpr)
      return S;
  }
  if (LoopBound1 && SE->isSCEVable(LoopBound1->getType())) {
    const SCEV *S = SE->getSCEV(LoopBound1);
    if (S && S->getSCEVType() == SCEVTypes::scAddRecExpr)
      return S;
  }
  // fixme: if both are SCEVTypes::scAddRecExpr, exit with warning, since unkown
  // behaviour
  return nullptr;
}

bool fitstype(int MinItercount, const Type *T) {
  return (uint64_t)MinItercount <
         *APInt::getSignedMaxValue(T->getIntegerBitWidth()).getRawData();
}

const SCEV *AIEMetaData::getTruncInductionSCEV(int MinIterCount) const {
  const SCEV *S = nullptr;

  for (BasicBlock *BB : L->blocks()) {
    for (BasicBlock::iterator IT = BB->begin(), IE = BB->end(); IT != IE;
         ++IT) {
      Instruction *I = &(*IT);
      if (!SE->isSCEVable(I->getType()))
        continue;
      LLVM_DEBUG(I->dump());

      // remove all instructions that are not relevant for branch decision
      bool ValidInstruction = false;
      for (uint Index = 0; Index < I->getNumOperands(); Index++) {
        if (I->getOperand(Index) == LoopBound0 ||
            I->getOperand(Index) == LoopBound1)
          ValidInstruction = true;
      }
      if (!ValidInstruction)
        continue;

      const SCEV *SIntern = SE->getSCEV(I);
      LLVM_DEBUG(dbgs() << "SCEV "; SIntern->dump());
      LLVM_DEBUG(dbgs() << SIntern->getSCEVType() << "\n");

      if (const SCEVAddExpr *AR = dyn_cast<SCEVAddExpr>(SIntern)) {
        if (const SCEVZeroExtendExpr *Zext =
                dyn_cast<SCEVZeroExtendExpr>(AR->getOperand(1))) {
          const SCEVTruncateExpr *Trunc =
              dyn_cast<SCEVTruncateExpr>(Zext->getOperand(0));
          if (Trunc && fitstype(MinIterCount, Trunc->getType())) {
            const SCEV *OrigSCEV = Trunc->getOperand();

            if (const SCEVConstant *SCEVConst =
                    dyn_cast<SCEVConstant>(AR->getOperand(0))) {
              const SCEV *Step =
                  SE->getConstant(OrigSCEV->getType(),
                                  SCEVConst->getValue()->getSExtValue(), true);

              // extract start point from phi of the SCEV
              const SCEV *Start = nullptr;
              Value *StartVal = dyn_cast<SCEVUnknown>(OrigSCEV)->getValue();
              if (StartVal) {
                PHINode *PN =
                    dyn_cast<PHINode>(dyn_cast<Instruction>(StartVal));
                if (PN) {
                  for (uint Op = 0; Op < PN->getNumOperands(); Op++) {
                    if (PN->getIncomingBlock(Op) == L->getLoopPreheader()) {
                      Value *V = PN->getOperand(Op);
                      if (isa<Constant>(V)) {
                        Start = SE->getSCEV(V);
                      }
                    }
                  }
                }
              }

              if (!Start)
                continue;

              S = SE->getAddRecExpr(
                  Start, Step, L, llvm::SCEVAddExpr::NoWrapFlags::FlagAnyWrap);
              LLVM_DEBUG(dbgs() << "Found SCEV "; S->dump());
              return S;
            }
          }
        }
      }
    }
  }
  return S;
}

// Function to add an assume in the loop Header
void AIEMetaData::addAssumeToLoopHeader(uint64_t MinIterCount,
                                        LLVMContext *Context) {
  LLVM_DEBUG(dbgs() << "Processing Loop Metadata: "
                    << L->getHeader()->getParent()->getName() << " "
                    << L->getName() << " (" << MinIterCount << ")\n");

  ICmpInst *CombInstr = dyn_cast<ICmpInst>(
      dyn_cast<BranchInst>(L->getExitingBlock()->getTerminator())
          ->getCondition());
  LoopBound0 = dyn_cast<Instruction>(CombInstr->getOperand(0));
  LoopBound1 = dyn_cast<Instruction>(CombInstr->getOperand(1));
  LLVM_DEBUG(dbgs() << "Compare Instructions \nOperand0"; LoopBound0->dump());
  if (LoopBound1)
    LLVM_DEBUG(dbgs() << " Operand1"; LoopBound1->dump());

  // Find the canonical induction variable
  const SCEV *S = getSCEV();

  if (!S) {
    S = getTruncInductionSCEV(MinIterCount);
  }

  if (!S) {
    LLVM_DEBUG(dbgs() << "AIEMetadata-Warning: Could not extract "
                         "SCEVAddRecExpr! Will not process Metadata\n");
    return;
  }
  isIncrement(S);
  Value *MinIterValue = calcMinLoopIter(S, MinIterCount, Context);
  if (!MinIterValue) {
    return;
  }

  getBoundries();

  if (!MaxBoundry) {
    LLVM_DEBUG(dbgs() << "AIEMetadata-Warning: Could not find Iteration "
                         "Variable. Will not process Metadata\n");
    return;
  }
  if (isa<Instruction>(MaxBoundry)) {
    BasicBlock *MaxBB = dyn_cast<Instruction>(MaxBoundry)->getParent();
    if (MaxBB && !DT->dominates(MaxBB, L->getHeader())) {
      LLVM_DEBUG(dbgs() << "AIEMetadata-Warning: MaxBoundry is not in the same "
                           "BB as the Header ("
                        << L->getHeader()->getName() << ")\nMaxBoundry =";
                 MaxBoundry->dump(););
      if (MaxBB)
        LLVM_DEBUG(dbgs() << "MaxBoundry BB = " << MaxBB->getName() << "\n";);
      return;
    }
  }

  if (isa<Constant>(MaxBoundry)) {
    LLVM_DEBUG(dbgs() << "Iteration Variable (Max value) is an integer and "
                         "therefore no assumption "
                         "has to be added!");
    return;
  }

  LLVM_DEBUG(dbgs() << "Min IterationVAlue : "; MinIterValue->dump());
  LLVM_DEBUG(dbgs() << "Max Value          : "; MaxBoundry->dump());

  IRBuilder<> Builder(L->getHeader()->getTerminator());

  Value *Cmp = nullptr;
  // ensure equalize types in the comparison
  if (MaxBoundry->getType() != MinIterValue->getType()) {
    if (MinIterValue->getType()->getScalarSizeInBits() <
        MaxBoundry->getType()->getScalarSizeInBits()) {
      MinIterValue = Builder.CreateSExt(MinIterValue, MaxBoundry->getType());
    } else {
      MaxBoundry = Builder.CreateSExt(MaxBoundry, MinIterValue->getType());
    }
  }
  Cmp = Builder.CreateICmpSGT(MaxBoundry, MinIterValue);

  LLVM_DEBUG(dbgs() << "Inserting Condition:"; MinIterValue->dump();
             dbgs() << "With Comparator:"; Cmp->dump());

  // Insert the `llvm.assume` Call
  Function *AssumeFn =
      Intrinsic::getDeclaration(L->getHeader()->getModule(), Intrinsic::assume);
  CallInst *Call = Builder.CreateCall(AssumeFn, Cmp);
  Call->setTailCall(true);
  AC->registerAssumption(dyn_cast<AssumeInst>(Call));
}
