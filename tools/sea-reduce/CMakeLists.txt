# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_llvm_executable(sea-reduce sea-reduce.cpp)
llvm_update_compile_flags(sea-reduce)
target_link_libraries(sea-reduce PRIVATE MLIRSea SeaTransforms SeaReduceInterface MLIRReduceLib
  MLIRReduce MLIRTransforms)
mlir_check_all_link_libraries(sea-reduce)
