# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_llvm_executable(sea-expr-eval sea-expr-eval.cpp)
llvm_update_compile_flags(sea-expr-eval)
target_link_libraries(sea-expr-eval PRIVATE SeaExpr)
