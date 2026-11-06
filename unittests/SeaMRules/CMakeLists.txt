# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the SeaM rewrite rules (no gtest); run by
# test/Expr/seam-rules.test through lit.
add_llvm_executable(sea-seam-rules-test SeaMRulesTest.cpp)
llvm_update_compile_flags(sea-seam-rules-test)
target_link_libraries(sea-seam-rules-test PRIVATE SeaExpr)
