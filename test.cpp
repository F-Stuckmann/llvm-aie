//===-- example.cpp - Basic iteration count example -----------------*- C++
//-*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2024 Advanced Micro Devices, Inc. or its affiliates
//

// // min_iteration_count will update to 4, when unrolled by 4
// void unroll(int *ptr, int n) {
// __builtin_assume(n >= 4);
// #pragma clang loop min_iteration_count(4)
//   for (int i = 0; i < n; i++) {
//     ptr[i] = ptr[i] + 8;
//   }
// }

////////////////////////////// increment
// void increment(int *ptr, int n) {
//   __builtin_assume(n >= 4);
// #pragma clang loop unroll(disable)
// #pragma clang loop min_iteration_count(4)
//   for (int i = 0; i < n; i++) {
//     ptr[i] = ptr[i] + 8;
//   }
// }

// void increment_multiple(int *ptr, int n) {
//   __builtin_assume(n >= 28);
// #pragma clang loop unroll(disable)
// #pragma clang loop min_iteration_count(4)
//   for (int i = 0; i < n; i += 7) {
//     ptr[i] = ptr[i] + 8;
//   }
// }

// // void increment_variable(int *ptr, int n, int m) {
// //   __builtin_assume(n >= 4);
// // #pragma clang loop unroll(disable)
// // #pragma clang loop min_iteration_count(4)
// //   for (int i = 0; i < n; i += m) {
// //     ptr[i] = ptr[i] + 8;
// //   }
// // }

// void conditionalLoop(int *ptr, int n) {
//   if (n > 0) {
//     __builtin_assume(n >= 4);
// #pragma clang loop unroll(disable)
// #pragma clang loop min_iteration_count(4)
//     for (int i = 0; i < n; i++) {
//       // nestedLoop(ptr, i);
//       ptr[i] = ptr[i] + 8;
//     }
//   }
// }

// void nestedLoop(int *ptr, int n, int m) {
//   for (int i = 0; i < n; i++) {
//     __builtin_assume(m >= 4);
// #pragma clang loop unroll(disable)
// #pragma clang loop min_iteration_count(4)
//     for (int j = 0; j < m; j++) {
//       ptr[i] = ptr[j] + 8;
//     }
//   }
// }

// void nestedpartialLoop(int *ptr, int n, int m, int o) {
//   for (int i = 0; i < n; i++) {
//     __builtin_assume(m >= 4);
// #pragma clang loop unroll(disable)
// #pragma clang loop min_iteration_count(4)
//     for (int j = 0; j < m; j++) {
//       ptr[i] = ptr[j] + 8;
//     }
//     for (int k = 0; k < o; k++) {
//       ptr[k] = ptr[i] + 8;
//     }
//   }
// }

// void deepnestedouterLoop(int *ptr, int n, int m, int o, int p) {
//   for (int i = 0; i < n; i++) {
//     __builtin_assume(m >= 4);
// #pragma clang loop unroll(disable)
// #pragma clang loop min_iteration_count(4)
//     for (int j = 0; j < m; j++) {
//       for (int k = 0; k < p; k++) {
//         ptr[k + i] = ptr[j] + 8;
//       }
//     }
//     for (int k = 0; k < o; k++) {
//       ptr[k] = ptr[i] + 8;
//     }
//   }
// }

// void innerDeepNestedLoop(int *ptr, int n, int m, int o, int p) {
//   for (int i = 0; i < n; i++) {
//     for (int j = 0; j < m; j++) {
//       __builtin_assume(p >= 4);
// #pragma clang loop unroll(disable)
// #pragma clang loop min_iteration_count(4)
//       for (int k = 0; k < p; k++) {
//         ptr[k + i] = ptr[j] + 8;
//       }
//     }
//     for (int k = 0; k < o; k++) {
//       ptr[k] = ptr[i] + 8;
//     }
//   }
// }

//////////////////////// Decrement
// void decrement(int *ptr, int n) {
//   __builtin_assume(n >= 4);
// #pragma clang loop unroll(disable)
// #pragma clang loop min_iteration_count(4)
//   for (int i = n; i > 0; i--) {
//     ptr[i] = ptr[i] + 8;
//   }
// }

// void decrement_multiple(int *ptr, int n) {
//   __builtin_assume(n >= 28);
// #pragma clang loop unroll(disable)
// #pragma clang loop min_iteration_count(4)
//   for (int i = n; i > 0; i -= 7) {
//     ptr[i] = ptr[i] + 8;
//   }
// }

//////////////////////////// start position
void increment_start_pos(int *ptr, int n, int m) {
  __builtin_assume(n >= 4);
#pragma clang loop unroll(disable)
#pragma clang loop min_iteration_count(2)
  for (int i = m; i < n; i++) {
    ptr[i] = ptr[i] + 8;
  }
}

/////////////////////////// other loop
