// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ExprCoreTest.cpp - counter invariants of the Expr core -------------===//
//
// Plain-main driver (no gtest), run by test/Expr/expr-core.test. Without an
// argument: the boundary cases that must work (the last allowed reference
// count and id, ordinary final release, duplicate terms, id reset on
// recycling, lookup of an existing term before building one); prints `test:
// <name> ok` lines, exits non-zero on a failed check. With one argument, a
// crash mode that must abort at its cause (lit: `not --crash`):
// refcount-overflow, refcount-underflow, id-exhausted. Counters are set near
// their limits through ExprCoreTestAccess (never by building billions of
// nodes).
//
//===----------------------------------------------------------------------===//

#include "sea/Expr/Expr.hh"
#include "sea/Expr/ScopedMarks.hh"
#include <boost/functional/hash.hpp>

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace expr {
struct ExprCoreTestAccess {
  static unsigned &count(ENode *n) { return n->count; }
  static unsigned &idCount(ExprFactory &f) { return f.idCount; }
  static size_t freeListSize(ExprFactory &f) { return f.freeList.size(); }
  static ENode *freeListBack(ExprFactory &f) { return f.freeList.back(); }
  static unsigned id(ENode *n) { return n->id; }
  static uint64_t marks(ENode *n) { return n->marks; }
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
  // duplicate terms: the same term twice is one node, and the second mk
  // leaves the children's counts as they were
  Expr x = var(f, "x");
  const unsigned x0 = A::count(x.get());
  Expr a = mk<BADD>(x, x);
  CHECK(A::count(x.get()) == x0 + 2,
        "two child edges of the new node: " << A::count(x.get()) - x0);
  Expr b = mk<BADD>(x, x);
  CHECK(a == b, "duplicate term is the same node");
  CHECK(A::count(x.get()) == x0 + 2,
        "the duplicate term holds no child edges of its own: "
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
  static long copies;
  int v;
  explicit Counted(int x) : v(x) { ++live; }
  Counted(const Counted &o) : v(o.v) {
    ++live;
    ++copies;
  }
  ~Counted() { --live; }
};
long Counted::live = 0;
long Counted::copies = 0;
struct CountedTrait {
  static void print(std::ostream &OS, const Counted &c, int, bool) {
    OS << "counted" << c.v;
  }
  static bool less(const Counted &a, const Counted &b) { return a.v < b.v; }
  static bool equal_to(const Counted &a, const Counted &b) {
    return a.v == b.v;
  }
  static size_t hash(const Counted &c) { return std::hash<int>()(c.v); }
  static TerminalKind getKind() { return TerminalKind::TEST_ONLY; }
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
      // a duplicate term leaves no operator copy behind
      Expr d = f.mkTerm(CountedOp(Counted(7)));
      CHECK(d == keep[7] && Counted::live == base + 10000,
            "duplicate term operator not kept: " << Counted::live);
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

/// A term that exists already is found before anything is built: no
/// operator clone (a copy of the terminal value), no new id. An argument
/// range that yields fresh terms by value keeps them alive until the node
/// holds them.
void testLookup() {
  ExprFactory f;
  CountedOp o(Counted(7));
  Expr t = f.mkTerm(o);
  const long c0 = Counted::copies;
  const unsigned made = f.nodesMade();
  Expr u = f.mkTerm(o);
  CHECK(u == t, "the existing node");
  CHECK(Counted::copies == c0,
        "no operator clone on a hit: " << Counted::copies - c0 << " copies");
  CHECK(f.nodesMade() == made, "no id on a hit");
  Expr x = var(f, "x");
  Expr s = mk<BADD>(x, x);
  const unsigned x0 = A::count(x.get());
  CHECK(mk<BADD>(x, x) == s && A::count(x.get()) == x0,
        "a hit leaves the argument counts");

  std::vector<std::string> names = {"p", "q"};
  auto fresh = llvm::map_range(
      names, [&f](const std::string &n) { return var(f, n.c_str()); });
  Expr sum = mknary<BADD>(fresh);
  CHECK(sum->arity() == 2, "two arguments");
  CHECK(sum->arg(0) == var(f, "p").get() && sum->arg(1) == var(f, "q").get(),
        "the arguments made by the range are the node's");
  CHECK(A::id(sum->arg(0)) != 0 && A::count(sum->arg(0)) == 1,
        "an argument made by the range is held by the node only");
  llvm::outs() << "test: lookup ok\n";
}

/// The watch of node creation (ExprFactory::setWatch): every step nodes
/// made, not on a lookup hit; it reads the live nodes; none after removal.
void testWatch() {
  ExprFactory f;
  struct W : ExprFactory::Watch {
    unsigned calls = 0;
    size_t live = 0, census = 0;
    void onNodes(ExprFactory &fac) override {
      ++calls;
      live = fac.nodesLive();
      census = 0;
      fac.forEachLive([&](const ENode *) { ++census; });
    }
  } w;
  std::vector<Expr> keep;
  keep.push_back(var(f, "x"));
  const unsigned made0 = f.nodesMade();
  f.setWatch(&w, 3);
  for (int i = 0; i < 4; ++i) // 4 new nodes each: a string, its bv const
    keep.push_back(var(f, ("v" + std::to_string(i)).c_str()));
  const unsigned made = f.nodesMade() - made0;
  CHECK(w.calls == made / 3,
        "one call per 3 nodes made: " << w.calls << " of " << made);
  CHECK(w.live == w.census && w.live > 0,
        "the census sees the live nodes: " << w.live << " " << w.census);
  const unsigned c0 = w.calls;
  for (int i = 0; i < 10; ++i)
    (void)var(f, "x"); // lookup hits: nothing made
  CHECK(w.calls == c0, "no call on a lookup hit");
  f.setWatch(nullptr, 0);
  for (int i = 0; i < 10; ++i)
    keep.push_back(var(f, ("u" + std::to_string(i)).c_str()));
  CHECK(w.calls == c0, "no call after removal");
  CHECK(f.nodesLive() > 0, "live count without a watch");
  llvm::outs() << "test: watch ok\n";
}

/// Scoped marks (ScopedMarks.hh): nested scopes hold distinct bits and
/// see the same nodes independently; clear keeps the lease; a scope keeps
/// its nodes alive and leaves no bit behind; recycled nodes start unmarked;
/// the two-bit use count.
void testMarks() {
  static_assert(sizeof(ENode) <= ExprFactoryAllocator::kSmallChunk);
  ExprFactory f;
  Expr x = var(f, "x"), y = var(f, "y");
  Expr s = mk<BADD>(x, y);
  {
    ScopedExprSet outer(f, "outer");
    CHECK(outer.insert(x) && !outer.insert(x), "first insertion only");
    CHECK(outer.contains(x) && !outer.contains(y), "membership");
    {
      ScopedExprSet inner(f, "inner");
      CHECK(f.marksLeased() == 3,
            "two live leases, distinct bits: " << f.marksLeased());
      CHECK(!inner.contains(x), "a nested scope does not see the outer");
      CHECK(inner.insert(x) && inner.insert(y), "the nested scope's own");
      inner.clear();
      CHECK(!inner.contains(x) && !inner.contains(y) && inner.empty(),
            "clear empties the set");
      CHECK(outer.contains(x), "clear of a nested scope keeps the outer's");
      CHECK(f.marksLeased() == 3, "clear keeps the lease");
      CHECK(inner.insert(y) && inner.contains(y), "reuse after clear");
    }
    CHECK(f.marksLeased() == 1, "the nested lease is released");
    CHECK(A::marks(y.get()) == 0, "the nested scope's bits are cleared");
    CHECK(outer.contains(x) && outer.size() == 1, "the outer set intact");
    // a scope keeps a node alive: the set holds the only reference
    ENode *raw = s.get();
    outer.insert(s);
    const size_t free0 = A::freeListSize(f);
    s.reset();
    CHECK(A::freeListSize(f) == free0 && outer.contains(raw),
          "a marked node stays alive");
  }
  CHECK(f.marksLeased() == 0, "every lease released");
  CHECK(A::marks(x.get()) == 0, "no bit left on a node");
  // the scope dropped the last reference of x + y: recycled storage starts
  // unmarked (a stale bit would make the new node a member of a new scope)
  CHECK(A::freeListSize(f) > 0 && A::marks(A::freeListBack(f)) == 0,
        "a freed node has no mark");
  {
    ScopedExprSet set(f, "recycle");
    ENode *slot = A::freeListBack(f);
    Expr m = mk<BMUL>(x, y);
    CHECK(m.get() == slot, "the recycled storage");
    CHECK(A::marks(m.get()) == 0 && !set.contains(m) && set.insert(m),
          "a recycled node starts with no marks");
  }
  // a lease per live scope, the lowest free bits; a released bit is reused
  {
    ScopedExprSet a(f, "a");
    {
      ScopedExprSet b(f, "b");
      ScopedExprSet c(f, "c");
      CHECK(f.marksLeased() == 7 && !std::strcmp(f.markOwner(1), "b"),
            "three bits");
    }
    ScopedUseCount u(f, "u");
    CHECK(f.marksLeased() == 7 && !std::strcmp(f.markOwner(2), "u"),
          "the two-bit lease reuses the freed bits");
  }
  // the two-bit counter: 0, 1, more than one
  {
    ScopedUseCount uses(f, "uses");
    ScopedExprSet other(f, "other");
    CHECK(uses.count(x) == 0 && !uses.shared(x), "unseen");
    CHECK(uses.add(x) && uses.count(x) == 1 && !uses.shared(x), "once");
    CHECK(!uses.add(x) && uses.count(x) == 2 && uses.shared(x), "twice");
    uses.add(x);
    CHECK(uses.count(x) == 2, "saturates at 2");
    CHECK(!other.contains(x), "a use count is not another scope's set");
    uses.add(y);
    CHECK(uses.count(y) == 1 && uses.size() == 2, "one log entry per node");
    uses.clear();
    CHECK(uses.count(x) == 0 && uses.count(y) == 0 && A::marks(x.get()) == 0,
          "clear forgets both bits");
    CHECK(uses.add(y) && uses.count(y) == 1, "reuse after clear");
  }
  CHECK(f.marksLeased() == 0 && A::marks(y.get()) == 0, "all released");
  // dagSize (a scope of its own): distinct nodes, nested in a live scope
  {
    ScopedExprSet live(f, "live");
    live.insert(x);
    Expr t = mk<BADD>(mk<BMUL>(x, y), mk<BMUL>(x, y), x);
    // the reference: a std::set of the reachable nodes
    std::set<ENode *> reach;
    std::vector<ENode *> todo{t.get()};
    while (!todo.empty()) {
      ENode *n = todo.back();
      todo.pop_back();
      if (reach.insert(n).second)
        todo.insert(todo.end(), n->args_begin(), n->args_end());
    }
    CHECK(dagSize(t) == reach.size(),
          "distinct nodes: " << dagSize(t) << " vs " << reach.size());
    CHECK(dagSize(t) == dagSize(ExprVector{t, x}),
          "a root met twice counts once");
    CHECK(dagSize(Expr()) == 0 && dagSize(ExprVector{Expr(), x}) == dagSize(x),
          "nulls skipped");
    CHECK(live.contains(x) && live.size() == 1, "the walk's own bit");
    // termForLog: a walk of at most 65 nodes decides printed or elided
    Expr chain = x;
    for (int i = 0; i < 100; ++i)
      chain = mk<BADD>(chain, var(f, ("c" + std::to_string(i)).c_str()));
    CHECK(termForLog(chain) ==
              "#" + std::to_string(chain->getId()) + " (> 64 nodes)",
          "a large term elided: " << termForLog(chain));
    CHECK(termForLog(t).find('#') != 0, "a small term printed");
  }
  llvm::outs() << "test: marks ok\n";
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
  } else if (!std::strcmp(mode, "marks-exhausted")) {
    // 63 live scopes of one kind (a leak) and one more pair: no bits left
    std::vector<std::unique_ptr<ScopedExprSet>> leak;
    for (int i = 0; i < 63; ++i)
      leak.push_back(std::make_unique<ScopedExprSet>(f, "leaked-walk"));
    ScopedUseCount u(f, "use-count"); // must abort
  } else if (!std::strcmp(mode, "marks-factory")) {
    ExprFactory g;
    Expr x = var(g, "x");
    ScopedExprSet s(f, "s");
    s.insert(x); // must abort
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
  testLookup();
  testWatch();
  testMarks();
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
