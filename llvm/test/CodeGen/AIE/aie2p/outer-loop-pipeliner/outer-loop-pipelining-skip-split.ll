; This file is licensed under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates
;
; RUN: llc -mtriple=aie2p -O2 -aie-enable-outer-loop-pipelining \
; RUN:     -stop-after=aie-outer-loop-pipeliner -o - %s 2>&1 \
; RUN:   | sed '/^[[:space:]]*$/d' | FileCheck %s
;
; RUN: llc -mtriple=aie2p -O2 -aie-enable-outer-loop-pipelining \
; RUN:     -stop-after=aie-outer-loop-pipeliner -o - %s \
; RUN:   | llc -mtriple=aie2p -x mir -run-pass=none -o /dev/null

; Pins the outer loop pipeliner's full post-pass IR with an exhaustive CHECK-NEXT
; transcript, so the exact CFG skeleton is verified and no instruction is missed.

; ============================================================================
; Test 1: an upcounting outer loop.
; ============================================================================

; CHECK-LABEL: define void @nested_loop_basic(ptr noalias %a, ptr noalias %b, ptr noalias %c, i32 %N, i32 %M) {
; CHECK-NEXT: entry:
; CHECK-NEXT: %cmp.outer = icmp sgt i32 %N, 1
; CHECK-NEXT: br i1 %cmp.outer, label %outer.header.preheader, label %exit
; CHECK-NEXT: outer.header.preheader: ; preds = %entry
; CHECK-NEXT: br label %stage0.top
; CHECK-NEXT: stage0.top: ; preds = %outer.header.preheader
; CHECK-NEXT: %v0.steady.top = load i32, ptr %a, align 4
; CHECK-NEXT: %v1.steady.top = load i32, ptr %b, align 4
; CHECK-NEXT: %.top = mul i32 %v0.steady.top, %v1.steady.top
; CHECK-NEXT: %outer.trip.adj = sub i32 %N, 1
; CHECK-NEXT: br label %steady.stage1.top
; CHECK-NEXT: steady.stage1.top: ; preds = %stage0.top, %steady.stage1.bottom.and.stage0.top
; CHECK-NEXT: %i.steady = phi i32 [ %i.next.steady, %steady.stage1.bottom.and.stage0.top ], [ 0, %stage0.top ]
; CHECK-NEXT: %a.ptr.steady = phi ptr [ %a.ptr.next.steady, %steady.stage1.bottom.and.stage0.top ], [ %a, %stage0.top ]
; CHECK-NEXT: %b.ptr.steady = phi ptr [ %b.ptr.next.steady, %steady.stage1.bottom.and.stage0.top ], [ %b, %stage0.top ]
; CHECK-NEXT: %c.ptr.steady = phi ptr [ %c.ptr.next.steady, %steady.stage1.bottom.and.stage0.top ], [ %c, %stage0.top ]
; CHECK-NEXT: %v0.steady.phi = phi i32 [ %v0.steady.top, %stage0.top ], [ %v0.steady.bottom, %steady.stage1.bottom.and.stage0.top ]
; CHECK-NEXT: %v1.steady.phi = phi i32 [ %v1.steady.top, %stage0.top ], [ %v1.steady.bottom, %steady.stage1.bottom.and.stage0.top ]
; CHECK-NEXT: %.phi = phi i32 [ %.top, %stage0.top ], [ %.bottom, %steady.stage1.bottom.and.stage0.top ]
; CHECK-NEXT: call void @llvm.set.loop.iterations.i32(i32 %M)
; CHECK-NEXT: %a.ptr.next.steady = getelementptr inbounds i32, ptr %a.ptr.steady, i32 1
; CHECK-NEXT: %b.ptr.next.steady = getelementptr inbounds i32, ptr %b.ptr.steady, i32 1
; CHECK-NEXT: %c.ptr.next.steady = getelementptr inbounds i32, ptr %c.ptr.steady, i32 1
; CHECK-NEXT: br label %steady.stage1.inner.inner.header
; CHECK-NEXT: steady.stage1.inner.inner.header: ; preds = %steady.stage1.inner.inner.header, %steady.stage1.top
; CHECK-NEXT: %lsr.iv.steady = phi i32 [ %lsr.iv.next.steady, %steady.stage1.inner.inner.header ], [ 0, %steady.stage1.top ]
; CHECK-NEXT: %inner.cond.steady = call i1 @llvm.loop.decrement.i32(i32 1)
; CHECK-NEXT: %lsr.iv.next.steady = add i32 %lsr.iv.steady, %.phi
; CHECK-NEXT: br i1 %inner.cond.steady, label %steady.stage1.inner.inner.header, label %steady.stage1.bottom.and.stage0.top, !llvm.loop !0
; CHECK-NEXT: steady.stage1.bottom.and.stage0.top: ; preds = %steady.stage1.inner.inner.header
; CHECK-NEXT: store i32 %lsr.iv.next.steady, ptr %c.ptr.steady, align 4
; CHECK-NEXT: %i.next.steady = add i32 %i.steady, 1
; CHECK-NEXT: %outer.cond.steady = icmp slt i32 %i.next.steady, %outer.trip.adj
; CHECK-NEXT: %v0.steady.bottom = load i32, ptr %a.ptr.next.steady, align 4
; CHECK-NEXT: %v1.steady.bottom = load i32, ptr %b.ptr.next.steady, align 4
; CHECK-NEXT: %.bottom = mul i32 %v0.steady.bottom, %v1.steady.bottom
; CHECK-NEXT: br i1 %outer.cond.steady, label %steady.stage1.top, label %lastiter.stage1.top, !llvm.loop !2
; CHECK-NEXT: lastiter.stage1.top: ; preds = %steady.stage1.bottom.and.stage0.top
; CHECK-NEXT: call void @llvm.set.loop.iterations.i32(i32 %M)
; CHECK-NEXT: br label %lastiter.stage1.inner.inner.header
; CHECK-NEXT: lastiter.stage1.inner.inner.header: ; preds = %lastiter.stage1.top, %lastiter.stage1.inner.inner.header
; CHECK-NEXT: %lsr.iv.lastiter = phi i32 [ %lsr.iv.next.lastiter, %lastiter.stage1.inner.inner.header ], [ 0, %lastiter.stage1.top ]
; CHECK-NEXT: %inner.cond.lastiter = call i1 @llvm.loop.decrement.i32(i32 1)
; CHECK-NEXT: %lsr.iv.next.lastiter = add i32 %lsr.iv.lastiter, %.bottom
; CHECK-NEXT: br i1 %inner.cond.lastiter, label %lastiter.stage1.inner.inner.header, label %lastiter.stage1.bottom, !llvm.loop !0
; CHECK-NEXT: lastiter.stage1.bottom: ; preds = %lastiter.stage1.inner.inner.header
; CHECK-NEXT: store i32 %lsr.iv.next.lastiter, ptr %c.ptr.next.steady, align 4
; CHECK-NEXT: br label %exit
; CHECK-NEXT: exit: ; preds = %lastiter.stage1.bottom, %entry
; CHECK-NEXT: ret void
; CHECK-NEXT: }

