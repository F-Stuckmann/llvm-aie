//===-- example_no_builtin.cpp - Basic iteration count example----*- C++-*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2024 Advanced Micro Devices, Inc. or its affiliates
//

inline void nestedLoop(int *ptr, int n) {
  for (int i = 0; i < n; i++) {
    ptr[i] = ptr[i] + 12;
  }
}

void itercounter(int *ptr, int n) {

#pragma clang loop unroll(disable)
#pragma clang loop min_iteration_count(4)
  for (int i = 0; i < n; i++) {
    // nestedLoop(ptr, i);
    ptr[i] = ptr[i] + 8;
  }
}
