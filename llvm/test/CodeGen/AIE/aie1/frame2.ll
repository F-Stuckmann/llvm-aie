; NOTE: Assertions have been autogenerated by utils/update_mir_test_checks.py
;
; This file is licensed under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; (c) Copyright 2023-2024 Advanced Micro Devices, Inc. or its affiliates
; RUN: llc -mtriple=aie --stop-after=prologepilog < %s \
; RUN:   | FileCheck -check-prefix=FPELIM %s
; RUN: llc -mtriple=aie --stop-after=prologepilog -frame-pointer=all < %s \
; RUN:   | FileCheck -check-prefix=WITHFP %s


%struct.key_t = type { i32, [16 x i8] }

define i32 @test() nounwind {
;
;
  ; FPELIM-LABEL: name: test
  ; FPELIM: bb.0 (%ir-block.0):
  ; FPELIM-NEXT:   frame-setup PADDA_sp_imm 32, implicit-def $sp, implicit $sp
  ; FPELIM-NEXT:   ST_SPIL_PTR killed $lr, -32, implicit $sp :: (store (s32) into %stack.1)
  ; FPELIM-NEXT:   $r12 = MV_SPECIAL2R $sp
  ; FPELIM-NEXT:   $p0 = MOV killed $r12
  ; FPELIM-NEXT:   $p0 = PADDA_nrm_imm $p0, -28
  ; FPELIM-NEXT:   renamable $p0 = nuw PADDA_nrm_imm killed renamable $p0, 4
  ; FPELIM-NEXT:   JAL @test1, csr_aie1, implicit-def dead $lr, implicit $p0, implicit-def $sp
  ; FPELIM-NEXT:   renamable $r0 = MOV_U20 0
  ; FPELIM-NEXT:   $lr = LR_LOAD -32, implicit-def $r15, implicit $sp :: (load (s32) from %stack.1)
  ; FPELIM-NEXT:   frame-destroy PADDA_sp_imm -32, implicit-def $sp, implicit $sp
  ; FPELIM-NEXT:   PseudoRET implicit $lr, implicit $r0
  ; WITHFP-LABEL: name: test
  ; WITHFP: bb.0 (%ir-block.0):
  ; WITHFP-NEXT:   frame-setup PADDA_sp_imm 32, implicit-def $sp, implicit $sp
  ; WITHFP-NEXT:   ST_SPIL_PTR killed $lr, -32, implicit $sp :: (store (s32) into %stack.1)
  ; WITHFP-NEXT:   $r12 = frame-setup MV_SPECIAL2R $sp
  ; WITHFP-NEXT:   $p7 = frame-setup MOV killed $r12
  ; WITHFP-NEXT:   $p7 = frame-setup PADDA_nrm_imm $p7, -32
  ; WITHFP-NEXT:   $p0 = MOV $p7
  ; WITHFP-NEXT:   $p0 = PADDA_nrm_imm $p0, -28
  ; WITHFP-NEXT:   renamable $p0 = nuw PADDA_nrm_imm killed renamable $p0, 4
  ; WITHFP-NEXT:   JAL @test1, csr_aie1, implicit-def dead $lr, implicit $p0, implicit-def $sp
  ; WITHFP-NEXT:   renamable $r0 = MOV_U20 0
  ; WITHFP-NEXT:   $lr = LR_LOAD -32, implicit-def $r15, implicit $sp :: (load (s32) from %stack.1)
  ; WITHFP-NEXT:   frame-destroy PADDA_sp_imm -32, implicit-def $sp, implicit $sp
  ; WITHFP-NEXT:   PseudoRET implicit $lr, implicit $r0
  %key = alloca %struct.key_t, align 4
  %1 = bitcast %struct.key_t* %key to i8*
  %2 = getelementptr inbounds %struct.key_t, %struct.key_t* %key, i64 0, i32 1, i64 0
  call void @test1(i8* %2) #3
  ret i32 0
}

declare void @llvm.memset.p0i8.i64(i8* nocapture, i8, i64, i1)

declare void @test1(i8*)