; ============================================================================
; Test 2: a downcounting outer loop (converted to a JNZD hardware loop).
; ============================================================================

; CHECK-LABEL: define void @downcount(ptr noalias %a, ptr noalias %c, i32 %N, i32 %M) {
; CHECK-NEXT: entry:
; CHECK-NEXT: %cmp.outer = icmp sgt i32 %N, 1
; CHECK-NEXT: br i1 %cmp.outer, label %outer.header.preheader, label %exit
; CHECK-NEXT: outer.header.preheader: ; preds = %entry
; CHECK-NEXT: br label %stage0.top
; CHECK-NEXT: stage0.top: ; preds = %outer.header.preheader
; CHECK-NEXT: %v0.steady.top = load i32, ptr %a, align 4
; CHECK-NEXT: %outer.jnzd.tc = sub i32 %N, 2
; CHECK-NEXT: %outer.ctr.init = call i32 @llvm.start.loop.iterations.i32(i32 %outer.jnzd.tc)
; CHECK-NEXT: br label %steady.stage1.top
; CHECK-NEXT: steady.stage1.top: ; preds = %stage0.top, %steady.stage1.bottom.and.stage0.top
; CHECK-NEXT: %a.ptr.steady = phi ptr [ %a.ptr.next.steady, %steady.stage1.bottom.and.stage0.top ], [ %a, %stage0.top ]
; CHECK-NEXT: %c.ptr.steady = phi ptr [ %c.ptr.next.steady, %steady.stage1.bottom.and.stage0.top ], [ %c, %stage0.top ]
; CHECK-NEXT: %v0.steady.phi = phi i32 [ %v0.steady.top, %stage0.top ], [ %v0.steady.bottom, %steady.stage1.bottom.and.stage0.top ]
; CHECK-NEXT: %outer.ctr = phi i32 [ %outer.ctr.init, %stage0.top ], [ %outer.ctr.next, %steady.stage1.bottom.and.stage0.top ]
; CHECK-NEXT: call void @llvm.set.loop.iterations.i32(i32 %M)
; CHECK-NEXT: %a.ptr.next.steady = getelementptr inbounds i32, ptr %a.ptr.steady, i32 1
; CHECK-NEXT: %c.ptr.next.steady = getelementptr inbounds i32, ptr %c.ptr.steady, i32 1
; CHECK-NEXT: br label %steady.stage1.inner.inner.header
; CHECK-NEXT: steady.stage1.inner.inner.header: ; preds = %steady.stage1.inner.inner.header, %steady.stage1.top
; CHECK-NEXT: %acc.steady = phi i32 [ 0, %steady.stage1.top ], [ %acc.next.steady, %steady.stage1.inner.inner.header ]
; CHECK-NEXT: %acc.next.steady = add i32 %acc.steady, %v0.steady.phi
; CHECK-NEXT: %inner.cond.steady = call i1 @llvm.loop.decrement.i32(i32 1)
; CHECK-NEXT: br i1 %inner.cond.steady, label %steady.stage1.inner.inner.header, label %steady.stage1.bottom.and.stage0.top, !llvm.loop !0
; CHECK-NEXT: steady.stage1.bottom.and.stage0.top: ; preds = %steady.stage1.inner.inner.header
; CHECK-NEXT: store i32 %acc.next.steady, ptr %c.ptr.steady, align 4
; CHECK-NEXT: %v0.steady.bottom = load i32, ptr %a.ptr.next.steady, align 4
; CHECK-NEXT: %outer.ctr.next = call i32 @llvm.loop.decrement.reg.i32(i32 %outer.ctr, i32 1)
; CHECK-NEXT: %outer.loop.cond = icmp ne i32 %outer.ctr.next, 0
; CHECK-NEXT: br i1 %outer.loop.cond, label %steady.stage1.top, label %lastiter.stage1.top, !llvm.loop !5
; CHECK-NEXT: lastiter.stage1.top: ; preds = %steady.stage1.bottom.and.stage0.top
; CHECK-NEXT: call void @llvm.set.loop.iterations.i32(i32 %M)
; CHECK-NEXT: br label %lastiter.stage1.inner.inner.header
; CHECK-NEXT: lastiter.stage1.inner.inner.header: ; preds = %lastiter.stage1.top, %lastiter.stage1.inner.inner.header
; CHECK-NEXT: %acc.lastiter = phi i32 [ 0, %lastiter.stage1.top ], [ %acc.next.lastiter, %lastiter.stage1.inner.inner.header ]
; CHECK-NEXT: %acc.next.lastiter = add i32 %acc.lastiter, %v0.steady.bottom
; CHECK-NEXT: %inner.cond.lastiter = call i1 @llvm.loop.decrement.i32(i32 1)
; CHECK-NEXT: br i1 %inner.cond.lastiter, label %lastiter.stage1.inner.inner.header, label %lastiter.stage1.bottom, !llvm.loop !0
; CHECK-NEXT: lastiter.stage1.bottom: ; preds = %lastiter.stage1.inner.inner.header
; CHECK-NEXT: store i32 %acc.next.lastiter, ptr %c.ptr.next.steady, align 4
; CHECK-NEXT: br label %exit
; CHECK-NEXT: exit: ; preds = %lastiter.stage1.bottom, %entry
; CHECK-NEXT: ret void
; CHECK-NEXT: }

