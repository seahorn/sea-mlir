// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- FoldTopEqsTest.cpp - tests of the concat-ite hoist -----------------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if a
// check fails. Run by test/VC/fold-top-eqs/hoist.test.
//
// What is tested (hoistConcatItes, include/sea/VC/FoldTopEqs.h,
// docs/design/FoldTopEqs.md "The concat-ite hoist"):
//  - shared: a concat of two bytes whose ite chains share their arms (a DAG
//    with 2^n paths and 4n ites); the hoist makes 2n - 1 ites, once each,
//    and the result is not larger than the input;
//  - leaves: a concat whose hoisted leaves are not concats of numerals is
//    refused by the numeral mode (counted) and hoisted by the all mode;
//  - budget: interleaved chains whose hoist would make more ites than the
//    parts hold: refused by the numeral mode, hoisted by the all mode;
//  - refused-memo: a top concat refused for budget leaves no memo entries,
//    so a hoistable concat among its arms is counted in either conjunct order;
//  - every hoist is checked equal to its input by z3;
//  - ptr-facts: a fact whose term (or concat part) derives from a pointer's
//    address outside a memory read is not substituted (addressDerived,
//    "Pointer observations"): an address, an extract of one, a sum with
//    one, a pointer difference, a concat part that is one; a read at a
//    pointer index is (its value is memory contents).
//
//===----------------------------------------------------------------------===//

#include "sea/VC/FoldTopEqs.h"

#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprOpArray.hh"
#include "sea/Expr/ExprOpBind.hh"
#include "sea/Expr/ExprOpBv.hh"
#include "sea/Expr/ExprOpPtr.hh"
#include "sea/Expr/ExprOpSort.hh"
#include "sea/Expr/Smt/Z3SolverImpl.hh"

#include "llvm/Support/raw_ostream.h"

#include <set>
#include <string>
#include <vector>

using namespace expr;
using sea::ConcatIte;
using sea::solver::SolverResult;

namespace {
int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      llvm::outs() << "FAIL: " << msg << " (" #cond ")\n";                     \
    }                                                                          \
  } while (0)

struct Env {
  ExprFactory efac;
  Expr cond(const std::string &n) {
    return bind::boolConst(mkTerm<std::string>(n, efac));
  }
  Expr byte(const std::string &n) {
    return op::bv::bvConst(mkTerm<std::string>(n, efac), 8);
  }
  Expr num(long v) { return op::bv::bvnum(mpz_class(v), 8, efac); }
  Expr concat(Expr a, Expr b) { return mk<BCONCAT>(a, b); }
};

/// z3: a != b is unsat
bool equal(Env &env, const Expr &a, const Expr &b) {
  sea::solver::z3_solver_impl s(env.efac);
  s.add(mk<NEQ>(a, b));
  return s.check() == SolverResult::UNSAT;
}

/// the hoist of \p t under \p mode, its counters in \p st
Expr hoist(const Expr &t, ConcatIte mode, sea::FoldTopEqsStats &st) {
  st = sea::FoldTopEqsStats();
  return sea::hoistConcatItes(t, mode, st);
}

