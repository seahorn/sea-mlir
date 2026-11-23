// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- CanonicalSumsTest.cpp - canonical linear forms of the query --------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if a
// check fails. Run by test/Expr/canonical-sums.test.
//
// What is tested (include/sea/Expr/CanonicalSums.hh):
//  - one case per item of the form: the exact output term (terms are
//    hash-consed; leaves are ordered by the creation order of the fixture's
//    variables), and that the output is equivalent to the input (a z3
//    miter, no assumption);
//  - a seeded random test: random linear terms over leaves of several
//    widths (zext, concat 0, low and high extracts, bitwise terms, products,
//    ite; products of two terms at <= 8 bits), in equalities and unsigned
//    compares, at widths 1 to 64. For each:
//    the output is equivalent to the input (a miter: Yices, else Bitwuzla,
//    else z3, with a limit, its unknowns shown) and equal to it at 8 random
//    ground points (z3; always decided), it has no bvsub and no
//    bvneg, and two fresh factories give the same text (determinism).
//
//===----------------------------------------------------------------------===//

#include "sea/Expr/CanonicalSums.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprOpBind.hh"
#include "sea/Expr/ExprOpBv.hh"
#include "sea/Expr/Smt/SolverFactory.h"
#include "sea/Expr/Smt/Z3SolverImpl.hh"
#include "sea/Expr/TypeChecker.hh"
#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>

using namespace expr;

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

std::string str(const Expr &e) {
  std::ostringstream os;
  os << *e;
  return os.str();
}

struct Fixture {
  ExprFactory efac;

  Expr var(const std::string &n, unsigned w) {
    return op::bv::bvConst(mkTerm<std::string>(n, efac), w);
  }
  Expr num(const mpz_class &v, unsigned w) { return op::bv::bvnum(v, w, efac); }
  Expr add(std::initializer_list<Expr> a) {
    ExprVector v(a);
    return mknary<BADD>(v.begin(), v.end());
  }

  /// in = out for every assignment (z3; bit-vector terms or formulas)
  bool equivalent(const Expr &in, const Expr &out) {
    sea::solver::z3_solver_impl s(efac);
    s.add(mk<NEG>(mk<EQ>(in, out)));
    if (getenv("CANON_TEST_DUMP"))
      s.to_smt_lib(llvm::errs());
    return s.check() == sea::solver::SolverResult::UNSAT;
  }

  /// the random miter: Yices, else Bitwuzla, else z3, with a limit (z3
  /// does not decide some 16-bit miters of rearranged adders in minutes,
  /// where Yices and Bitwuzla take milliseconds; a narrow product of
  /// rearranged sums can take Yices minutes). UNSAT: equivalent; SAT: a
  /// counterexample; UNKNOWN: out of the limit (counted, shown; the
  /// ground checks below still decide that case)
  sea::solver::SolverResult miter(const Expr &in, const Expr &out,
                                  const char *&backend) {
    using sea::solver::Backend;
    Backend b = sea::solver::available(Backend::Yices)      ? Backend::Yices
                : sea::solver::available(Backend::Bitwuzla) ? Backend::Bitwuzla
                                                            : Backend::Z3;
    backend = sea::solver::backendName(b);
    sea::solver::Limits l;
    if (b == Backend::Z3)
      l.rlimit = 20000000;
    else
      l.timeoutMs = 1000;
    sea::solver::SolverConfig config;
    config.yicesLogic = "QF_BV";
    auto s = sea::solver::makeSolver(efac, b, l, config);
    if (!s || !s->add(mk<NEG>(mk<EQ>(in, out))))
      return sea::solver::SolverResult::UNKNOWN;
    return s->check();
  }