define void @nested_loop_basic(ptr noalias %a, ptr noalias %b, ptr noalias %c,
                                i32 %N, i32 %M) {
entry:
  %cmp.outer = icmp sgt i32 %N, 1
  br i1 %cmp.outer, label %outer.header, label %exit

outer.header:
  %i = phi i32 [ 0, %entry ], [ %i.next, %outer.latch ]
  %a.ptr = phi ptr [ %a, %entry ], [ %a.ptr.next, %outer.latch ]
  %b.ptr = phi ptr [ %b, %entry ], [ %b.ptr.next, %outer.latch ]
  %c.ptr = phi ptr [ %c, %entry ], [ %c.ptr.next, %outer.latch ]
  %v0 = load i32, ptr %a.ptr, align 4
  %v1 = load i32, ptr %b.ptr, align 4
  call void @llvm.set.loop.iterations.i32(i32 %M)
  br label %inner.header

inner.header:
  %acc = phi i32 [ 0, %outer.header ], [ %acc.next, %inner.header ]
  %prod = mul i32 %v0, %v1
  %acc.next = add i32 %acc, %prod
  %inner.cond = call i1 @llvm.loop.decrement.i32(i32 1)
  br i1 %inner.cond, label %inner.header, label %outer.latch, !llvm.loop !1

outer.latch:
  store i32 %acc.next, ptr %c.ptr, align 4
  %a.ptr.next = getelementptr inbounds i32, ptr %a.ptr, i32 1
  %b.ptr.next = getelementptr inbounds i32, ptr %b.ptr, i32 1
  %c.ptr.next = getelementptr inbounds i32, ptr %c.ptr, i32 1
  %i.next = add i32 %i, 1
  %outer.cond = icmp slt i32 %i.next, %N
  br i1 %outer.cond, label %outer.header, label %exit, !llvm.loop !0

exit:
  ret void
}






