# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of Store-Map (no gtest); run by test/Expr/store-map.test
# through lit.
add_llvm_executable(sea-store-map-test StoreMapTest.cpp)
llvm_update_compile_flags(sea-store-map-test)
target_link_libraries(sea-store-map-test PRIVATE SeaExpr)
