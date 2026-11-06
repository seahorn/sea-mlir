# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_llvm_executable(sea-run sea-run.cpp)
llvm_update_compile_flags(sea-run)
target_link_libraries(sea-run PRIVATE SeaInterp SeaTransforms MLIRSea MLIRParser MLIRPass MLIRSupport)
mlir_check_all_link_libraries(sea-run)
