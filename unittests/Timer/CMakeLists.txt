# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the timers (sea/Support/Timer.h, no gtest); run
# by test/Tools/timer.test through lit.
add_llvm_executable(sea-timer-test TimerTest.cpp)
llvm_update_compile_flags(sea-timer-test)
target_link_libraries(sea-timer-test PRIVATE SeaSupport)
