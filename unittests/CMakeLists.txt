# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_subdirectory(ExprRewriter)
add_subdirectory(AddrRange)
add_subdirectory(Log)
add_subdirectory(Timer)
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
add_subdirectory(JoinCases)
add_subdirectory(IntRanges)
