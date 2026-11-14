# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the small-integer fast paths of
# include/sea/Expr/SmallArith.hh (no gtest); run by
# test/Expr/small-arith.test through lit.
add_llvm_executable(sea-small-arith-test SmallArithTest.cpp)
llvm_update_compile_flags(sea-small-arith-test)
target_link_libraries(sea-small-arith-test PRIVATE SeaExpr)
