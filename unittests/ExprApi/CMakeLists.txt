# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the supported Expr construction / sort API (no
# gtest); run by test/Expr/expr-api.test through lit.
add_llvm_executable(sea-expr-api-test ExprApiTest.cpp)
llvm_update_compile_flags(sea-expr-api-test)
target_link_libraries(sea-expr-api-test PRIVATE SeaExpr)
