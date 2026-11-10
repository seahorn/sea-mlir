# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the log selection syntax (sea/Support/Log.h, no
# gtest); run by test/Tools/log-spec.test through lit.
add_llvm_executable(sea-log-test LogTest.cpp)
llvm_update_compile_flags(sea-log-test)
target_link_libraries(sea-log-test PRIVATE SeaSupport)
