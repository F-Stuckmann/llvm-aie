; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
;
; This file is licensed under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; (c) Copyright 2023-2024 Advanced Micro Devices, Inc. or its affiliates
; RUN: llc -mtriple=aie2 --issue-limit=1 -verify-machineinstrs -o - < %s \
; RUN:   | FileCheck %s
define  <16 x i64> @_Z21test_mac_4x2_2x4_confiiiiii(i32 noundef %sgn_x, i32 noundef %sgn_y, i32 noundef %zero_acc1, i32 noundef %shift16, i32 noundef %sub_mul, i32 noundef %sub_acc1)  {
; CHECK-LABEL: _Z21test_mac_4x2_2x4_confiiiiii:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    nopb ; mova r6, #10; nops ; nopxm ; nopv
; CHECK-NEXT:    mova r7, #11
; CHECK-NEXT:    mova r8, #12
; CHECK-NEXT:    mova r9, #9
; CHECK-NEXT:    mova r10, #8
; CHECK-NEXT:    mova r11, #2
; CHECK-NEXT:    lshl r3, r3, r6
; CHECK-NEXT:    lshl r4, r4, r7
; CHECK-NEXT:    lshl r5, r5, r8
; CHECK-NEXT:    lshl r0, r0, r9
; CHECK-NEXT:    lshl r1, r1, r10
; CHECK-NEXT:    or r0, r0, r1
; CHECK-NEXT:    or r0, r0, r2
; CHECK-NEXT:    or r0, r0, r3
; CHECK-NEXT:    or r0, r0, r4
; CHECK-NEXT:    or r0, r0, r5
; CHECK-NEXT:    or r0, r0, r11
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vmac cm0, cm0, x0, x0, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %0 = tail call <16 x i32> @llvm.aie2.v16int32()
  %1 = tail call <32 x i16> @llvm.aie2.v32int16()
  %2 = tail call <16 x i64> @llvm.aie2.v16acc64()
  %shl1.i.i = shl i32 %shift16, 10
  %shl2.i.i = shl i32 %sub_mul, 11
  %shl4.i.i = shl i32 %sub_acc1, 12
  %shl14.i.i = shl i32 %sgn_x, 9
  %shl15.i.i = shl i32 %sgn_y, 8
  %or3.i.i = or i32 %shl14.i.i, %shl15.i.i
  %or5.i.i = or i32 %or3.i.i, %zero_acc1
  %or13.i.i = or i32 %or5.i.i, %shl1.i.i
  %or16.i.i = or i32 %or13.i.i, %shl2.i.i
  %or17.i.i = or i32 %or16.i.i, %shl4.i.i
  %or19.i.i = or i32 %or17.i.i, 2
  %3 = bitcast <16 x i32> %0 to <64 x i8>
  %4 = bitcast <32 x i16> %1 to <16 x i32>
  %5 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.mac.conf(<64 x i8> %3, <16 x i32> %4, <16 x i64> %2, i32 %or19.i.i)
  ret <16 x i64> %5
}


define  <16 x i64> @_Z16test_mac_4x2_2x8Dv32_tS_Dv16_u7__acc64(<32 x i16> noundef %a, <32 x i16> noundef %b, <16 x i64> %acc1)  {
; CHECK-LABEL: _Z16test_mac_4x2_2x8Dv32_tS_Dv16_u7__acc64:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    nopb ; mova r0, #24; nops ; nopxm ; nopv
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vmac cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %0 = bitcast <32 x i16> %a to <64 x i8>
  %1 = bitcast <32 x i16> %b to <16 x i32>
  %2 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.mac.conf(<64 x i8> %0, <16 x i32> %1, <16 x i64> %acc1, i32 24)
  ret <16 x i64> %2
}


define  <16 x i64> @_Z21test_negmac_4x16_16x8Dv64_hDv16_jDv16_u7__acc64(<64 x i8> noundef %a, <16 x i32> noundef %b, <16 x i64> %acc1)  {
; CHECK-LABEL: _Z21test_negmac_4x16_16x8Dv64_hDv16_jDv16_u7__acc64:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    nopb ; mova r0, #0; nops ; nopxm ; nopv
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vnegmac cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %0 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.negmac.conf(<64 x i8> %a, <16 x i32> %b, <16 x i64> %acc1, i32 0)
  ret <16 x i64> %0
}


