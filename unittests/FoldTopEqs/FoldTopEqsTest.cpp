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
//  - every hoist is checked equal to its input by z3.
//
//===----------------------------------------------------------------------===//

#include "sea/VC/FoldTopEqs.h"

#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprOpBv.hh"
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
} // namespace

int main() {
  testShared();
  llvm::outs() << "test: shared ok\n";
  testLeaves();
  llvm::outs() << "test: leaves ok\n";
  testBudget();
  llvm::outs() << "test: budget ok\n";
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
