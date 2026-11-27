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
//  - ptr-facts: a fact whose term (or concat part) mentions an object (a
//    PTR_SEGMENT) outside a memory read is not substituted (mentionsObject,
//    "Pointer observations"): an object's address, an extract or a sum of
//    one, a difference with one, the address of a node over one, a concat
//    part that is one. The same over a pointer register is substituted, as
//    is a read at an object's address (its value is memory contents); the
//    testing switch turns the skip off.
//  - concat-kept: a concat fact skipped for an object keeps the parts it
//    defines in its own conjunct (top and nested), so the fold is
//    equivalent to its input (z3, the object's address as a free value)
//    and still substitutes the parts elsewhere.
//  - trail: Q_0 <=> trail /\ Q_n ("Step 1's trail"). A simplifier that rewrites
//    the defining conjunct of a fact into another form that still holds its
//    term (injected: `t = c` ~> `t <= c /\ t >= c`, no rule of the rewriter
//    is known to do this) leaves the fact without an owner in the next
//    round; the term is replaced there, in that form too. The fact is
//    re-added (re-added=1) and the fold is z3-equivalent to its input
//    (unsat stays unsat: x * x = 5 has no solution in 8 bits). A constant's
//    fact re-added is eliminated by step 2 into its model trail. A fact
//    whose term only left the query (its conjunct rewritten by another
//    fact) is not re-added (re-added=0). Two lost facts, a constant's
//    (x = 5) and one whose term holds it (x * x = 9): both are re-added,
//    and step 2 substitutes x in the other (unsat kept; the term's fact
//    without x's is sat). Also lost: b (b ~> !!b), !b (!b ~> b -> false),
//    a concat part; the round bound (2 rounds); and a concat on a part's
//    path holding another value (nothing lost, the conjunct false).
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
/// skipped-base count; \p baseSkip: the fold's switch (TopEqFolder)
bool folds(const Expr &t, const Expr &c, uint64_t &skipped,
           bool baseSkip = true) {
  sea::TopEqFolder f([](Expr e) { return e; }, sea::ConcatIte::Numeral, 64,
                     baseSkip);
  Expr use = mk<BULT>(t, c);
  Expr q = f.fold(mk<AND>(mk<EQ>(t, c), use));
  skipped = f.stats().skippedBase;
  return q->arity() == 2 && q->arg(1) != use;
}

/// docs/design/PtrSegment.md, "What F2 does then": a fact whose term
/// mentions an object (a PTR_SEGMENT outside a read) is skipped; one that
/// mentions none is substituted, pointer observations of registers (block
/// arguments, nondet pointers) included: no term loses an object by it
void testPtrFacts() {
  ExprFactory efac;
  auto name = [&](const char *n) { return mkTerm<std::string>(n, efac); };
  Expr p = op::ptr::ptrConst(name("p"), 32),
       q = op::ptr::ptrConst(name("q"), 32);
  auto n32 = [&](long v) { return op::bv::bvnum(mpz_class(v), 32, efac); };
  auto n8 = [&](long v) { return op::bv::bvnum(mpz_class(v), 8, efac); };
  Expr seg = op::ptr::segment("stack.1", n32(0x1000));
  Expr x = op::bv::bvConst(name("x"), 32);
  Expr ap = mk<ADDR>(p), as = mk<ADDR>(seg);
  Expr node = op::ptr::addressPlus(seg, x);
  uint64_t sk = 0;
  struct Case {
    const char *name;
    Expr t, c;
    bool expect;
  };
  Expr mem = bind::mkConst(name("m"), sort::arrayTy(op::ptr::ptrsort(32, efac),
                                                    op::bv::bvsort(8, efac)));
  std::vector<Case> cases = {
      // no object: substituted
      {"addr", ap, n32(64), true},
      {"extract", op::bv::extract(7, 0, ap), n8(64), true},
      {"sum", mk<BADD>(mk<PTR_TO_INT>(p), x), n32(64), true},
      {"diff", mk<PTR_DIFF>(p, q), n32(64), true},
      {"read", op::array::select(mem, p), n8(7), true},
      {"value", x, n32(64), true},
      // an object: skipped
      {"seg-addr", as, n32(0x1000), false},
      {"seg-extract", op::bv::extract(7, 0, as), n8(0), false},
      {"seg-sum", mk<BADD>(mk<PTR_TO_INT>(seg), x), n32(64), false},
      {"seg-diff", mk<PTR_DIFF>(p, seg), n32(64), false},
      {"seg-node", mk<ADDR>(node), n32(64), false},
      // a read at an object's address: its value, not the address
      {"seg-read", op::array::select(mem, node), n8(7), true},
  };
  for (const Case &k : cases) {
    bool r = folds(k.t, k.c, sk);
    llvm::outs() << "ptr-facts: " << k.name << " folded=" << r
                 << " skipped-base=" << sk << "\n";
    CHECK(r == k.expect, std::string("ptr-facts: ") + k.name);
    CHECK(sk == (k.expect ? 0u : 1u),
          std::string("ptr-facts: skipped-base of ") + k.name);
  }
  // the testing switch: no skip
  CHECK(folds(as, n32(0x1000), sk, /*baseSkip=*/false) && sk == 0,
        "ptr-facts: no base skip");
  // a concat part: concat(x, addr(seg)) = c fixes x and addr(seg); x is
  // substituted, addr(seg) is not (nor the concat)
  Expr t = mk<BCONCAT>(x, as);
  sea::TopEqFolder f([](Expr e) { return e; });
  Expr ux = mk<BULT>(x, n32(3)), ua = mk<BULT>(as, n32(3));
  Expr r = f.fold(
      mk<AND>(mk<EQ>(t, op::bv::bvnum(mpz_class(5L), 64, efac)), ux, ua));
  bool xFolded = r->arity() == 3 && r->arg(1) != ux;
  bool aFolded = r->arity() == 3 && r->arg(2) != ua;
  llvm::outs() << "ptr-facts: concat-part x-folded=" << xFolded
               << " addr-folded=" << aFolded
               << " skipped-base=" << f.stats().skippedBase << "\n";
  CHECK(xFolded && !aFolded, "ptr-facts: concat part");
}

