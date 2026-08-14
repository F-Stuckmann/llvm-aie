//===-- AIERegAllocationUtils.cpp - AIE register allocation utils --------===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//

#include "AIERegAllocationUtils.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

using namespace llvm;

MCPhysReg AIERegAllocationUtils::findFreeNonOverlappingPhysReg(
    const LiveInterval &LI, const TargetRegisterClass &RC,
    ArrayRef<MCPhysReg> AllocationOrder, MCPhysReg AvoidPhysReg,
    const BitVector &ReservedRegUnits, const TargetRegisterInfo &TRI,
    LiveRegMatrix &LRM) {
  for (MCPhysReg PhysReg : AllocationOrder) {
    if (!RC.contains(PhysReg))
      continue;

    if (AvoidPhysReg && TRI.regsOverlap(PhysReg, AvoidPhysReg))
      continue;

    const bool HasReservedUnit =
        llvm::any_of(TRI.regunits(PhysReg), [&](MCRegUnit Unit) {
          return !ReservedRegUnits.empty() && ReservedRegUnits.test(Unit);
        });
    if (HasReservedUnit)
      continue;

    if (LRM.checkInterference(LI, PhysReg) == LiveRegMatrix::IK_Free)
      return PhysReg;
  }

  return MCRegister::NoRegister;
}