/// two bytes; at level k, A_k = ite(c_k, A_k+1, B_k+1) and
/// B_k = ite(c_k, B_k+1, A_k+1), numerals at level n. concat(A_0, A'_0) has
/// 2^n paths; the distinct concats of the hoist are (A_k, A'_k) and
/// (B_k, B'_k): 2n - 1 ites.
void testShared() {
  const unsigned n = 24;
  Env env;
  std::vector<Expr> a(2), b(2);
  for (unsigned j = 0; j < 2; ++j) {
    a[j] = env.num(2 * j + 1);
    b[j] = env.num(2 * j + 2);
  }
  for (unsigned k = n; k-- > 0;) {
    Expr c = env.cond("c" + std::to_string(k));
    for (unsigned j = 0; j < 2; ++j) {
      Expr ak = mk<ITE>(c, a[j], b[j]);
      Expr bk = mk<ITE>(c, b[j], a[j]);
      a[j] = ak;
      b[j] = bk;
    }
  }
  Expr t = env.concat(a[0], a[1]);
  sea::FoldTopEqsStats st;
  for (ConcatIte mode : {ConcatIte::Numeral, ConcatIte::All}) {
    Expr h = hoist(t, mode, st);
    llvm::outs() << "shared: mode="
                 << (mode == ConcatIte::All ? "all" : "numeral")
                 << " concat-ite=" << st.hoisted
                 << " refused=" << st.hoistRefused << " dag-in=" << dagSize(t)
                 << " dag-out=" << dagSize(h) << "\n";
    CHECK(isOpX<ITE>(h), "shared: hoisted");
    CHECK(st.hoisted == 2 * n - 1, "shared: each distinct concat once");
    CHECK(dagSize(h) <= dagSize(t), "shared: no growth");
  }
  // a small instance to z3 (the large one has 2^24 paths, fine for z3 too,
  // but slower than this test needs)
  Env small;
  Expr c0 = small.cond("c0"), c1 = small.cond("c1");
  Expr a1 = mk<ITE>(c1, small.num(1), small.num(2));
  Expr b1 = mk<ITE>(c1, small.num(2), small.num(1));
  Expr a1p = mk<ITE>(c1, small.num(3), small.num(4));
  Expr b1p = mk<ITE>(c1, small.num(4), small.num(3));
  Expr s = small.concat(mk<ITE>(c0, a1, b1), mk<ITE>(c0, a1p, b1p));
  Expr hs = hoist(s, ConcatIte::Numeral, st);
  CHECK(st.hoisted == 3, "shared small: three ites");
  CHECK(equal(small, s, hs), "shared small: exact");
}

/// concat(ite(c, x, 1), ite(c, y, 2)): the then-leaf concat(x, y) is not
/// numerals
void testLeaves() {
  Env env;
  Expr c = env.cond("c");
  Expr t = env.concat(mk<ITE>(c, env.byte("x"), env.num(1)),
                      mk<ITE>(c, env.byte("y"), env.num(2)));
  sea::FoldTopEqsStats st;
  Expr h = hoist(t, ConcatIte::Numeral, st);
  llvm::outs() << "leaves: numeral concat-ite=" << st.hoisted
               << " refused=" << st.hoistRefused << "\n";
  CHECK(h == t, "leaves: numeral keeps the concat");
  CHECK(st.hoisted == 0 && st.hoistRefused == 1, "leaves: numeral refuses");
  h = hoist(t, ConcatIte::All, st);
  llvm::outs() << "leaves: all concat-ite=" << st.hoisted
               << " refused=" << st.hoistRefused << "\n";
  CHECK(isOpX<ITE>(h) && st.hoisted == 1, "leaves: all hoists");
  CHECK(equal(env, t, h), "leaves: exact");
  // concat(x, ite(c, 1, 2)): the hoist does not apply; nothing counted
  Expr u = env.concat(env.byte("x"), mk<ITE>(c, env.num(1), env.num(2)));
  h = hoist(u, ConcatIte::Numeral, st);
  CHECK(h == u && st.hoistRefused == 0, "leaves: not applicable, not counted");
}

/// distinct ite nodes along the ite arms from \p roots
size_t spineItes(std::vector<ENode *> stack) {
  std::set<ENode *> seen;
  while (!stack.empty()) {
    ENode *n = stack.back();
    stack.pop_back();
    if (!isOpX<ITE>(n) || !seen.insert(n).second)
      continue;
    stack.push_back(n->arg(1));
    stack.push_back(n->arg(2));
  }
  return seen.size();
}