/// does \p e contain the node \p t?
bool mentions(const Expr &e, const Expr &t) {
  std::set<ENode *> seen;
  std::vector<ENode *> stack{e.get()};
  while (!stack.empty()) {
    ENode *n = stack.back();
    stack.pop_back();
    if (n == t.get())
      return true;
    if (!seen.insert(n).second)
      continue;
    for (size_t i = 0; i < n->arity(); ++i)
      stack.push_back(n->arg(i));
  }
  return false;
}

/// a concat fact skipped because it mentions an object still gives its
/// other parts; the conjunct that defines them keeps them (rebuilt, not
/// substituted), so the fold is equivalent to its input. z3 decides the
/// equivalence with the object's address as a free value \p a (the fold
/// never substitutes it).
void testConcatKept() {
  ExprFactory efac;
  auto name = [&](const char *n) { return mkTerm<std::string>(n, efac); };
  auto n32 = [&](long v) { return op::bv::bvnum(mpz_class(v), 32, efac); };
  Expr x = op::bv::bvConst(name("x"), 32), y = op::bv::bvConst(name("y"), 32);
  Expr as = mk<ADDR>(op::ptr::segment("stack.1", n32(0x1000)));
  Expr a = op::bv::bvConst(name("a"), 32);
  auto equiv = [&](const Expr &in, const Expr &out) {
    sea::solver::z3_solver_impl s(efac);
    s.add(mk<NEQ>(replaceAll(in, as, a), replaceAll(out, as, a)));
    return s.check() == SolverResult::UNSAT;
  };
  auto uses = [](const Expr &q, const Expr &t) {
    unsigned k = 0;
    for (size_t i = 0; i < q->arity(); ++i)
      k += mentions(Expr(q->arg(i)), t);
    return k;
  };
  // top: concat(x + y, addr(seg)) = 0x7_00000005 fixes x + y = 7; with
  // x = y (2x = 7 has no solution) the query is unsat
  {
    Expr s = mk<BADD>(x, y);
    Expr c = op::bv::bvnum(mpz_class((7UL << 32) | 5UL), 64, efac);
    Expr in = mk<AND>(mk<EQ>(mk<BCONCAT>(s, as), c), mk<BULT>(s, n32(100)),
                      mk<EQ>(x, y));
    sea::TopEqFolder f([](Expr e) { return e; });
    Expr out = f.fold(in);
    bool eq = equiv(in, out);
    llvm::outs() << "concat-kept: top equiv=" << eq
                 << " part-uses=" << uses(out, s) << "\n";
    CHECK(eq, "concat-kept: top: the fold is equivalent to its input");
    CHECK(uses(out, s) == 1, "concat-kept: top: x + y only in its fact");
  }
  // nested: concat(addr(seg), concat(x, y)) = 0x1000_00000003_00000004:
  // the inner concat and x, y are facts, the outer one is skipped
  {
    Expr c = op::bv::bvnum((mpz_class(0x1000UL) << 64) +
                               (mpz_class(3UL) << 32) + mpz_class(4UL),
                           96, efac);
    Expr in =
        mk<AND>(mk<EQ>(mk<BCONCAT>(as, mk<BCONCAT>(x, y)), c), mk<BULT>(x, y));
    sea::TopEqFolder f([](Expr e) { return e; });
    Expr out = f.fold(in);
    bool eq = equiv(in, out);
    llvm::outs() << "concat-kept: nested equiv=" << eq
                 << " x-uses=" << uses(out, x) << " y-uses=" << uses(out, y)
                 << "\n";
    CHECK(eq, "concat-kept: nested: the fold is equivalent to its input");
    CHECK(uses(out, x) == 1 && uses(out, y) == 1,
          "concat-kept: nested: x, y only in their fact");
  }
}