  /// \p in = \p out under \p n random assignments of the bit-vector
  /// constants (ground, so z3 decides each at once); false on a difference
  bool groundEqual(const Expr &in, const Expr &out, std::mt19937_64 &rng,
                   unsigned n) {
    ExprSet consts;
    filter(in, op::bind::IsConst(), std::inserter(consts, consts.begin()));
    TypeChecker tc;
    for (unsigned k = 0; k < n; ++k) {
      ExprVector conj{mk<NEG>(mk<EQ>(in, out))};
      for (const Expr &c : consts) {
        Expr t = tc.typeOf(c);
        if (!isOpX<op::BVSORT>(t))
          continue;
        unsigned w = op::bv::width(t);
        mpz_class v = 0L;
        for (unsigned b = 0; b < w; b += 32)
          v = v * mpz_class(4294967296UL) +
              mpz_class((unsigned long)(rng() & 0xffffffffUL));
        conj.push_back(mk<EQ>(c, num(smallarith::modPow2(v, w), w)));
      }
      sea::solver::z3_solver_impl s(efac);
      s.add(mknary<AND>(conj.begin(), conj.end()));
      if (s.check() != sea::solver::SolverResult::UNSAT)
        return false;
    }
    return true;
  }

  Expr canon(const Expr &e, CanonicalSums::Stats *st = nullptr) {
    CanonicalSums c;
    Expr r = c.rewrite(e);
    if (st)
      *st = c.stats();
    return r;
  }

  /// \p in (a formula) becomes \p expect, equivalently
  void expect(const char *name, const Expr &in, const Expr &expect) {
    Expr out = canon(in);
    CHECK(out == expect,
          name << ": got " << str(out) << ", expected " << str(expect));
    CHECK(equivalent(in, out), name << ": not equivalent: " << str(out));
  }
};

/// a non-linear context of a term (an equality with a sum side would be
/// balanced)
Expr ctx(const Expr &t) { return mk<BULE>(t, t); }

/// 1: the shapes P, P + c, P + ~N + (c+1), ~N + (c+1); coefficients
void testShape() {
  Fixture F;
  Expr x = F.var("x", 8), y = F.var("y", 8), z = F.var("z", 8);
  auto c = [&](unsigned long v) { return F.num(v, 8); };
  // x + y + z: one n-ary chain
  F.expect("shape.p", ctx(F.add({z, mk<BADD>(x, y)})), ctx(F.add({x, y, z})));
  // 3 + y + x: the constant last
  F.expect("shape.p-c", ctx(F.add({c(3), y, x})),
           ctx(mk<BADD>(F.add({x, y}), c(3))));
  // x - y + 5 = (x + ~y) + 6: no bvsub, one bvnot
  F.expect("shape.p-n-c", ctx(mk<BADD>(mk<BSUB>(x, y), c(5))),
           ctx(mk<BADD>(mk<BADD>(x, mk<BNOT>(y)), c(6))));
  // x - y - z = (x + ~(y + z)) + 1: -1 is not distributed per term
  F.expect("shape.n-chain", ctx(mk<BSUB>(mk<BSUB>(x, y), z)),
           ctx(mk<BADD>(mk<BADD>(x, mk<BNOT>(F.add({y, z}))), c(1))));
  // -x = ~x + 1; -x - 1 = ~x
  F.expect("shape.neg", ctx(mk<BNEG>(x)), ctx(mk<BADD>(mk<BNOT>(x), c(1))));
  F.expect("shape.neg-1", ctx(mk<BSUB>(mk<BNEG>(x), c(1))), ctx(mk<BNOT>(x)));
  // x - x + 7 = 7
  F.expect("shape.const", ctx(F.add({mk<BSUB>(x, x), c(7)})), ctx(c(7)));
  // coefficients: 129 = -127 goes to N; 128 = 2^(w-1) stays positive
  F.expect("coef.neg", ctx(F.add({mk<BMUL>(c(129), x), y})),
           ctx(mk<BADD>(mk<BADD>(y, mk<BNOT>(mk<BMUL>(c(127), x))), c(1))));
  F.expect("coef.half", ctx(F.add({mk<BMUL>(x, c(128)), y})),
           ctx(F.add({mk<BMUL>(c(128), x), y})));
  // 2x + x = 3x; 255 * (x + y) distributes (-1), 3 * (x + y) does not
  F.expect("coef.merge", ctx(F.add({mk<BMUL>(c(2), x), x})),
           ctx(mk<BMUL>(c(3), x)));
  F.expect("coef.minus-one", ctx(mk<BMUL>(c(255), mk<BADD>(x, y))),
           ctx(mk<BADD>(mk<BNOT>(F.add({x, y})), c(1))));
  F.expect("coef.no-distribute", ctx(mk<BMUL>(c(3), mk<BADD>(y, x))),
           ctx(mk<BMUL>(c(3), F.add({x, y}))));
  // width 1: 1 = 2^0 is positive; x + x = 0
  Expr b = F.var("b", 1), d = F.var("d", 1);
  F.expect("w1", ctx(mk<BSUB>(b, d)), ctx(F.add({b, d})));
}