/// four nodes per level and byte, the arms mapped differently in the two
/// bytes (the maps from a search): the pairs of nodes the hoist meets
/// outnumber the ites the two bytes hold
void testBudget() {
  const unsigned n = 8, w = 4;
  const unsigned map[4][4] = {
      {1, 3, 2, 0}, {1, 0, 0, 2}, {3, 0, 1, 2}, {2, 1, 0, 2}};
  Env env;
  std::vector<std::vector<Expr>> lv(2, std::vector<Expr>(w));
  for (unsigned j = 0; j < 2; ++j)
    for (unsigned i = 0; i < w; ++i)
      lv[j][i] = env.num(10 * j + i);
  for (unsigned k = n; k-- > 0;) {
    Expr c = env.cond("c" + std::to_string(k));
    std::vector<std::vector<Expr>> next(2, std::vector<Expr>(w));
    for (unsigned j = 0; j < 2; ++j)
      for (unsigned i = 0; i < w; ++i)
        next[j][i] = mk<ITE>(c, lv[j][map[2 * j][i]], lv[j][map[2 * j + 1][i]]);
    lv = next;
  }
  Expr t = env.concat(lv[0][0], lv[1][0]);
  size_t parts = spineItes({t->arg(0), t->arg(1)});
  sea::FoldTopEqsStats st;
  Expr h = hoist(t, ConcatIte::Numeral, st);
  llvm::outs() << "budget: numeral concat-ite=" << st.hoisted
               << " refused=" << st.hoistRefused << "\n";
  CHECK(h == t && st.hoistRefused == 1, "budget: numeral refuses growth");
  h = hoist(t, ConcatIte::All, st);
  llvm::outs() << "budget: all concat-ite=" << st.hoisted
               << " parts-ites=" << parts << "\n";
  CHECK(st.hoisted > parts, "budget: all makes more ites than the parts");
  CHECK(equal(env, t, h), "budget: exact");
}
/// the two bytes of testBudget (the numeral mode refuses their concat), with
/// condition names prefixed by \p pfx
std::pair<Expr, Expr> budgetBytes(Env &env, const std::string &pfx) {
  const unsigned n = 8, w = 4;
  const unsigned map[4][4] = {
      {1, 3, 2, 0}, {1, 0, 0, 2}, {3, 0, 1, 2}, {2, 1, 0, 2}};
  std::vector<std::vector<Expr>> lv(2, std::vector<Expr>(w));
  for (unsigned j = 0; j < 2; ++j)
    for (unsigned i = 0; i < w; ++i)
      lv[j][i] = env.num(10 * j + i);
  for (unsigned k = n; k-- > 0;) {
    Expr c = env.cond(pfx + std::to_string(k));
    std::vector<std::vector<Expr>> next(2, std::vector<Expr>(w));
    for (unsigned j = 0; j < 2; ++j)
      for (unsigned i = 0; i < w; ++i)
        next[j][i] = mk<ITE>(c, lv[j][map[2 * j][i]], lv[j][map[2 * j + 1][i]]);
    lv = next;
  }
  return {lv[0][0], lv[1][0]};
}

/// T1 = concat(ite(c, P, Q), ite(c, P', Q')) is refused for budget; T2 =
/// concat(P, P') is T1's then-arm concat, hoistable on its own. A refusal
/// leaves its tried concats uncounted, so T2 is counted as the one ite made
/// in either order of the conjuncts.
void testRefusedMemo() {
  Env env;
  Expr c = env.cond("top"), d = env.cond("d");
  Expr p = mk<ITE>(d, env.num(1), env.num(2));
  Expr pp = mk<ITE>(d, env.num(3), env.num(4));
  auto [q, qp] = budgetBytes(env, "q");
  Expr t1 = env.concat(mk<ITE>(c, p, q), mk<ITE>(c, pp, qp));
  Expr t2 = env.concat(p, pp);
  Expr k = op::bv::bvnum(mpz_class(7L), 16, env.efac);
  sea::FoldTopEqsStats st;
  for (int order = 0; order < 2; ++order) {
    Expr e1 = mk<EQ>(t1, k), e2 = mk<EQ>(t2, k);
    Expr in = order ? mk<AND>(e1, e2) : mk<AND>(e2, e1);
    Expr h = hoist(in, ConcatIte::Numeral, st);
    llvm::outs() << "refused-memo: order " << order
                 << " concat-ite=" << st.hoisted
                 << " refused=" << st.hoistRefused << "\n";
    CHECK(st.hoisted == 1 && st.hoistRefused == 1,
          "refused-memo: counts independent of the order");
    CHECK(h != in, "refused-memo: the hoisted conjunct is kept");
    CHECK(equal(env, in, h), "refused-memo: exact");
  }
}
} // namespace

