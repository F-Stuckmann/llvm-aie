;
; This file is licensed under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; (c) Copyright 2025 Advanced Micro Devices, Inc. or its affiliates


; hoisting store due to licm 
define void @hoist_store(ptr noalias %param, i32 %0, ptr addrspace(5) %pI8.052.i.i, i32 %1, i32 %2, ptr %3, i20 %4, i20 %5, i20 %6, i20 %7, <16 x i32> %8) {
entry:
  %count2.i.i.i = getelementptr i8, ptr %param, i20 140
  br label %for.body.i31.i

for.body.i31.i:                                   ; preds = %for.body.i31.i, %entry
  %pI8.052.i.i1 = phi ptr addrspace(5) [ null, %entry ], [ %pI8.052.i.i, %for.body.i31.i ]
  %33 = zext i20 0 to i32
  store i32 %33, ptr %count2.i.i.i, align 4, !alias.scope !0
  br i1 false, label %exit, label %for.body.i31.i

exit:                                   ; preds = %for.body.i31.i
  ret void
}

; AA is not disabmiguing ptrs 
define void @sink_store_ambiguous_ptr(ptr noalias %param, i32 %0, ptr addrspace(5) %in, i32 %1, i32 %2, ptr %3, i20 %4, i20 %5, i20 %6, i20 %7, <16 x i32> %8) {
entry:
  %count2.i.i.i = getelementptr i8, ptr %param, i20 140
  br label %for.body.i31.i

for.body.i31.i:                                   ; preds = %for.body.i31.i, %entry
  %ptr.loop = phi ptr addrspace(5) [ %in, %entry ], [ %ptr.inc, %for.body.i31.i ]
  %ptr.inc = getelementptr <16 x i32>, ptr addrspace(5) %ptr.loop, i20 512
  %31 = load <16 x i32>, ptr addrspace(5) %ptr.inc, align 64
  %32 = tail call { ptr, i20, i20 } @llvm.aie2p.add.3d(ptr null, i20 0, i20 0, i20 0, i20 0, i20 0, i20 0, i20 0)
  %34 = tail call <16 x i32> @llvm.aie2p.vshuffle(<16 x i32> %31, <16 x i32> zeroinitializer, i32 0)
  %35 = zext i20 0 to i32
  store i32 %35, ptr %count2.i.i.i, align 4, !alias.scope !0
  br i1 false, label %exit, label %for.body.i31.i

exit:                                   ; preds = %for.body.i31.i
  ret void
}

; Sink store to exit
define void @sink_store(ptr noalias %param, i32 %0, ptr addrspace(5) %pI8.052.i.i, i32 %1, i32 %2, ptr %3, i20 %4, i20 %5, i20 %6, i20 %7, <16 x i32> %8) {
entry:
  %count2.i.i.i = getelementptr i8, ptr %param, i20 140
  br label %for.body.i31.i

for.body.i31.i:                                   ; preds = %for.body.i31.i, %entry
  %loop.counter = phi i32 [ 0, %entry ], [ %inc, %for.body.i31.i ]
  %inc = add i32 %loop.counter, 1
  store i32 %inc, ptr %count2.i.i.i, align 4, !alias.scope !0
  br i1 false, label %exit, label %for.body.i31.i

exit:                                   ; preds = %for.body.i31.i
  ret void
}


; Function Attrs: nounwind memory(none)
declare <32 x float> @llvm.aie2p.v32bf16.to.v32accfloat(<32 x bfloat>) #0

; Function Attrs: nounwind memory(none)
declare { ptr, i20, i20 } @llvm.aie2p.add.3d(ptr, i20, i20, i20, i20, i20, i20, i20) #0

; Function Attrs: nounwind memory(none)
declare <16 x i32> @llvm.aie2p.vshuffle(<16 x i32>, <16 x i32>, i32) #0

attributes #0 = { nounwind memory(none) }

!0 = !{!1}
!1 = distinct !{!1, !2, !"sink_store: %param"}
!2 = distinct !{!2, !"sink_store"}
