#!/bin/bash
#===-- test.sh - Reproduce min iteration count bug //
#
# This file is licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# (c) Copyright 2024 Advanced Micro Devices, Inc. or its affiliates
#

pkill meld

INPUT=test.cpp
INPUT_MOD=test_no_builtin.cpp

OPT_LEVEL=" -O2 "

PREFIX=""

GEN_RAW_IR=0
GEN_IR=1

DUMP_PASS_IR=1
DUMP_PASS_ORDER=0
DEBUG_PASSES=""
STOP_BEFORE="" # inling
STOP_AFTER=""
START_BEFORE="" # inling
START_AFTER=""

COMARE=0

workspaceFolder="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"

if [ "${STOP_BEFORE}" != "" ] && [ "${STOP_AFTER}" != "" ]; then
  echo "Only stop-before or stop-after can be set at the same time!"
  exit 1
fi

OPTS_ASM=""
OPTS_LLC=""
if [ ${DUMP_PASS_IR} -ne 0 ]; then
  OPTS_LLC="${OPTS_LLC} -mllvm -print-after-all"
fi
if [ "${DEBUG_PASSES}" != "" ]; then
  OPTS_LLC="${OPTS_LLC} -mllvm --debug-only=${DEBUG_PASSES}"
fi
## Stop
if [ "${STOP_BEFORE}" != "" ]; then
  OPTS_LLC="${OPTS_LLC} -mllvm --stop-before=${STOP_BEFORE}"
fi
if [ "${STOP_AFTER}" != "" ]; then
  OPTS_LLC="${OPTS_LLC} -mllvm --stop-before=${STOP_AFTER}"
fi
## Start
if [ "${START_BEFORE}" != "" ]; then
  OPTS_LLC="${OPTS_LLC} -mllvm --start-before=${START_BEFORE}"
fi
if [ "${START_AFTER}" != "" ]; then
  OPTS_LLC="${OPTS_LLC} -mllvm --start-before=${START_AFTER}"
fi
if [ ${DUMP_PASS_ORDER} -ne 0 ]; then
  OPTS_LLC="-mllvm -debug-pass=Arguments"
fi

FILE_RAW="kernel_${PREFIX}"
FILE_LOG="log_${PREFIX}.log"
FILE_RAW_NO_BUILTIN="kernel_${PREFIX}_no_builtins"
FILE_LOG_NO_BUILTIN="log_${PREFIX}_no_builtins.log"

OUTPUT_BUILTIN="${workspaceFolder}/${FILE_RAW}.o"
OUTPUT_NO_BUILTIN="${workspaceFolder}/${FILE_RAW_NO_BUILTIN}.o"

if [ ${GEN_RAW_IR} -ne 0 ]; then
  OPTS_ASM="-S -emit-llvm"
  OPT_LEVEL=" -O0"
  OPTS_LLC="-mllvm --debug-pass-manager"
  OUTPUT_BUILTIN="${workspaceFolder}/${FILE_RAW}_raw.ll"
  OUTPUT_NO_BUILTIN="${workspaceFolder}/${FILE_RAW_NO_BUILTIN}_raw.ll"
fi

if [ ${GEN_IR} -ne 0 ]; then
  OPTS_ASM="-S -emit-llvm"
  OUTPUT_BUILTIN="${workspaceFolder}/${FILE_RAW}.ll"
  OUTPUT_NO_BUILTIN="${workspaceFolder}/${FILE_RAW_NO_BUILTIN}.ll"
fi

cd build
if ! ninja clang; then
  echo "Compilation failed. Aborting the script."
  cd ..
  exit 1
fi
cd ..

if [ ${COMARE} -eq 0 ]; then
  # with builtins
  rm ${FILE_LOG}
  cmd_builtins="./build/bin/clang  -cc1 -triple aie2-none-unknown-elf -emit-obj ${OPT_LEVEL} ${OPTS_LLC} ${OPTS_ASM} -o ${OUTPUT_BUILTIN} -x c++ ${INPUT} >${FILE_LOG} 2>&1"
  echo "${cmd_builtins}"
  eval ${cmd_builtins} &
  first_pid=$!

else
  # disable loop
  rm ${FILE_LOG}
  cmd_builtins="./build/bin/clang  -cc1 -triple aie2-none-unknown-elf -mllvm --loop-enable-itercount=0 -emit-obj ${OPT_LEVEL} ${OPTS_LLC} ${OPTS_ASM} -o ${OUTPUT_BUILTIN} -x c++ ${INPUT_MOD} >${FILE_LOG} 2>&1"
  echo "${cmd_builtins}"
  eval ${cmd_builtins} &
  first_pid=$!
fi

# without builtins
rm ${FILE_LOG_NO_BUILTIN}
cmd_no_builtins="./build/bin/clang  -cc1 -triple aie2-none-unknown-elf -mllvm --loop-enable-itercount=1 -emit-obj ${OPT_LEVEL} ${OPTS_LLC} ${OPTS_ASM} -o ${OUTPUT_NO_BUILTIN} -x c++ ${INPUT_MOD} >${FILE_LOG_NO_BUILTIN} 2>&1"
eval ${cmd_no_builtins} &
second_pid=$!

wait ${first_pid}
wait ${second_pid}

if [ ${GEN_IR} -eq 0 ]; then

  ./build/bin/llvm-objdump -d ${OUTPUT_BUILTIN} --no-show-raw-insn >"${workspaceFolder}/${FILE_RAW}.asm"
  OUTPUT_BUILTIN="${workspaceFolder}/${FILE_RAW}.asm"

  ./build/bin/llvm-objdump -d ${OUTPUT_NO_BUILTIN} --no-show-raw-insn >"${workspaceFolder}/${FILE_RAW_NO_BUILTIN}.asm"
  OUTPUT_NO_BUILTIN="${workspaceFolder}/${FILE_RAW_NO_BUILTIN}.asm"
fi

echo "meld ${FILE_LOG} ${FILE_LOG_NO_BUILTIN}"
echo "meld ${OUTPUT_BUILTIN} ${OUTPUT_NO_BUILTIN}"

meld -n ${FILE_LOG} ${FILE_LOG_NO_BUILTIN} &
meld -n ${OUTPUT_BUILTIN} ${OUTPUT_NO_BUILTIN} &

# ./build/bin/llvm-dis ${OUTPUT_BUILTIN} -o ${OUTPUT_BUILTIN}
# ./build/bin/llvm-dis ${OUTPUT_BUILTIN} -o ${OUTPUT_BUILTIN}

# ./build/bin//opt --stop-before=inliner -O2 kernel_builtins_LL.ll  -f -o - | ./build/bin/llvm-dis -o - > tmp_builtins.ll

# ./build/bin//opt --stop-before=inliner -O2 kernel_builtins_LL.ll --print-after-all > tmp_builtins.log 2>&1
