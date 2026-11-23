# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# test drivers stay out of bin/ (the tools); directory scope, so it covers the
# subdirectories and ends with this file
set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/test-bin)

add_subdirectory(ExprRewriter)
add_subdirectory(AddrRange)
add_subdirectory(Log)
add_subdirectory(Timer)
add_subdirectory(Stats)
add_subdirectory(SolverFactory)
# these check rules for soundness with Yices (lit feature `yices`)
if(SEA_WITH_YICES)
  add_subdirectory(SeaMRules)
  add_subdirectory(BvRules)
  add_subdirectory(StoreMap)
  add_subdirectory(MemOps)
  add_subdirectory(Ackermann)
endif()
add_subdirectory(ExprCore)
add_subdirectory(ExprApi)
add_subdirectory(SmallArith)
add_subdirectory(JoinCases)
add_subdirectory(IntRanges)
add_subdirectory(DiffDecider)
add_subdirectory(FoldTopEqs)
add_subdirectory(CanonicalSums)
