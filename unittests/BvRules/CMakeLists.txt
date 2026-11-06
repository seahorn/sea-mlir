# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the bit-vector / Boolean normalization rules
# (include/sea/Expr/BvRules.hh); run by test/Expr/bv-rules.test.
add_llvm_executable(sea-bv-rules-test BvRulesTest.cpp)
llvm_update_compile_flags(sea-bv-rules-test)
target_link_libraries(sea-bv-rules-test PRIVATE SeaExpr)