/// 2: one leaf order at every width; equal forms are one node
void testOrder() {
  Fixture F;
  Expr x = F.var("x", 8), y = F.var("y", 8);
  Expr X = F.var("X", 16), Y = F.var("Y", 16);
  // the bases x, y are ordered by creation, at 8 and at 16 bits
  F.expect("order.8", ctx(F.add({y, x})), ctx(F.add({x, y})));
  Expr zx = op::bv::zext(x, 16), zy = op::bv::zext(y, 16);
  F.expect("order.16", ctx(F.add({zy, zx})), ctx(F.add({zx, zy})));
  // concat 0 y and a low extract of X share their bases y and X: the
  // order is x < y < X (creation), whatever the spelling
  Expr cy = mk<BCONCAT>(F.num(0, 8), y);
  Expr lx = op::bv::zext(op::bv::extract(7, 0, X), 16);
  F.expect("order.base", ctx(F.add({lx, cy, zx})), ctx(F.add({zx, cy, lx})));
  // zext y and concat 0 y are one leaf (spelled as the first one met)
  F.expect("order.same-leaf", mk<BULE>(F.add({zy, Y}), F.add({cy, Y})),
           mk<BULE>(F.add({zy, Y}), F.add({zy, Y})));
  // equal forms, different spellings: one node
  Expr a = mk<BADD>(mk<BSUB>(x, y), F.num(3, 8));
  Expr b = F.add({F.num(3, 8), mk<BNEG>(y), x});
  CanonicalSums c;
  Expr out = c.rewrite(mk<BULE>(a, b));
  CHECK(isOpX<BULE>(out) && out->arg(0) == out->arg(1),
        "order.hash-cons: got " << str(out));
}

/// 3: equalities with a sum side are balanced
void testEq() {
  Fixture F;
  Expr x = F.var("x", 8), y = F.var("y", 8), z = F.var("z", 8);
  auto c = [&](unsigned long v) { return F.num(v, 8); };
  // z = 5 - x - y: x + y + z = 5, no isolated term
  F.expect("eq.const", mk<EQ>(z, mk<BSUB>(mk<BSUB>(c(5), x), y)),
           mk<EQ>(F.add({x, y, z}), c(5)));
  // x + 1 = y - z: x + z + 1 = y
  F.expect("eq.balanced", mk<EQ>(mk<BADD>(x, c(1)), mk<BSUB>(y, z)),
           mk<EQ>(mk<BADD>(F.add({x, z}), c(1)), y));
  // the same equality from the other side
  F.expect("eq.symmetric", mk<EQ>(mk<BSUB>(y, z), mk<BADD>(x, c(1))),
           mk<EQ>(mk<BADD>(F.add({x, z}), c(1)), y));
  // x + 1 = x: false; x - y = x - y + 0: true
  F.expect("eq.false", mk<EQ>(mk<BADD>(x, c(1)), x), mk<FALSE>(F.efac));
  F.expect("eq.true", mk<EQ>(mk<BSUB>(x, y), F.add({x, mk<BNEG>(y)})),
           mk<TRUE>(F.efac));
  // no sum side: unchanged
  F.expect("eq.plain", mk<EQ>(x, c(4)), mk<EQ>(x, c(4)));
}

