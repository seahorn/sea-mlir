# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the stat lines (sea/Support/Stats.h, no gtest);
# run by test/Tools/stats.test through lit.
add_llvm_executable(sea-stats-test StatsTest.cpp)
llvm_update_compile_flags(sea-stats-test)
target_link_libraries(sea-stats-test PRIVATE SeaSupport)
