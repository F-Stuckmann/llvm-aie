#!/bin/bash
# Split the pipeline around loop-strength-reduce, starting from C++, and keep
# every intermediate around for inspection:
#
#   1. clang -O2 -emit-llvm   -> $OUT/01-clang.ll     (full middle-end output)
#   2. halt codegen before LSR-> $OUT/02-pre-lsr.*    (see METHOD below)
#   3.                        -> $OUT/03-pre-lsr.ll   (module, parseable by opt)
#   4. opt -passes=loop-reduce-> $OUT/04-post-lsr.ll  (LSR alone)
#
# METHOD picks how codegen is halted at step 2:
#   stop  - llc -stop-before=loop-reduce dumps MIR; no MachineFunction exists
#           yet, so the MIR is just the IR module in a `--- |` wrapper.
#   print - llc -print-before=loop-reduce -print-module-scope dumps the module
#           to stderr, the same instrumentation run-o1-pass.sh uses for
#           middle-end passes. Works for any pass, not just pipeline prefixes.
#
#   ./run-lsr.sh                                  # -O2, MIR stop point
#   METHOD=print ./run-lsr.sh                     # -O2, print-before dump
#   OPTLEVEL=-O1 METHOD=print ./run-lsr.sh kernel.cpp

set -euo pipefail

LLVM_BIN=${LLVM_BIN:-build/bin}
FILE=${1:-lsr-test.cpp}
TRIPLE=${TRIPLE:-aie2ps}
OPTLEVEL=${OPTLEVEL:--O2}
METHOD=${METHOD:-stop}
OUT=${OUT:-lsr-out}

mkdir -p "$OUT"
echo "$OUT"

# 1. Whole middle-end pipeline. -fno-discard-value-names keeps the
#    source-derived names so the before/after diff stays readable.
"$LLVM_BIN/clang" --target="$TRIPLE" "$OPTLEVEL" -fno-discard-value-names \
  -S -emit-llvm "$FILE" -o "$OUT/01-clang.ll"

case $METHOD in
stop)
  "$LLVM_BIN/llc" -mtriple="$TRIPLE" -stop-before=loop-reduce \
    "$OUT/01-clang.ll" -o "$OUT/02-pre-lsr.mir"
  # `--- |` + the two-space-indented module + per-function yaml stubs.
  awk 'NR==1 {next} /^\.\.\.$/ {exit} {sub(/^  /, ""); print}' \
    "$OUT/02-pre-lsr.mir" > "$OUT/03-pre-lsr.ll"
  HALT="$OUT/02-pre-lsr.mir"
  ;;
print)
  "$LLVM_BIN/llc" -mtriple="$TRIPLE" -print-before=loop-reduce \
    -print-module-scope "$OUT/01-clang.ll" -o /dev/null \
    2> "$OUT/02-print-before.log"
  # One "*** IR Dump Before ... ***" banner per loop LSR visits; keep dump #1.
  awk '/^;? ?\*\*\* IR Dump/ {n++; next} n==1 {print}' \
    "$OUT/02-print-before.log" > "$OUT/03-pre-lsr.ll"
  HALT="$OUT/02-print-before.log"
  ;;
*)
  echo "error: METHOD must be 'stop' or 'print', got '$METHOD'" >&2
  exit 1
  ;;
esac

# 4. Resume with LSR alone. -mtriple matters: LSR queries the target's
#    addressing modes through TTI.
"$LLVM_BIN/opt" -mtriple="$TRIPLE" -passes=loop-reduce \
  -S "$OUT/03-pre-lsr.ll" -o "$OUT/04-post-lsr.ll"

echo "clang IR ($OPTLEVEL)  : $OUT/01-clang.ll"
echo "halt ($METHOD)       : $HALT"
echo "pre-LSR IR       : $OUT/03-pre-lsr.ll"
echo "post-LSR IR      : $OUT/04-post-lsr.ll"
echo
echo "diff (pre-LSR -> post-LSR):"
# diff -u "$OUT/03-pre-lsr.ll" "$OUT/04-post-lsr.ll" || true
