# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of ARM (include/sea/Expr/AddrRangeMap.hh, no gtest);
# run by test/Expr/addr-range.test through lit.
add_llvm_executable(sea-addr-range-test AddrRangeTest.cpp)
llvm_update_compile_flags(sea-addr-range-test)
target_link_libraries(sea-addr-range-test PRIVATE SeaExpr)
