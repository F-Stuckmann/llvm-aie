//===-- AIERegAllocationUtils.h - AIE register allocation utils -*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AIE_UTILS_AIEREGALLOCATIONUTILS_H
#define LLVM_LIB_TARGET_AIE_UTILS_AIEREGALLOCATIONUTILS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/MC/MCRegister.h"

namespace llvm {

class LiveInterval;
class LiveRegMatrix;
class TargetRegisterClass;
class TargetRegisterInfo;

namespace AIERegAllocationUtils {

MCPhysReg findFreeNonOverlappingPhysReg(const LiveInterval &LI,
                                        const TargetRegisterClass &RC,
                                        ArrayRef<MCPhysReg> AllocationOrder,
                                        MCPhysReg AvoidPhysReg,
                                        const BitVector &ReservedRegUnits,
                                        const TargetRegisterInfo &TRI,
                                        LiveRegMatrix &LRM);

} // namespace AIERegAllocationUtils
} // namespace llvm

#endif
