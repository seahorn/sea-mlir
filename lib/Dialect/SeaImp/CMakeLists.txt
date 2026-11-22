# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_mlir_dialect_library(MLIRSeaImp
  SeaImpDialect.cpp
  SeaImpOps.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/sea/Dialect/SeaImp

  DEPENDS
  MLIRSeaImpOpsIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRMemorySlotInterfaces
  MLIRSideEffectInterfaces
  MLIRSea
  )
