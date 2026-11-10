# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_mlir_library(SeaInterp
  Fuzz.cpp
  FuzzIntrospect.cpp
  Interpreter.cpp
  ParseInput.cpp
  Print.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/sea/Interp

  LINK_LIBS PUBLIC
  MLIRSea
  MLIRIR
  MLIRAsmParser
  MLIRBytecodeReader
  MLIRParser
  SeaSupport
  )
