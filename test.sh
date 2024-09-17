#!/bin/bash
#===-- test.sh - Reproduce min iteration count bug //
#
# This file is licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# (c) Copyright 2024 Advanced Micro Devices, Inc. or its affiliates
#

GEN_RAW_IR=0
GEN_IR=1

workspaceFolder="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
OPTS_ASM="-S -emit-llvm"
OPTS_ASM=""
OPTS_LLC="-mllvm -debug-pass=Arguments"
OPTS_LLC="-mllvm -print-after-all"
# OPTS_LLC="-mllvm --stop-before=inling -mllvm -print-after-all"
# OPTS_LLC="-mllvm --debug-only=inline,loop-simplify,loop-rotate,simplifycfg,scalar-evolution"
# OPTS_LLC="-mllvm --debug-only=scalar-evolution"
# OPTS_LLC=""
OPT_LEVEL=" -O2"

OUTPUT_BUILTIN="${workspaceFolder}/kernel_builtins.asm"
OUTPUT_NO_BUILTIN="${workspaceFolder}/kernel_no_builtins.asm"

if [ ${GEN_RAW_IR} -ne 0 ]; then 
  OPTS_ASM="-S -emit-llvm"
  OPT_LEVEL=" -O0"
  OPTS_LLC="-mllvm --debug-pass-manager"
  OUTPUT_BUILTIN="${workspaceFolder}/kernel_builtins_LL.ll"
  OUTPUT_NO_BUILTIN="${workspaceFolder}/kernel_no_builtins_LL.ll"
fi

if [ ${GEN_IR} -ne 0 ]; then 
  OPTS_ASM="-S -emit-llvm"
fi

cd build; 
if ! ninja clang; then
  echo "Compilation failed. Aborting the script."
  cd ..
  exit 1
fi
cd ..



# with builtins
cmd_builtins="./build/bin/clang  -cc1 -triple aie2-none-unknown-elf -emit-obj ${OPT_LEVEL} ${OPTS_LLC} ${OPTS_ASM} -o ${OUTPUT_BUILTIN} -x c++ test.cpp >log.log 2>&1"
echo "${cmd_builtins}"
eval ${cmd_builtins} & 
first_pid=$!


# without builtins
cmd_no_builtins="./build/bin/clang  -cc1 -triple aie2-none-unknown-elf -emit-obj ${OPT_LEVEL} ${OPTS_LLC} ${OPTS_ASM} -o ${OUTPUT_NO_BUILTIN} -x c++ test_no_builtin.cpp >log_no_builtins.log 2>&1"
eval ${cmd_no_builtins} &
second_pid=$!

wait ${first_pid}
wait ${second_pid}

echo "meld log.log log_no_builtins.log"
echo "meld ${OUTPUT_BUILTIN} ${OUTPUT_NO_BUILTIN}"

./build/bin/llvm-dis ${OUTPUT_BUILTIN} -o ${OUTPUT_BUILTIN}
./build/bin/llvm-dis ${OUTPUT_BUILTIN} -o ${OUTPUT_BUILTIN}


# ./build/bin//opt --stop-before=inliner -O2 kernel_builtins_LL.ll  -f -o - | ./build/bin/llvm-dis -o - > tmp_builtins.ll

# ./build/bin//opt --stop-before=inliner -O2 kernel_builtins_LL.ll --print-after-all > tmp_builtins.log 2>&1
