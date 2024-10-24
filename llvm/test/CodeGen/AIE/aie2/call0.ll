; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
;
; This file is licensed under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; (c) Copyright 2023-2024 Advanced Micro Devices, Inc. or its affiliates
; RUN: llc -mtriple=aie2 --issue-limit=1 -global-isel -verify-machineinstrs -o - < %s | FileCheck %s

; stack frame just for the link register

define void @test(i32 %x) {
; CHECK-LABEL: test:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    nopb ; nopa ; nops ; jl #f; nopv
; CHECK-NEXT:    nopx // Delay Slot 5
; CHECK-NEXT:    paddb [sp], #32 // Delay Slot 4
; CHECK-NEXT:    st lr, [sp, #-32] // 4-byte Folded Spill Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
; CHECK-NEXT:    lda lr, [sp, #-32] // 4-byte Folded Reload
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    nop // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    paddb [sp], #-32 // Delay Slot 1

entry:
  call void @f()
  ret void
}
declare void @f()