/// does the fold of [t = c, use(t)] replace t in use(t)? \p skipped: the
/// skipped-ptr count
bool folds(const Expr &t, const Expr &c, uint64_t &skipped) {
  sea::TopEqFolder f([](Expr e) { return e; });
  Expr use = mk<BULT>(t, c);
  Expr q = f.fold(mk<AND>(mk<EQ>(t, c), use));
  skipped = f.stats().skippedPtr;
  return q->arity() == 2 && q->arg(1) != use;
}

void testPtrFacts() {
  ExprFactory efac;
  auto name = [&](const char *n) { return mkTerm<std::string>(n, efac); };
  Expr p = op::ptr::ptrConst(name("p"), 32),
       q = op::ptr::ptrConst(name("q"), 32);
  Expr x = op::bv::bvConst(name("x"), 32);
  auto n32 = [&](long v) { return op::bv::bvnum(mpz_class(v), 32, efac); };
  auto n8 = [&](long v) { return op::bv::bvnum(mpz_class(v), 8, efac); };
  Expr ap = mk<ADDR>(p);
  uint64_t sk = 0;
  struct Case {
    const char *name;
    Expr t, c;
    bool expect;
  };
  Expr mem = bind::mkConst(name("m"), sort::arrayTy(op::ptr::ptrsort(32, efac),
                                                    op::bv::bvsort(8, efac)));
  std::vector<Case> cases = {
      {"addr", ap, n32(64), false},
      {"extract", op::bv::extract(7, 0, ap), n8(64), false},
      {"sum", mk<BADD>(mk<PTR_TO_INT>(p), x), n32(64), false},
      {"diff", mk<PTR_DIFF>(p, q), n32(64), false},
      {"read", op::array::select(mem, p), n8(7), true},
      {"value", x, n32(64), true},
  };
  for (const Case &k : cases) {
    bool r = folds(k.t, k.c, sk);
    llvm::outs() << "ptr-facts: " << k.name << " folded=" << r
                 << " skipped-ptr=" << sk << "\n";
    CHECK(r == k.expect, std::string("ptr-facts: ") + k.name);
  }
  // a concat part: concat(x, addr(p)) = c fixes x and addr(p); x is
  // substituted, addr(p) is not (nor the concat)
  Expr t = mk<BCONCAT>(x, ap);
  sea::TopEqFolder f([](Expr e) { return e; });
  Expr ux = mk<BULT>(x, n32(3)), ua = mk<BULT>(ap, n32(3));
  Expr r = f.fold(
      mk<AND>(mk<EQ>(t, op::bv::bvnum(mpz_class(5L), 64, efac)), ux, ua));
  bool xFolded = r->arity() == 3 && r->arg(1) != ux;
  bool aFolded = r->arity() == 3 && r->arg(2) != ua;
  llvm::outs() << "ptr-facts: concat-part x-folded=" << xFolded
               << " addr-folded=" << aFolded
               << " skipped-ptr=" << f.stats().skippedPtr << "\n";
  CHECK(xFolded && !aFolded, "ptr-facts: concat part");
}

int main() {
  testShared();
  llvm::outs() << "test: shared ok\n";
  testLeaves();
  llvm::outs() << "test: leaves ok\n";
  testBudget();
  llvm::outs() << "test: budget ok\n";
  testRefusedMemo();
  llvm::outs() << "test: refused-memo ok\n";
  testPtrFacts();
  llvm::outs() << "test: ptr-facts ok\n";
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
