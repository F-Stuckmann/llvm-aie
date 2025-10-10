#!/bin/bash

# This file is licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# (c) Copyright 2025 Advanced Micro Devices, Inc. or its affiliates

FILE=scripts/ReduceL2_aie2_int8_1-whole.ll
FILE=scripts/AvgPool2D_0-avgpool2d.ll
FILE=scripts/Conv2D_bf16xbfp16_0-conv2d_bf16.ll

cd build
ninja opt llc
cd ..



/scratch/work-ref/build/bin/llc -mtriple=aie2p --stop-after=register-coalescer ${FILE} -o tmp-after-coalescer.mir.orig
build/bin/llc -mtriple=aie2p --stop-after=register-coalescer ${FILE} -o tmp-after-coalescer.mir


/scratch/work-ref/build/bin/llc -mtriple=aie2p ${FILE} -o tmp.mir.orig # --debug-only=postpipeliner &>log.log.orig
build/bin/llc -mtriple=aie2p ${FILE} -o tmp.mir # --debug-only=regalloc --aie-wawreg-rewrite=false &>log.log
