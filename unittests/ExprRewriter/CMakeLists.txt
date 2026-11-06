# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the Expr rewriter (no gtest); run by
# test/Expr/rewriter.test through lit.
add_llvm_executable(sea-expr-rewriter-test ExprRewriterTest.cpp)
llvm_update_compile_flags(sea-expr-rewriter-test)
target_link_libraries(sea-expr-rewriter-test PRIVATE SeaExpr)