/// z3: \p a <=> \p b
bool equiv(ExprFactory &efac, const Expr &a, const Expr &b) {
  sea::solver::z3_solver_impl s(efac);
  s.add(mk<NEG>(mk<EQ>(a, b)));
  return s.check() == SolverResult::UNSAT;
}

SolverResult check(ExprFactory &efac, const Expr &e) {
  sea::solver::z3_solver_impl s(efac);
  s.add(e);
  return s.check();
}

const char *res(SolverResult r) {
  return r == SolverResult::SAT     ? "sat"
         : r == SolverResult::UNSAT ? "unsat"
                                    : "unknown";
}

/// a simplifier that rewrites the conjunct \p from (`t = c`) into `t <= c
/// /\ t >= c`: equivalent, holds t, and is no fact the fold reads
sea::TopEqFolder::Simplify splitEq(const Expr &from) {
  Expr t = from->left(), c = from->right();
  Expr to = mk<AND>(mk<BULE>(t, c), mk<BUGE>(t, c));
  return [from, to](Expr e) { return replaceAll(e, from, to); };
}

/// An injected simplifier over the top-level conjuncts, one pass per call:
/// a rule fires when all its `from` conjuncts are present, and replaces them
/// by its `to` conjuncts (each conjunct used by one rule at most). Each rule
/// is an equivalence; none of them is a rule of the rewriter.
using ConjRule = std::pair<std::vector<Expr>, std::vector<Expr>>;
sea::TopEqFolder::Simplify conjRules(std::vector<ConjRule> rules) {
  return [rules](Expr e) {
    ExprVector cs;
    std::vector<Expr> stack{e};
    while (!stack.empty()) {
      Expr c = stack.back();
      stack.pop_back();
      if (isOpX<AND>(c))
        for (size_t i = c->arity(); i-- > 0;)
          stack.push_back(c->arg(i));
      else
        cs.push_back(c);
    }
    std::set<ENode *> used;
    ExprVector added;
    for (const ConjRule &r : rules) {
      bool all = true;
      for (const Expr &f : r.first)
        all &= llvm::is_contained(cs, f) && !used.count(f.get());
      if (!all)
        continue;
      for (const Expr &f : r.first)
        used.insert(f.get());
      added.insert(added.end(), r.second.begin(), r.second.end());
    }
    ExprVector out;
    for (const Expr &c : cs)
      if (!used.count(c.get()))
        out.push_back(c);
    out.insert(out.end(), added.begin(), added.end());
    if (out.empty())
      return mk<TRUE>(e->efac());
    return out.size() == 1 ? out[0] : mknary<AND>(out.begin(), out.end());
  };
}

/// fold \p in with \p simp (at most \p rounds rounds); print and check
/// z3 equivalence and the re-added counts
void trailCase(ExprFactory &efac, const char *name, const Expr &in,
               sea::TopEqFolder::Simplify simp, uint64_t reAdded,
               uint64_t reAddedConst, unsigned rounds = 64) {
  sea::TopEqFolder f(std::move(simp), sea::ConcatIte::Numeral, rounds);
  Expr out = f.fold(in);
  bool eq = equiv(efac, in, out);
  llvm::outs() << "trail: " << name << " re-added=" << f.stats().reAdded
               << " const=" << f.stats().reAddedConst
               << " cap=" << f.stats().roundCap << " equiv=" << eq << "\n";
  CHECK(eq, std::string("trail: ") + name + ": equivalent to its input");
  CHECK(f.stats().reAdded == reAdded && f.stats().reAddedConst == reAddedConst,
        std::string("trail: ") + name + ": re-added counts");
}

