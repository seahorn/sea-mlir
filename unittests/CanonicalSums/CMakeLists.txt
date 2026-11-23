# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the canonical linear forms of the query
# (include/sea/Expr/CanonicalSums.hh); run by test/Expr/canonical-sums.test.
add_llvm_executable(sea-canonical-sums-test CanonicalSumsTest.cpp)
llvm_update_compile_flags(sea-canonical-sums-test)
target_link_libraries(sea-canonical-sums-test PRIVATE SeaExpr)
