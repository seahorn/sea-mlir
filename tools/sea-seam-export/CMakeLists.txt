# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_llvm_executable(sea-seam-export sea-seam-export.cpp)
llvm_update_compile_flags(sea-seam-export)
target_link_libraries(sea-seam-export PRIVATE SeaVC SeaExpr MLIRSea MLIRParser)
