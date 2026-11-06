# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# The patterns and passes (no MLIRReduce dependency: sea-opt links them).
add_mlir_library(SeaReduce
  ReductionPatterns.cpp
  PARTIAL_SOURCES_INTENDED

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/sea/Reduce

  LINK_LIBS PUBLIC
  MLIRSea
  MLIRIR
  MLIRPass
  MLIRRewrite
  MLIRTransformUtils
  )

# The DialectReductionPatternInterface of the sea dialect (sea-reduce).
add_mlir_library(SeaReduceInterface
  ReductionInterface.cpp
  SeaReductionTree.cpp
  PARTIAL_SOURCES_INTENDED

  LINK_LIBS PUBLIC
  SeaReduce
  MLIRReduce
  )
