# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_llvm_executable(sea-vc sea-vc.cpp)
llvm_update_compile_flags(sea-vc)
target_link_libraries(sea-vc PRIVATE SeaSupport SeaVC SeaInterp SeaExpr SeaTransforms MLIRSea
  MLIRParser MLIRPass)
