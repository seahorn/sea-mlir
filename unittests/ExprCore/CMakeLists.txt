# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the Expr core counters (no gtest); run by
# test/Expr/expr-core.test through lit (normal mode and crash modes).
add_llvm_executable(sea-expr-core-test ExprCoreTest.cpp)
llvm_update_compile_flags(sea-expr-core-test)
target_link_libraries(sea-expr-core-test PRIVATE SeaExpr)