define  <16 x i64> @_Z21test_negmac_4x16_16x8Dv64_hiDv16_jiDv16_u7__acc64(<64 x i8> noundef %a, i32 noundef %sgn_x, <16 x i32> noundef %b, i32 noundef %sgn_y, <16 x i64> %acc1)  {
; CHECK-LABEL: _Z21test_negmac_4x16_16x8Dv64_hiDv16_jiDv16_u7__acc64:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    nopb ; mova r2, #9; nops ; nopxm ; nopv
; CHECK-NEXT:    mova r3, #8
; CHECK-NEXT:    lshl r0, r0, r2
; CHECK-NEXT:    lshl r1, r1, r3
; CHECK-NEXT:    or r0, r1, r0
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vnegmac cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %shl14.i.i = shl i32 %sgn_x, 9
  %shl15.i.i = shl i32 %sgn_y, 8
  %or3.i.i = or i32 %shl15.i.i, %shl14.i.i
  %0 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.negmac.conf(<64 x i8> %a, <16 x i32> %b, <16 x i64> %acc1, i32 %or3.i.i)
  ret <16 x i64> %0
}


define  <16 x i64> @_Z24test_negmac_4x8_8x4_confDv32_sDv64_aDv16_u7__acc64iii(<32 x i16> noundef %a, <64 x i8> noundef %b, <16 x i64> %acc1, i32 noundef %zero_acc1, i32 noundef %sub_mul, i32 noundef %sub_acc1)  {
; CHECK-LABEL: _Z24test_negmac_4x8_8x4_confDv32_sDv64_aDv16_u7__acc64iii:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    mova r3, #11
; CHECK-NEXT:    mova r4, #12
; CHECK-NEXT:    mova r5, #818
; CHECK-NEXT:    lshl r1, r1, r3
; CHECK-NEXT:    lshl r2, r2, r4
; CHECK-NEXT:    or r0, r1, r0
; CHECK-NEXT:    or r0, r0, r2
; CHECK-NEXT:    or r0, r0, r5
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vnegmac cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %shl2.i.i = shl i32 %sub_mul, 11
  %shl4.i.i = shl i32 %sub_acc1, 12
  %or13.i.i = or i32 %shl2.i.i, %zero_acc1
  %or17.i.i = or i32 %or13.i.i, %shl4.i.i
  %or19.i.i = or i32 %or17.i.i, 818
  %0 = bitcast <32 x i16> %a to <64 x i8>
  %1 = bitcast <64 x i8> %b to <16 x i32>
  %2 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.negmac.conf(<64 x i8> %0, <16 x i32> %1, <16 x i64> %acc1, i32 %or19.i.i)
  ret <16 x i64> %2
}


define  <16 x i64> @_Z19test_negmac_2x4_4x8Dv32_tiDv32_siDv16_u7__acc64(<32 x i16> noundef %a, i32 noundef %sgn_x, <32 x i16> noundef %b, i32 noundef %sgn_y, <16 x i64> %acc1)  {
; CHECK-LABEL: _Z19test_negmac_2x4_4x8Dv32_tiDv32_siDv16_u7__acc64:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    mova r2, #9; nopb ; nopx
; CHECK-NEXT:    mova r3, #8
; CHECK-NEXT:    mova r4, #26
; CHECK-NEXT:    lshl r0, r0, r2
; CHECK-NEXT:    lshl r1, r1, r3
; CHECK-NEXT:    or r0, r0, r1
; CHECK-NEXT:    or r0, r0, r4
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vnegmac cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %shl14.i.i = shl i32 %sgn_x, 9
  %shl15.i.i = shl i32 %sgn_y, 8
  %or3.i.i = or i32 %shl14.i.i, %shl15.i.i
  %or9.i.i = or i32 %or3.i.i, 26
  %0 = bitcast <32 x i16> %a to <64 x i8>
  %1 = bitcast <32 x i16> %b to <16 x i32>
  %2 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.negmac.conf(<64 x i8> %0, <16 x i32> %1, <16 x i64> %acc1, i32 %or9.i.i)
  ret <16 x i64> %2
}


