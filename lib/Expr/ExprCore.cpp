// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ExprCore.cpp - out-of-line parts of the Expr core ------------------===//
#include "sea/Expr/ExprCore.hh"

#include <cstdio>
#include <cstdlib>

namespace expr {

[[noreturn]] __attribute__((noinline, cold)) void
fatalCounter(const char *kind, const void *factory, const void *node,
             unsigned long long value, unsigned long long limit) {
  std::fprintf(stderr,
               "error: [%s] Expr counter invariant broken: factory=%p "
               "node=%p value=%llu limit=%llu\n",
               kind, factory, node, value, limit);
  std::fflush(stderr);
  std::abort();
}

} // namespace expr