define void @downcount(ptr noalias %a, ptr noalias %c, i32 %N, i32 %M) {
entry:
  %cmp.outer = icmp sgt i32 %N, 1
  br i1 %cmp.outer, label %outer.header, label %exit

outer.header:
  %i = phi i32 [ %N, %entry ], [ %i.next, %outer.latch ]
  %a.ptr = phi ptr [ %a, %entry ], [ %a.ptr.next, %outer.latch ]
  %c.ptr = phi ptr [ %c, %entry ], [ %c.ptr.next, %outer.latch ]
  %v0 = load i32, ptr %a.ptr, align 4
  call void @llvm.set.loop.iterations.i32(i32 %M)
  br label %inner.header

inner.header:
  %acc = phi i32 [ 0, %outer.header ], [ %acc.next, %inner.header ]
  %acc.next = add i32 %acc, %v0
  %inner.cond = call i1 @llvm.loop.decrement.i32(i32 1)
  br i1 %inner.cond, label %inner.header, label %outer.latch, !llvm.loop !1

outer.latch:
  store i32 %acc.next, ptr %c.ptr, align 4
  %a.ptr.next = getelementptr inbounds i32, ptr %a.ptr, i32 1
  %c.ptr.next = getelementptr inbounds i32, ptr %c.ptr, i32 1
  %i.next = add i32 %i, -1
  %outer.cond = icmp sgt i32 %i.next, 1
  br i1 %outer.cond, label %outer.header, label %exit, !llvm.loop !0

exit:
  ret void
}

declare void @llvm.set.loop.iterations.i32(i32)
declare i1 @llvm.loop.decrement.i32(i32)

!0 = distinct !{!0, !2, !3}
!1 = distinct !{!1, !2}
!2 = !{!"llvm.loop.mustprogress"}
!3 = !{!"llvm.loop.itercount.range", i32 2}
