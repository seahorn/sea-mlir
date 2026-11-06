// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ExprCoreTest.cpp - counter invariants of the Expr core -------------===//
//
// Plain-main driver (no gtest), run by test/Expr/expr-core.test. Without an
// argument: the boundary cases that must work (the last allowed reference
// count and id, ordinary final release, duplicate candidates, id reset on
// recycling); prints `test: <name> ok` lines, exits non-zero on a failed
// check. With one argument, a crash mode that must abort at its cause
// (lit: `not --crash`): refcount-overflow, refcount-underflow, id-exhausted.
// Counters are set near their limits through ExprCoreTestAccess (never by
// building billions of nodes).
//
//===----------------------------------------------------------------------===//

#include "sea/Expr/Expr.hh"
#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <limits>
#include <string>

namespace expr {
struct ExprCoreTestAccess {
  static unsigned &count(ENode *n) { return n->count; }
  static unsigned &idCount(ExprFactory &f) { return f.idCount; }
  static size_t freeListSize(ExprFactory &f) { return f.freeList.size(); }
  static ENode *freeListBack(ExprFactory &f) { return f.freeList.back(); }
  static unsigned id(ENode *n) { return n->id; }
};
} // namespace expr

using namespace expr;
using A = ExprCoreTestAccess;

namespace {
int failures = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      llvm::errs() << "FAIL: " << msg << " [" #cond "] (line " << __LINE__     \
                   << ")\n";                                                   \
    }                                                                          \
  } while (0)

constexpr unsigned kMax = std::numeric_limits<unsigned>::max();

Expr var(ExprFactory &f, const char *n) {
  return op::bv::bvConst(mkTerm<std::string>(n, f), 8);
}

void testLimits() {
  ExprFactory f;
  Expr x = var(f, "x");
  // the last allowed count: max - 1 -> max works; back down by releases
  unsigned saved = A::count(x.get());
  A::count(x.get()) = kMax - 1;
  x->Ref();
  CHECK(A::count(x.get()) == kMax, "count reaches max");
  A::count(x.get()) = saved;
  // the last allowed id (one new node: a string terminal)
  A::idCount(f) = kMax - 1;
  Expr y = mkTerm<std::string>("y-last-id", f);
  CHECK(y->getId() == kMax, "the last id is max: " << y->getId());
  llvm::outs() << "test: limits ok\n";
}

void testLifecycle() {
  ExprFactory f;
  // duplicate candidates: the same term twice is one node (the second
  // candidate is disposed of, its children released)
  Expr x = var(f, "x");
  const unsigned x0 = A::count(x.get());
  Expr a = mk<BADD>(x, x);
  CHECK(A::count(x.get()) == x0 + 2,
        "two child edges of the new node: " << A::count(x.get()) - x0);
  Expr b = mk<BADD>(x, x);
  CHECK(a == b, "duplicate candidate canonized");
  CHECK(A::count(x.get()) == x0 + 2,
        "the duplicate candidate released its child edges: "
            << A::count(x.get()) - x0);
  // ordinary final release: the node goes to the free list with id 0
  size_t before = A::freeListSize(f);
  unsigned aid = a->getId();
  CHECK(aid != 0, "a canonical node has an id");
  a.reset();
  b.reset();
  CHECK(A::freeListSize(f) == before + 1, "released node recycled");
  CHECK(A::count(x.get()) == x0, "child edges released with the node");
  CHECK(A::id(A::freeListBack(f)) == 0, "recycled node id reset to 0");
  // reuse: a new node from the free list gets a new id
  Expr c = mk<BMUL>(x, x);
  CHECK(c->getId() > aid, "reused storage gets a new id: " << c->getId());
  llvm::outs() << "test: lifecycle ok\n";
}

/// A terminal value that counts its live copies (to check that operators
/// are destroyed, not only their storage freed).
struct Counted {
  static long live;
  int v;
  explicit Counted(int x) : v(x) { ++live; }
  Counted(const Counted &o) : v(o.v) { ++live; }
  ~Counted() { --live; }
};
long Counted::live = 0;
struct CountedTrait {
  static void print(std::ostream &OS, const Counted &c, int, bool) {
    OS << "counted" << c.v;
  }
  static bool less(const Counted &a, const Counted &b) { return a.v < b.v; }
  static bool equal_to(const Counted &a, const Counted &b) {
    return a.v == b.v;
  }
  static size_t hash(const Counted &c) { return std::hash<int>()(c.v); }
  static TerminalKind getKind() { return TerminalKind::UINT; }
  static std::string name() { return "test::Counted"; }
  static Expr inferType(Expr exp, TypeChecker &) { return Expr(); }
};
using CountedOp = Terminal<Counted, CountedTrait>;

void testReclamation() {
  {
    ExprFactory f;
    long base = Counted::live;
    {
      std::vector<Expr> keep;
      // more nodes than the free list holds (4096): some are destroyed
      // with a full list, the rest recycled
      for (int i = 0; i < 10000; ++i)
        keep.push_back(f.mkTerm(CountedOp(Counted(i))));
      CHECK(Counted::live == base + 10000, "one operator per node");
      // a duplicate candidate's operator is destroyed too
      Expr d = f.mkTerm(CountedOp(Counted(7)));
      CHECK(d == keep[7] && Counted::live == base + 10000,
            "duplicate candidate operator destroyed: " << Counted::live);
    }
    CHECK(Counted::live == base, "operators destroyed on release: "
                                     << Counted::live - base << " left");
    CHECK(A::freeListSize(f) <= 4096, "free list bounded");
  }
  // a deep chain (2e5 levels): its release is iterative (no recursion per
  // level), and a list that fills up during the release destroys the rest
  {
    ExprFactory f;
    Expr x = var(f, "x");
    const unsigned x0 = A::count(x.get());
    Expr e = x;
    for (int i = 0; i < 200000; ++i)
      e = mk<BADD>(e, x);
    CHECK(A::count(x.get()) == x0 + 200001,
          "one edge to x per level (two at the first, x + x)");
    e.reset();
    CHECK(A::freeListSize(f) == 4096,
          "free list full after the release: " << A::freeListSize(f));
    // every level released its edge to the shared leaf, including the
    // levels disposed of after the free list filled up
    CHECK(A::count(x.get()) == x0,
          "shared leaf count restored: " << A::count(x.get()) << " vs " << x0);
    Expr again = mk<BADD>(x, x); // storage reused
    CHECK(again->getId() != 0, "allocation after the release");
  }
  llvm::outs() << "test: reclamation ok\n";
}

int crash(const char *mode) {
  ExprFactory f;
  if (!std::strcmp(mode, "refcount-overflow")) {
    Expr x = var(f, "x");
    A::count(x.get()) = kMax;
    x->Ref(); // must abort
  } else if (!std::strcmp(mode, "refcount-underflow")) {
    // a lost reference: the count says 0 while a release comes
    Expr x = var(f, "x");
    ENode *p = x.get();
    A::count(p) = 0;
    f.Deref(p); // must abort
  } else if (!std::strcmp(mode, "id-exhausted")) {
    A::idCount(f) = kMax;
    Expr y = mkTerm<std::string>("y", f); // must abort
  }
  llvm::errs() << "no crash in mode " << mode << "\n";
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  if (argc > 1)
    return crash(argv[1]);
  testLimits();
  testLifecycle();
  testReclamation();
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
