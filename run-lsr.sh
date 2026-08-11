#!/bin/bash
# Split the pipeline around loop-strength-reduce, starting from C++, and keep
# every intermediate around for inspection:
#
#   1. clang -O1 -emit-llvm          -> $OUT/01-clang.ll     (IR out of the frontend+opt pipeline)
#   2. llc -stop-before=loop-reduce  -> $OUT/02-pre-lsr.mir  (codegen halted right before LSR)
#   3. strip the MIR wrapper         -> $OUT/03-pre-lsr.ll   (same module, parseable by opt)
#   4. opt -passes=loop-reduce       -> $OUT/04-post-lsr.ll  (LSR alone)
#

set -euo pipefail

LLVM_BIN=${LLVM_BIN:-build/bin}
FILE=${1:-lsr-test.cpp}
TRIPLE=${TRIPLE:-aie2ps}
OPTLEVEL=${OPTLEVEL:--O1}
OUT=${OUT:-lsr-out}

mkdir -p "$OUT"

# 1. -fno-discard-value-names keeps the source-derived names, so the before/after
#    diff stays readable. -O1 avoids unrolling, which otherwise buries the loop.
"$LLVM_BIN/clang" --target="$TRIPLE" "$OPTLEVEL" -fno-discard-value-names \
  -S -emit-llvm "$FILE" -o "$OUT/01-clang.ll"

# 2. Everything the codegen pipeline does before LSR; the dumped MIR still
#    carries the IR module because no MachineFunction has been built yet.
"$LLVM_BIN/llc" -mtriple="$TRIPLE" -stop-before=loop-reduce \
  "$OUT/01-clang.ll" -o "$OUT/02-pre-lsr.mir"

# 3. The MIR file is `--- |` + the two-space-indented module + per-function yaml
#    stubs; keep only the module so opt can parse it.
awk 'NR==1 {next} /^\.\.\.$/ {exit} {sub(/^  /, ""); print}' \
  "$OUT/02-pre-lsr.mir" > "$OUT/03-pre-lsr.ll"

# 4. Resume with LSR alone. -mtriple matters: LSR queries the target's
#    addressing modes through TTI.
"$LLVM_BIN/opt" -mtriple="$TRIPLE" -passes=loop-reduce \
  -S "$OUT/03-pre-lsr.ll" -o "$OUT/04-post-lsr.ll"

echo "clang IR    : $OUT/01-clang.ll"
echo "pre-LSR MIR : $OUT/02-pre-lsr.mir"
echo "pre-LSR IR  : $OUT/03-pre-lsr.ll"
echo "post-LSR IR : $OUT/04-post-lsr.ll"
echo
echo "diff (pre-LSR -> post-LSR):"
diff -u "$OUT/03-pre-lsr.ll" "$OUT/04-post-lsr.ll" || true
