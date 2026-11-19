# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the difference decider (include/sea/Expr/
# DiffDecider.hh, BvRange.hh; no gtest); run by test/Expr/diff-decider.test.
add_llvm_executable(sea-diff-decider-test DiffDeciderTest.cpp)
llvm_update_compile_flags(sea-diff-decider-test)
target_link_libraries(sea-diff-decider-test PRIVATE SeaExpr)
