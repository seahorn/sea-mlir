# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the Ackermann lowering (no gtest); run by
# test/Expr/ackermann.test through lit.
add_llvm_executable(sea-ackermann-test AckermannTest.cpp)
llvm_update_compile_flags(sea-ackermann-test)
target_link_libraries(sea-ackermann-test PRIVATE SeaExpr)
