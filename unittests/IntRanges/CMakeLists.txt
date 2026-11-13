# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main oracle of the integer ranges of the sea value ops
# (InferIntRangeInterface, lib/Dialect/Sea/SeaIntRanges.cpp) and of the facts
# of sea-range-fold, exhaustive at small widths against evalPureOp; run by
# test/Transforms/int-ranges.test through lit.
add_llvm_executable(sea-int-ranges-test IntRangesTest.cpp)
llvm_update_compile_flags(sea-int-ranges-test)
target_link_libraries(sea-int-ranges-test PRIVATE SeaTransforms MLIRSea MLIRParser)
