# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_llvm_executable(sea-opt sea-opt.cpp)
llvm_update_compile_flags(sea-opt)
target_link_libraries(sea-opt PRIVATE MLIRSea SeaTransforms SeaReduce MLIROptLib MLIRTransforms)
mlir_check_all_link_libraries(sea-opt)