void testTrail() {
  ExprFactory efac;
  auto name = [&](const char *n) { return mkTerm<std::string>(n, efac); };
  auto n8 = [&](long v) { return op::bv::bvnum(mpz_class(v), 8, efac); };
  Expr x = op::bv::bvConst(name("x"), 8), z = op::bv::bvConst(name("z"), 8);
  // a term: x * x = 5 /\ x * x < 9, unsat
  {
    Expr t = mk<BMUL>(x, x);
    Expr fact = mk<EQ>(t, n8(5));
    Expr in = mk<AND>(fact, mk<BULT>(t, n8(9)));
    sea::TopEqFolder f(splitEq(fact));
    Expr out = f.fold(in);
    bool eq = equiv(efac, in, out);
    llvm::outs() << "trail: term re-added=" << f.stats().reAdded
                 << " rounds=" << f.stats().rounds
                 << " in=" << res(check(efac, in))
                 << " out=" << res(check(efac, out)) << " equiv=" << eq << "\n";
    CHECK(f.stats().reAdded == 1 && f.stats().reAddedConst == 0,
          "trail: term: the lost fact is re-added");
    CHECK(eq, "trail: term: the fold is equivalent to its input");
    CHECK(check(efac, out) == SolverResult::UNSAT, "trail: term: unsat kept");
  }
  // a constant: x = 5 /\ x < 9; re-added, then step 2 takes it into the
  // model trail
  {
    Expr fact = mk<EQ>(x, n8(5));
    Expr in = mk<AND>(fact, mk<BULT>(x, n8(9)), mk<BULT>(z, x));
    sea::TopEqFolder f(splitEq(fact));
    Expr out = f.fold(in);
    sea::TopConstEliminator elim;
    Expr q2 = elim.eliminate(out);
    bool onTrail = false;
    for (const sea::TrailEntry &e : elim.trail())
      onTrail |= e.var == x && e.value == n8(5);
    bool eq = equiv(efac, in, out);
    llvm::outs() << "trail: const re-added=" << f.stats().reAdded
                 << " equiv=" << eq << " step2-trail=" << onTrail
                 << " step2-mentions=" << mentions(q2, x) << "\n";
    CHECK(f.stats().reAdded == 1 && f.stats().reAddedConst == 1,
          "trail: const: the lost fact is re-added");
    CHECK(eq, "trail: const: the fold is equivalent to its input");
    CHECK(onTrail && !mentions(q2, x),
          "trail: const: step 2 moves it to the model trail");
  }
  // two lost facts, one of a constant that the other's term holds:
  // x * x = 9 /\ x <= 5 /\ x >= 5, unsat (x = 5, 25 != 9). Injected, one
  // pass per call: x * x = 9 ~> x * x <= 9 /\ x * x >= 9, x <= 5 /\ x >= 5
  // ~> x = 5, and x = 5 ~> x <= 5 /\ x >= 5. The second round replaces x * x
  // (lost), the third round replaces x (lost). Kept alone, x * x = 9 is sat (x
  // = 3); with x = 5 re-added too, step 2 substitutes 5 for x in it: 5 * 5 = 9,
  // false
  {
    Expr t = mk<BMUL>(x, x);
    Expr in = mk<AND>(mk<EQ>(t, n8(9)), mk<BULE>(x, n8(5)), mk<BUGE>(x, n8(5)));
    Expr eqT = mk<EQ>(t, n8(9)), eqX = mk<EQ>(x, n8(5));
    Expr le = mk<BULE>(x, n8(5)), ge = mk<BUGE>(x, n8(5));
    auto simp = conjRules({{{eqT}, {mk<BULE>(t, n8(9)), mk<BUGE>(t, n8(9))}},
                           {{le, ge}, {eqX}},
                           {{eqX}, {le, ge}}});
    sea::TopEqFolder f(simp);
    Expr out = f.fold(in);
    sea::TopConstEliminator elim;
    Expr q2 = elim.eliminate(out);
    bool eq = equiv(efac, in, out);
    // the term's fact alone with Q_n (the constant's dropped, not
    // substituted): what step 2 must not send
    Expr termOnly = replaceAll(out, eqX, mk<TRUE>(efac));
    llvm::outs() << "trail: two re-added=" << f.stats().reAdded
                 << " const=" << f.stats().reAddedConst << " equiv=" << eq
                 << " step2=" << res(check(efac, q2))
                 << " term-only=" << res(check(efac, termOnly))
                 << " step2-mentions=" << mentions(q2, x) << "\n";
    CHECK(f.stats().reAdded == 2 && f.stats().reAddedConst == 1,
          "trail: two: both facts re-added, one of a constant");
    CHECK(eq, "trail: two: the fold is equivalent to its input");
    CHECK(check(efac, q2) == SolverResult::UNSAT && !mentions(q2, x),
          "trail: two: step 2 substitutes x in the kept term fact");
  }
  Expr b = bind::boolConst(name("b")), y = op::bv::bvConst(name("y"), 8);
  Expr y1 = mk<EQ>(y, n8(1));
  // lost b: b ~> !!b (no fact); the next round replaces b in it
  trailCase(efac, "bool", mk<AND>(b, mk<OR>(mk<NEG>(b), y1)),
            conjRules({{{b}, {mk<NEG>(mk<NEG>(b))}}}), 1, 1);
  // lost !b: !b ~> (b -> false)
  trailCase(efac, "not-bool", mk<AND>(mk<NEG>(b), mk<OR>(b, y1)),
            conjRules({{{mk<NEG>(b)}, {mk<IMPL>(b, mk<FALSE>(efac))}}}), 1, 1);
  // lost concat part: concat(x, z) = 0x0102 ~> x <= 1 /\ x >= 1 /\ z = 2;
  // x is lost (z keeps its fact, the concat's term left the query)
  {
    Expr cxz = mk<BCONCAT>(x, z);
    Expr k = op::bv::bvnum(mpz_class(0x0102L), 16, efac);
    trailCase(efac, "concat-part", mk<AND>(mk<EQ>(cxz, k), mk<BULT>(x, n8(3))),
              conjRules({{{mk<EQ>(cxz, k)},
                          {mk<BULE>(x, n8(1)), mk<BUGE>(x, n8(1)),
                           mk<EQ>(z, n8(2))}}}),
              1, 1);
  }
  // the round bound: [x * x = 5] with 2 rounds; the second round replaces
  // x * x in the rewritten conjunct and is the last (unsat kept)
  {
    Expr fact = mk<EQ>(mk<BMUL>(x, x), n8(5));
    trailCase(efac, "cap", fact, splitEq(fact), 1, 0, /*rounds=*/2);
  }
  // a path concat holding another value: concat(x, z) = 0x0102 first, then
  // concat(x, z) = 0x0103 /\ ...: the second states x = 1 (the map's value)
  // but not the concat, so its concat is replaced, x with it, and the
  // conjunct is 0x0102 = 0x0103 (false); nothing lost
  {
    Expr cxz = mk<BCONCAT>(x, z);
    Expr k2 = op::bv::bvnum(mpz_class(0x0102L), 16, efac),
         k3 = op::bv::bvnum(mpz_class(0x0103L), 16, efac);
    trailCase(
        efac, "path-mismatch",
        mk<AND>(mk<EQ>(cxz, k2), mk<EQ>(cxz, k3), mk<BULT>(x, n8(3))),
        [](Expr e) { return e; }, 0, 0);
  }
  // normal: x = 3 /\ x * z = 7 /\ x * z < 9. The first round rewrites the
  // second conjunct to 3 * z = 7 (x substituted in it): x * z leaves the query
  // and is replaced nowhere after; nothing is re-added
  {
    Expr t = mk<BMUL>(x, z);
    Expr in = mk<AND>(mk<EQ>(x, n8(3)), mk<EQ>(t, n8(7)), mk<BULT>(t, n8(9)));
    sea::TopEqFolder f([](Expr e) { return e; });
    Expr out = f.fold(in);
    bool eq = equiv(efac, in, out);
    llvm::outs() << "trail: normal re-added=" << f.stats().reAdded
                 << " rounds=" << f.stats().rounds << " equiv=" << eq << "\n";
    CHECK(f.stats().reAdded == 0, "trail: normal: nothing re-added");
    CHECK(!mentions(out, t), "trail: normal: x * z left the query");
    CHECK(eq, "trail: normal: the fold is equivalent to its input");
  }
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
  testConcatKept();
  llvm::outs() << "test: concat-kept ok\n";
  testTrail();
  llvm::outs() << "test: trail ok\n";
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
