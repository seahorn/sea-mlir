# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_mlir_library(SeaVC
  Names.cpp
  OpSem.cpp
  Replay.cpp
  SymStore.cpp
  VCGen.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/sea/VC

  LINK_LIBS PUBLIC
  MLIRSea
  MLIRIR
  SeaExpr
  SeaInterp
  )
