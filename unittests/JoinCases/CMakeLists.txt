# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the cases joins and their counters
# (include/sea/VC/JoinCases.h, sea-vc --vc-join-cases, experimental); run by
# test/VC/join-cases-counters.test through lit.
add_llvm_executable(sea-join-cases-test JoinCasesTest.cpp)
llvm_update_compile_flags(sea-join-cases-test)
target_link_libraries(sea-join-cases-test PRIVATE SeaVC SeaExpr)
