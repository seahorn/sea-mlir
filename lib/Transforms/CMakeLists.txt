# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_mlir_library(SeaTransforms
  ExpandIntrinsics.cpp
  ExpandMemSet.cpp
  Passes.cpp
  PassLog.cpp
  PlaceStatic.cpp
  ScalarizeStructs.cpp
  SplitCriticalEdges.cpp
  SplitAtChecks.cpp
  MergeAsserts.cpp
  SinkAssumes.cpp
  LowerPtrIndex.cpp
  CheckAlign.cpp
  CheckPtrIndex.cpp
  WidenMemory.cpp
  ApplyTracking.cpp
  LowerTracking.cpp
  LowerFatPointers.cpp
  DropFreshMemHavoc.cpp
  GuardNullMemHavoc.cpp
  SplitWideAccess.cpp
  WrapFlags.cpp
  PrintRefs.cpp
  TestInsertPrints.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/sea/Transforms

  LINK_LIBS PUBLIC
  MLIRSea
  MLIRIR
  MLIRPass
  MLIRTransforms
  SeaSupport
  )
