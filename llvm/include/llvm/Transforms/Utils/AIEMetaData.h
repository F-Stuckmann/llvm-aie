//===-- AIEMetaDataPass.h - Top-level interface for AIE -----------------*- C++
//-*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2024 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_AIEMETADATAPASS_H
#define LLVM_TRANSFORMS_UTILS_AIEMETADATAPASS_H
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

namespace llvm {

class Loop;
/// Loop unroll pass that will support both full and partial unrolling.
/// It is a function pass to have access to function and module analyses.
/// It will also put loops into canonical form (simplified and LCSSA).
class AIEMetaData : public PassInfoMixin<AIEMetaData> {
private:
  LLVMContext *Context;
  ScalarEvolution *SE;
  AssumptionCache *AC;
  DominatorTree *DT;
  const Loop *L;
  Instruction *LoopBound0;
  Instruction *LoopBound1;
  bool Increment;

  bool extractMetaData(Loop &L);

  void addAssumeToLoopHeader(uint64_t MinIterCount, LLVMContext *Context);

  Value *getMaxBoundry() const;
  bool isIncrement(const SCEV *S);
  Value *calcMinValue(const SCEV *S, int MinIterCount, LLVMContext *Context);

  bool hasAssumption(Value *Header);

  const SCEV *getTruncInductionSCEV() const;
  const SCEV *getSCEV() const;

public:
  PreservedAnalyses run(Loop &L, LoopAnalysisManager &AM,
                        LoopStandardAnalysisResults &AR, LPMUpdater &U);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_AIEMETADATAPASS_H