/// 4: an extract stays on top of the full-width sum
void testExtract() {
  Fixture F;
  Expr X = F.var("X", 16), Y = F.var("Y", 16), x = F.var("x", 8);
  Expr s = mk<BSUB>(Y, X);
  Expr cs = mk<BADD>(mk<BADD>(Y, mk<BNOT>(X)), F.num(1, 16));
  // the low byte of a 16-bit difference: canonical at 16 bits, extract on top
  F.expect("extract.top", ctx(op::bv::extract(7, 0, s)),
           ctx(op::bv::extract(7, 0, cs)));
  // inside an 8-bit sum: a leaf, its base the 16-bit sum
  F.expect("extract.leaf",
           ctx(F.add({op::bv::extract(7, 0, s), x, F.num(2, 8)})),
           ctx(mk<BADD>(F.add({x, op::bv::extract(7, 0, cs)}), F.num(2, 8))));
}

/// 5: bitwise terms stay as they are
void testBitwise() {
  Fixture F;
  Expr x = F.var("x", 8), y = F.var("y", 8);
  Expr nx = mk<BNOT>(x), xx = mk<BXOR>(x, F.num(255, 8));
  F.expect("bitwise.not-eq", mk<EQ>(nx, y), mk<EQ>(nx, y));
  F.expect("bitwise.xor-eq", mk<EQ>(xx, y), mk<EQ>(xx, y));
  // ~x + 1 is a sum of the leaf ~x: unchanged
  F.expect("bitwise.not-plus-one", ctx(mk<BADD>(nx, F.num(1, 8))),
           ctx(mk<BADD>(nx, F.num(1, 8))));
}

/// random terms: equivalent, no bvsub / bvneg, deterministic
struct Gen {
  Fixture &F;
  std::mt19937_64 &rng;
  const std::vector<unsigned> widths{1, 2, 5, 8, 16, 33, 64};

  unsigned pick(unsigned n) { return unsigned(rng() % n); }
  mpz_class rnd(unsigned w) {
    mpz_class m, half;
    mpz_ui_pow_ui(m.get_mpz_t(), 2, w);
    mpz_ui_pow_ui(half.get_mpz_t(), 2, w - 1);
    switch (pick(5)) {
    case 0:
      return mpz_class(1L);
    case 1:
      return m - 1;
    case 2:
      return half;
    case 3:
      return smallarith::modPow2(half + 1, w);
    default:
      return smallarith::modPow2(
          mpz_class((unsigned long)rng()) * mpz_class((unsigned long)rng()), w);
    }
  }
  Expr leaf(unsigned w) {
    return F.var(std::string("v") + std::to_string(pick(3)) + "_" +
                     std::to_string(w),
                 w);
  }
  Expr term(unsigned w, unsigned depth) {
    if (depth == 0)
      return pick(4) ? leaf(w) : F.num(rnd(w), w);
    unsigned k = pick(13);
    switch (k) {
    case 0:
    case 1: {
      ExprVector a;
      for (unsigned i = 0, n = 2 + pick(3); i < n; ++i)
        a.push_back(term(w, depth - 1));
      return mknary<BADD>(a.begin(), a.end());
    }
    case 2:
    case 3:
      return mk<BSUB>(term(w, depth - 1), term(w, depth - 1));
    case 4:
      return mk<BNEG>(term(w, depth - 1));
    case 5:
      return mk<BMUL>(F.num(rnd(w), w), term(w, depth - 1));
    case 6:
      // a product of two terms (a leaf): narrow only, the miter of a wide
      // multiplier is hard for the solver
      if (w > 8)
        return leaf(w);
      return mk<BMUL>(term(w, depth - 1), term(w, depth - 1));
    case 7: {
      if (w == 1)
        return leaf(w);
      unsigned v = 1 + pick(w - 1);
      Expr s = term(v, depth - 1);
      return pick(2) ? op::bv::zext(s, w)
                     : Expr(mk<BCONCAT>(F.num(0, w - v), s));
    }
    case 8: {
      if (w >= 64)
        return leaf(w);
      unsigned u = w + 1 + pick(64 - w);
      unsigned lo = pick(3) ? 0 : pick(u - w + 1);
      return op::bv::extract(lo + w - 1, lo, term(u, depth - 1));
    }
    case 9:
      return mk<BNOT>(term(w, depth - 1));
    case 10:
      return mk<BXOR>(term(w, depth - 1), term(w, depth - 1));
    case 11:
      return mk<ITE>(mk<EQ>(term(w, depth - 1), term(w, depth - 1)),
                     term(w, depth - 1), term(w, depth - 1));
    default:
      return leaf(w);
    }
  }
  Expr formula(unsigned depth) {
    unsigned w = widths[pick(widths.size())];
    Expr a = term(w, depth), b = term(w, depth);
    return pick(3) ? Expr(mk<EQ>(a, b)) : Expr(mk<BULE>(a, b));
  }
};

