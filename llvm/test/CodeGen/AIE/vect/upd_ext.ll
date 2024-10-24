; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
;
; This file is licensed under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; (c) Copyright 2023-2024 Advanced Micro Devices, Inc. or its affiliates
; RUN: llc < %s -verify-machineinstrs -mtriple=aie | FileCheck %s
define noundef <8 x i32> @_Z8upd_testv() {
; CHECK-LABEL: _Z8upd_testv:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    padda [sp], #64
; CHECK-NEXT:    nop
; CHECK-NEXT:    vlda wr0, [sp, #-64]
; CHECK-NEXT:    vlda vh0, [sp, #-32]
; CHECK-NEXT:    padda [sp], #-64
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
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %sbuff = alloca <8 x i32>, align 32
  %val = alloca <4 x i32>, align 16
  %sbuff.0.sbuff.0.sbuff.0. = load volatile <8 x i32>, ptr %sbuff, align 32
  %val.0.val.0.val.0. = load volatile <4 x i32>, ptr %val, align 16
  %0 = tail call <8 x i32> @llvm.aie.upd.v.v8i32.hi(<8 x i32> %sbuff.0.sbuff.0.sbuff.0., <4 x i32> %val.0.val.0.val.0.)
  ret <8 x i32> %0
}
define noundef <16 x i16> @_Z10ext_w_testv()  {
; CHECK-LABEL: _Z10ext_w_testv:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    padda [sp], #64
; CHECK-NEXT:    nop
; CHECK-NEXT:    vst.spil wr0, [sp, #-64]
; CHECK-NEXT:    vst.spil wr1, [sp, #-32]
; CHECK-NEXT:    vlda.spil wr0, [sp, #-64]
; CHECK-NEXT:    vlda.spil wr1, [sp, #-32]
; CHECK-NEXT:    // kill: def $wr0 killed $wr0 killed $xa
; CHECK-NEXT:    padda [sp], #-64
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    nop // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %sbuff = alloca <32 x i16>, align 64
  %0 = tail call <32 x i16> @llvm.aie.v32i16undef()
  store volatile <32 x i16> %0, ptr %sbuff, align 64
  %sbuff.0.sbuff.0.sbuff.0. = load volatile <32 x i16>, ptr %sbuff, align 64
  %1 = tail call <16 x i16> @llvm.aie.ext.w.v32i16.lo(<32 x i16> %sbuff.0.sbuff.0.sbuff.0.)
  ret <16 x i16> %1
}
declare <32 x i16> @llvm.aie.v32i16undef()
declare <16 x i16> @llvm.aie.ext.w.v32i16.lo(<32 x i16>)
declare <8 x i32> @llvm.aie.upd.v.v8i32.hi(<8 x i32>, <4 x i32>)
