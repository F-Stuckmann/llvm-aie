  ;===-- Induction_var.ll - tricky InductionVar extraction----*- IR-*-===//
;
; This file is licensed under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; (c) Copyright 2024 Advanced Micro Devices, Inc. or its affiliates
;

; ModuleID = 'test_no_builtin.cpp'
source_filename = "test_no_builtin.cpp"
target datalayout = "e-m:e-p:20:32-i1:8:32-i8:8:32-i16:16:32-i32:32:32-f32:32:32-i64:32-f64:32-a:0:32-n32"
target triple = "aie2-none-unknown-elf"

; Function Attrs: mustprogress nofree norecurse nosync nounwind memory(argmem: readwrite, inaccessiblemem: write)
define dso_local void @_ZtruncCounter(i32 noundef %num_elems, i32 noundef %n, ptr nonnull align 4 dereferenceable(4) %it_input) local_unnamed_addr #0 {
entry:
  %input_max.sroa.0.promoted = load ptr, ptr %it_input, align 1
  br label %for.cond

for.cond:
  %add.ptr.i.i.i.i107 = phi ptr [ %input_max.sroa.0.promoted, %entry ], [ %add.ptr.i.i.i.i, %for.body ]
  %i.0 = phi i32 [ 0, %entry ], [ %add, %for.body ]
  %cmp = icmp ult i32 %i.0, %n
  br i1 %cmp, label %for.body, label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.cond
  ret void

for.body:                                         ; preds = %for.cond, %for.body
  %add.ptr.i.i.i.i = getelementptr inbounds i8, ptr %add.ptr.i.i.i.i107, i20 32
  %add = add nuw nsw i32 %i.0 , 1
  br label %for.cond, !llvm.loop !6
}

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: write)
declare void @llvm.assume(i1 noundef) #1

attributes #0 = { mustprogress nofree norecurse nosync nounwind memory(argmem: readwrite, inaccessiblemem: write) "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #1 = { nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: write) }

!llvm.module.flags = !{!0}

!0 = !{i32 1, !"wchar_size", i32 4}
!2 = !{!3, !3, i64 0}
!3 = !{!"int", !4, i64 0}
!4 = !{!"omnipotent char", !5, i64 0}
!5 = !{!"Simple C++ TBAA"}
!6 = distinct !{!6, !7, !8, !9}
!7 = !{!"llvm.loop.mustprogress"}
!8 = !{!"llvm.loop.itercount.range", i64 4}
!9 = !{!"llvm.loop.unroll.disable"}
!10 = distinct !{!10, !7, !8, !9}
!11 = distinct !{!11, !7, !8, !9}
!12 = distinct !{!12, !7, !9}
!13 = distinct !{!13, !7, !8, !9}
!14 = distinct !{!14, !7, !8, !9}
!15 = distinct !{!15, !7, !9}
!16 = distinct !{!16, !7, !9}
!17 = distinct !{!17, !7, !8, !9}
!18 = distinct !{!18, !7, !9}
!19 = distinct !{!19, !7, !9}
!20 = distinct !{!20, !7, !9}
!21 = distinct !{!21, !7, !9}
!22 = distinct !{!22, !7, !8, !9}
!23 = distinct !{!23, !7, !9}
!24 = distinct !{!24, !7, !9}
!25 = distinct !{!25, !7, !8, !9}
!26 = distinct !{!26, !7, !8, !9}
!27 = distinct !{!27, !7, !28, !9}
!28 = !{!"llvm.loop.itercount.range", i64 2}
