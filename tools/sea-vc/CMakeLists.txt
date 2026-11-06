# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_llvm_executable(sea-vc sea-vc.cpp)
llvm_update_compile_flags(sea-vc)
target_link_libraries(sea-vc PRIVATE SeaVC SeaInterp SeaExpr SeaTransforms MLIRSea
  MLIRParser MLIRPass)
# the last place a bare --config name is looked up (build trees; an install
# has <prefix>/share/sea-mlir/profiles)
target_compile_definitions(sea-vc PRIVATE
  SEA_SOURCE_PROFILES_DIR="${PROJECT_SOURCE_DIR}/profiles")