bool hasSubOrNeg(const Expr &e) {
  std::unordered_set<ENode *> seen;
  std::vector<ENode *> st{e.get()};
  while (!st.empty()) {
    ENode *n = st.back();
    st.pop_back();
    if (!seen.insert(n).second)
      continue;
    if (isOpX<BSUB>(n) || isOpX<BNEG>(n))
      return true;
    for (size_t i = 0; i < n->arity(); ++i)
      st.push_back(n->arg(i));
  }
  return false;
}

void testRandom(unsigned count) {
  unsigned sums = 0, eqs = 0, unknown = 0;
  const char *backend = "";
  for (unsigned i = 0; i < count; ++i) {
    std::string text[2];
    for (int run = 0; run < 2; ++run) {
      Fixture F;
      std::mt19937_64 rng(1000 + i);
      Gen g{F, rng};
      Expr q = mk<AND>(g.formula(3), g.formula(2));
      CanonicalSums::Stats st;
      if (getenv("CANON_TEST_TRACE"))
        llvm::errs() << "random " << i << ": " << str(q) << "\n";
      Expr out = F.canon(q, &st);
      text[run] = str(out);
      if (getenv("CANON_TEST_TRACE"))
        llvm::errs() << "  -> " << text[run] << "\n";
      if (run)
        continue;
      sums += st.sums;
      eqs += st.eqs;
      sea::solver::SolverResult r = F.miter(q, out, backend);
      CHECK(r != sea::solver::SolverResult::SAT,
            "random " << i << ": not equivalent: " << str(q) << " -> "
                      << text[run]);
      unknown += r == sea::solver::SolverResult::UNKNOWN;
      CHECK(F.groundEqual(q, out, rng, 8),
            "random " << i << ": differs at a ground point: " << str(q)
                      << " -> " << text[run]);
      CHECK(!hasSubOrNeg(out),
            "random " << i << ": bvsub / bvneg left: " << text[run]);
    }
    CHECK(text[0] == text[1], "random " << i << ": not deterministic");
  }
  llvm::outs() << "random: terms=" << count << " sums=" << sums
               << " eqs=" << eqs << " miter=" << backend
               << " unknown=" << unknown << "\n";
}

} // namespace

int main(int argc, char **argv) {
  unsigned randomCount = 300;
  if (argc > 1)
    randomCount = std::atoi(argv[1]);
  struct Test {
    const char *name;
    void (*fn)();
  } tests[] = {{"shape", testShape},
               {"order", testOrder},
               {"eq", testEq},
               {"extract", testExtract},
               {"bitwise", testBitwise}};
  for (const Test &t : tests) {
    int before = failures;
    t.fn();
    llvm::outs() << "test: " << t.name << " "
                 << (failures == before ? "ok" : "FAILED") << "\n";
  }
  int before = failures;
  testRandom(randomCount);
  llvm::outs() << "test: random " << (failures == before ? "ok" : "FAILED")
               << "\n";
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
