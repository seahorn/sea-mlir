# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the memory-operation terms (no gtest); run by
# test/Expr/memops.test through lit.
add_llvm_executable(sea-memops-test MemOpsTest.cpp)
llvm_update_compile_flags(sea-memops-test)
target_link_libraries(sea-memops-test PRIVATE SeaExpr)
