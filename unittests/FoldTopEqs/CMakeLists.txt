# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the concat-ite hoist of --vc-fold-top-eqs (no
# gtest); run by test/VC/fold-top-eqs/hoist.test through lit.
add_llvm_executable(sea-fold-top-eqs-test FoldTopEqsTest.cpp)
llvm_update_compile_flags(sea-fold-top-eqs-test)
target_link_libraries(sea-fold-top-eqs-test PRIVATE SeaVC)
