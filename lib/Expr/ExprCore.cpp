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

[[noreturn]] __attribute__((noinline, cold)) void
fatalMarks(const char *kind, const ExprFactory *factory,
           const ExprFactory *other, const char *owner, unsigned want) {
  if (other)
    std::fprintf(stderr,
                 "error: [%s] a node of factory %p given to a mark scope of "
                 "factory %p\n",
                 kind, static_cast<const void *>(other),
                 static_cast<const void *>(factory));
  else
    std::fprintf(stderr,
                 "error: [%s] no free Expr mark bit: scope '%s' needs %u, "
                 "factory=%p has %d live leases (a leaked scope or runaway "
                 "nesting; there is no fallback):\n",
                 kind, owner ? owner : "?", want,
                 static_cast<const void *>(factory),
                 __builtin_popcountll(factory->marksLeased()));
  for (unsigned i = 0; i < 64; ++i)
    if (factory->marksLeased() >> i & 1)
      std::fprintf(stderr, "  lease: bit=%u owner=%s\n", i,
                   factory->markOwner(i) ? factory->markOwner(i) : "?");
  std::fflush(stderr);
  std::abort();
}

} // namespace expr