define  <16 x i64> @_Z18test_msc_4x16_16x8Dv64_hDv16_jDv16_u7__acc64(<64 x i8> noundef %a, <16 x i32> noundef %b, <16 x i64> %acc1)  {
; CHECK-LABEL: _Z18test_msc_4x16_16x8Dv64_hDv16_jDv16_u7__acc64:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    nopb ; mova r0, #0; nops ; nopxm ; nopv
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vmsc cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %0 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.msc.conf(<64 x i8> %a, <16 x i32> %b, <16 x i64> %acc1, i32 0)
  ret <16 x i64> %0
}


define  <16 x i64> @_Z18test_msc_elem_32_2Dv64_hS_Dv16_u7__acc64(<64 x i8> noundef %a, <64 x i8> noundef %b, <16 x i64> %acc1)  {
; CHECK-LABEL: _Z18test_msc_elem_32_2Dv64_hS_Dv16_u7__acc64:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    nopb ; mova r0, #40; nops ; nopxm ; nopv
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vmsc cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %0 = bitcast <64 x i8> %b to <16 x i32>
  %1 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.msc.conf(<64 x i8> %a, <16 x i32> %0, <16 x i64> %acc1, i32 40)
  ret <16 x i64> %1
}


define  <16 x i64> @_Z23test_msc_elem_16_2_confDv32_tiS_iDv16_u7__acc64iiii(<32 x i16> noundef %a, i32 noundef %sgn_x, <32 x i16> noundef %b, i32 noundef %sgn_y, <16 x i64> %acc1, i32 noundef %zero_acc1, i32 noundef %shift16, i32 noundef %sub_mul, i32 noundef %sub_acc1)  {
; CHECK-LABEL: _Z23test_msc_elem_16_2_confDv32_tiS_iDv16_u7__acc64iiii:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    nopb ; mova r6, #10; nops ; nopxm ; nopv
; CHECK-NEXT:    mova r7, #11
; CHECK-NEXT:    mova r8, #12
; CHECK-NEXT:    mova r9, #9
; CHECK-NEXT:    mova r10, #8
; CHECK-NEXT:    mova r11, #90
; CHECK-NEXT:    lshl r3, r3, r6
; CHECK-NEXT:    lshl r4, r4, r7
; CHECK-NEXT:    lshl r5, r5, r8
; CHECK-NEXT:    lshl r0, r0, r9
; CHECK-NEXT:    lshl r1, r1, r10
; CHECK-NEXT:    or r0, r0, r1
; CHECK-NEXT:    or r0, r0, r2
; CHECK-NEXT:    or r0, r0, r3
; CHECK-NEXT:    or r0, r0, r4
; CHECK-NEXT:    or r0, r0, r5
; CHECK-NEXT:    or r0, r0, r11
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vmsc cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %shl1.i.i = shl i32 %shift16, 10
  %shl2.i.i = shl i32 %sub_mul, 11
  %shl4.i.i = shl i32 %sub_acc1, 12
  %shl14.i.i = shl i32 %sgn_x, 9
  %shl15.i.i = shl i32 %sgn_y, 8
  %or3.i.i = or i32 %shl14.i.i, %shl15.i.i
  %or11.i.i = or i32 %or3.i.i, %zero_acc1
  %or13.i.i = or i32 %or11.i.i, %shl1.i.i
  %or16.i.i = or i32 %or13.i.i, %shl2.i.i
  %or17.i.i = or i32 %or16.i.i, %shl4.i.i
  %or19.i.i = or i32 %or17.i.i, 90
  %0 = bitcast <32 x i16> %a to <64 x i8>
  %1 = bitcast <32 x i16> %b to <16 x i32>
  %2 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.msc.conf(<64 x i8> %0, <16 x i32> %1, <16 x i64> %acc1, i32 %or19.i.i)
  ret <16 x i64> %2
}


define  <16 x i64> @_Z21test_msc_4x4_4x4_confDv32_tDv32_sDv16_u7__acc64iiii(<32 x i16> noundef %a, <32 x i16> noundef %b, <16 x i64> %acc1, i32 noundef %zero_acc1, i32 noundef %shift16, i32 noundef %sub_mul, i32 noundef %sub_acc1)  {
; CHECK-LABEL: _Z21test_msc_4x4_4x4_confDv32_tDv32_sDv16_u7__acc64iiii:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    mova r4, #10; nopb ; nopx
; CHECK-NEXT:    mova r5, #11
; CHECK-NEXT:    mova r6, #12
; CHECK-NEXT:    mova r7, #314
; CHECK-NEXT:    lshl r1, r1, r4
; CHECK-NEXT:    lshl r2, r2, r5
; CHECK-NEXT:    lshl r3, r3, r6
; CHECK-NEXT:    or r0, r1, r0
; CHECK-NEXT:    or r0, r0, r2
; CHECK-NEXT:    or r0, r0, r3
; CHECK-NEXT:    or r0, r0, r7
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vmsc cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %shl1.i.i = shl i32 %shift16, 10
  %shl2.i.i = shl i32 %sub_mul, 11
  %shl4.i.i = shl i32 %sub_acc1, 12
  %or13.i.i = or i32 %shl1.i.i, %zero_acc1
  %or16.i.i = or i32 %or13.i.i, %shl2.i.i
  %or17.i.i = or i32 %or16.i.i, %shl4.i.i
  %or19.i.i = or i32 %or17.i.i, 314
  %0 = bitcast <32 x i16> %a to <64 x i8>
  %1 = bitcast <32 x i16> %b to <16 x i32>
  %2 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.msc.conf(<64 x i8> %0, <16 x i32> %1, <16 x i64> %acc1, i32 %or19.i.i)
  ret <16 x i64> %2
}
define  <16 x i64> @_Z21test_negmsc_4x16_16x8Dv64_hDv16_jDv16_u7__acc64(<64 x i8> noundef %a, <16 x i32> noundef %b, <16 x i64> %acc1) {
; CHECK-LABEL: _Z21test_negmsc_4x16_16x8Dv64_hDv16_jDv16_u7__acc64:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    nopb ; mova r0, #0; nops ; nopxm ; nopv
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vnegmsc cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %0 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.negmsc.conf(<64 x i8> %a, <16 x i32> %b, <16 x i64> %acc1, i32 0)
  ret <16 x i64> %0
}

define  <16 x i64> @_Z19test_negmsc_2x8_8x8Dv32_tDv64_hDv16_u7__acc64(<32 x i16> noundef %a, <64 x i8> noundef %b, <16 x i64> %acc1) {
; CHECK-LABEL: _Z19test_negmsc_2x8_8x8Dv32_tDv64_hDv16_u7__acc64:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    nopb ; mova r0, #18; nops ; nopxm ; nopv
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vnegmsc cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %0 = bitcast <32 x i16> %a to <64 x i8>
  %1 = bitcast <64 x i8> %b to <16 x i32>
  %2 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.negmsc.conf(<64 x i8> %0, <16 x i32> %1, <16 x i64> %acc1, i32 18)
  ret <16 x i64> %2
}

define  <16 x i64> @_Z19test_negmsc_elem_32Dv32_tS_Dv16_u7__acc64(<32 x i16> noundef %a, <32 x i16> noundef %b, <16 x i64> %acc1) {
; CHECK-LABEL: _Z19test_negmsc_elem_32Dv32_tS_Dv16_u7__acc64:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %entry
; CHECK-NEXT:    nopb ; mova r0, #56; nops ; nopxm ; nopv
; CHECK-NEXT:    ret lr
; CHECK-NEXT:    vnegmsc cm0, cm1, x0, x2, r0 // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
entry:
  %0 = bitcast <32 x i16> %a to <64 x i8>
  %1 = bitcast <32 x i16> %b to <16 x i32>
  %2 = tail call <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.negmsc.conf(<64 x i8> %0, <16 x i32> %1, <16 x i64> %acc1, i32 56)
  ret <16 x i64> %2
}

declare <16 x i32> @llvm.aie2.v16int32()
declare <32 x i16> @llvm.aie2.v32int16()
declare <16 x i64> @llvm.aie2.v16acc64()

declare <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.mac.conf(<64 x i8>, <16 x i32>, <16 x i64>, i32)

declare <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.negmac.conf(<64 x i8>, <16 x i32>, <16 x i64>, i32)

declare <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.msc.conf(<64 x i8>, <16 x i32>, <16 x i64>, i32)

declare <16 x i64> @llvm.aie2.I512.I512.ACC1024.acc32.negmsc.conf(<64 x i8>, <16 x i32>, <16 x i64>, i32)
