// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- AckermannTest.cpp - tests of the Ackermann lowering ----------------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if a
// check fails. Run by test/Expr/ackermann.test.
//
// What is tested (include/sea/Expr/Ackermann.hh, docs/design/Ackermann.md):
//  - read sharing: the same index term, and numerals equal modulo 2^w, give
//    one constant; different numerals need no constraint; symbolic pairs get
//    one constraint each (counts);
//  - which arrays are lowered: only-read arrays yes; an array under a store,
//    in an equality or in an ite is kept;
//  - soundness of the constraints: aliasing pairs (x = y, x = numeral) that
//    are unsat stay unsat; nested reads (an index read from the same array);
//  - model reconstruction: the original formula is true in AckermannModel,
//    an unread index reads 0 (defaulted), the array term itself evaluates as
//    the same function;
//  - a seeded random test at 3-bit indices: original and lowered formulas
//    agree on sat / unsat (Yices), and every sat model of the lowered one
//    makes the original true through AckermannModel (also with z3);
//  - LowBits: random terms against extract(k-1, 0, e) (z3, validity);
//  - narrowed offsets (setNarrow): with every read's offset in its object
//    (the A-INB the narrowing rests on, conjoined), original and lowered
//    formulas agree, and sat models are models of the original.
//
//===----------------------------------------------------------------------===//

#include "sea/Expr/Ackermann.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/LowBits.hh"
#include "sea/Expr/Smt/Yices2SolverImpl.hh"
#include "sea/Expr/Smt/Z3SolverImpl.hh"
#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <random>
#include <string>
#include <vector>

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

constexpr unsigned kIdx = 3, kVal = 4;

struct Env {
  ExprFactory efac;
  Expr arrSort() {
    return sort::arrayTy(op::bv::bvsort(kIdx, efac),
                         op::bv::bvsort(kVal, efac));
  }
  Expr arr(const std::string &n) {
    return op::bind::mkConst(mkTerm<std::string>(n, efac), arrSort());
  }
  Expr idx(const std::string &n) {
    return op::bv::bvConst(mkTerm<std::string>(n, efac), kIdx);
  }
  Expr val(const std::string &n) {
    return op::bv::bvConst(mkTerm<std::string>(n, efac), kVal);
  }
  Expr in(long v) { return op::bv::bvnum(mpz_class(v), kIdx, efac); }
  Expr vn(long v) { return op::bv::bvnum(mpz_class(v), kVal, efac); }
};

// the solver of the last model: a model must not outlive its solver
std::unique_ptr<sea::solver::Solver> lastSolver;

sea::solver::SolverResult solve(Env &env, Expr f, bool z3 = false,
                                sea::solver::Solver::model_ref *m = nullptr) {
  std::unique_ptr<sea::solver::Solver> s;
  if (z3)
    s = std::make_unique<sea::solver::z3_solver_impl>(env.efac);
  else
    s = std::make_unique<sea::solver::yices_solver_impl>(env.efac);
  s->add(f);
  auto r = s->check();
  if (m && r == sea::solver::SolverResult::SAT) {
    *m = s->get_model();
    lastSolver = std::move(s);
  }
  return r;
}

bool isTrue(Expr e) { return e && isOpX<TRUE>(e); }

void testSharing() {
  Env env;
  Expr A = env.arr("A");
  Expr x = env.idx("x"), y = env.idx("y");
  using op::array::select;
  // select(A, x) twice, select(A, 1), select(A, 9) (= 1 mod 8), select(A, 2),
  // select(A, y)
  Expr f =
      mk<AND>(mk<EQ>(select(A, x), select(A, x)),
              mk<EQ>(select(A, env.in(1)), env.vn(3)),
              mk<EQ>(select(A, op::bv::bvnum(mpz_class(9L), kIdx, env.efac)),
                     env.vn(3)));
  f = mk<AND>(f, mk<NEQ>(select(A, env.in(2)), select(A, y)));
  Ackermannizer ack;
  Expr g = ack.lower(f);
  const auto &s = ack.stats();
  // reads: x, 1 (with 9), 2, y
  CHECK(s.lowered == 1 && s.kept == 0, "one array lowered");
  CHECK(s.reads == 4, "reads " << s.reads);
  CHECK(s.pairs == 6, "pairs " << s.pairs);
  CHECK(s.numeralPairs == 1, "numeral pairs " << s.numeralPairs);
  CHECK(s.constraints == 5, "constraints " << s.constraints);
  CHECK(solve(env, g) == sea::solver::SolverResult::SAT, "sat");
  CHECK(s.offsetPairs == 0, "offset pairs " << s.offsetPairs);
  // S + c: x + 1, 1 + x and x + 9 (= x + 1 mod 8) are one read; x + 2 another,
  // never equal to it: no constraint
  Expr xa = mk<BADD>(x, env.in(1)), xb = mk<BADD>(env.in(1), x),
       xc = mk<BADD>(x, op::bv::bvnum(mpz_class(9L), kIdx, env.efac)),
       xd = mk<BADD>(x, env.in(2));
  Expr h = mk<AND>(mk<AND>(mk<NEQ>(select(A, xa), select(A, xb)),
                           mk<EQ>(select(A, xc), select(A, xd))),
                   mk<EQ>(select(A, xd), select(A, xd)));
  Ackermannizer ack2;
  Expr g2 = ack2.lower(h);
  const auto &s2 = ack2.stats();
  CHECK(s2.reads == 2 && s2.offsetPairs == 1 && s2.constraints == 0,
        "offsets: reads " << s2.reads << " offset pairs " << s2.offsetPairs
                          << " constraints " << s2.constraints);
  CHECK(solve(env, g2) == sea::solver::SolverResult::UNSAT,
        "x + 1 and 1 + x share a cell");
  Expr h3 = mk<EQ>(mk<EQ>(select(A, xa), select(A, xd)), mk<FALSE>(env.efac));
  Ackermannizer ack3;
  CHECK(solve(env, ack3.lower(h3)) == sea::solver::SolverResult::SAT,
        "x + 1 and x + 2 may differ");
  // linear forms: (x + 3) - y and x - (y - 3) are one read; 2x + 1 and
  // x + x + 2 differ by an offset; ite(b, x + 1, y + 1) and ite(b, x + 2,
  // y + 2) too
  {
    Expr b = mk<EQ>(y, env.in(0));
    Expr r1 = select(A, mk<BSUB>(mk<BADD>(x, env.in(3)), y));
    Expr r2 = select(A, mk<BSUB>(x, mk<BSUB>(y, env.in(3))));
    Expr r3 = select(A, mk<BADD>(mk<BMUL>(env.in(2), x), env.in(1)));
    Expr r4 = select(A, mk<BADD>(x, x, env.in(2)));
    Expr r5 =
        select(A, mk<ITE>(b, mk<BADD>(x, env.in(1)), mk<BADD>(y, env.in(1))));
    Expr r6 =
        select(A, mk<ITE>(b, mk<BADD>(env.in(2), x), mk<BADD>(y, env.in(2))));
    Expr h4 = mk<AND>(mk<AND>(mk<NEQ>(r1, r2), mk<EQ>(r3, r4)), mk<EQ>(r5, r6));
    Ackermannizer ack4;
    Expr g4 = ack4.lower(h4);
    const auto &s4 = ack4.stats();
    // reads: r1(=r2), r3, r4, r5, r6; offset pairs: (r3, r4), (r5, r6)
    CHECK(s4.reads == 5 && s4.offsetPairs == 2 && s4.constraints == 8,
          "linear: reads " << s4.reads << " offset pairs " << s4.offsetPairs
                           << " constraints " << s4.constraints);
    CHECK(solve(env, g4) == sea::solver::SolverResult::UNSAT,
          "(x + 3) - y and x - (y - 3) share a cell");
    // reads that must stay apart (sat): x - y vs x + y, an ite vs one branch
    Expr apart[][2] = {
        {mk<BSUB>(x, y), mk<BADD>(x, y)},
        {mk<ITE>(b, mk<BADD>(x, env.in(1)), mk<BADD>(y, env.in(1))),
         mk<BADD>(x, env.in(1))},
        {mk<BMUL>(env.in(3), x), mk<BMUL>(env.in(5), x)}};
    // reads that meet when b is false (unsat): different offsets per branch
    {
      Expr i1 = mk<ITE>(b, mk<BADD>(x, env.in(1)), mk<BADD>(y, env.in(2)));
      Expr i2 = mk<ITE>(b, mk<BADD>(x, env.in(2)), mk<BADD>(y, env.in(2)));
      Expr h6 = mk<AND>(mk<NEG>(b), mk<NEQ>(select(A, i1), select(A, i2)));
      Ackermannizer ack6;
      CHECK(solve(env, ack6.lower(h6)) == sea::solver::SolverResult::UNSAT,
            "ite offsets differ per branch");
    }
    for (auto &pr : apart) {
      Expr h5 = mk<NEQ>(select(A, pr[0]), select(A, pr[1]));
      Ackermannizer ack5;
      CHECK(solve(env, ack5.lower(h5)) == sea::solver::SolverResult::SAT,
            "apart: " << *pr[0] << " vs " << *pr[1]);
    }
  }
  // residue pairs: S_k + c_k = S_l + c_l needs S_k - S_l = c_l - c_k; when
  // every coefficient of S_k - S_l is even (a multiple of 2^t) and c_l - c_k
  // is odd (not), the indices are never equal: no constraint. o + 2x + 1 vs
  // o + 2y: o cancels in the difference (the slots of one table)
  {
    Expr o = env.idx("o");
    Expr two = env.in(2);
    Expr i1 = mk<BADD>(mk<BMUL>(two, x), env.in(1));
    Expr i2 = mk<BMUL>(two, y);
    Expr i3 = mk<BADD>(o, mk<BMUL>(two, x), env.in(1));
    Expr i4 = mk<BADD>(o, mk<BMUL>(two, y));
    Expr i5 = mk<BADD>(mk<BMUL>(two, y), env.in(2)); // 2y + 2: even, kept
    Expr i6 = x;                                     // coefficient 1: kept
    Expr h7 = mk<AND>(mk<NEQ>(select(A, i1), select(A, i2)),
                      mk<NEQ>(select(A, i3), select(A, i4)));
    Ackermannizer ack7;
    Expr g7 = ack7.lower(h7);
    const auto &s7 = ack7.stats();
    // pairs: (i1,i2) odd difference: residue; (i3,i4) o cancels: residue;
    // (i1,i3), (i1,i4), (i2,i3), (i2,i4): o has coefficient 1: kept
    CHECK(s7.reads == 4 && s7.residuePairs == 2 && s7.constraints == 4,
          "residue: reads " << s7.reads << " residue pairs " << s7.residuePairs
                            << " constraints " << s7.constraints);
    CHECK(solve(env, g7) == sea::solver::SolverResult::SAT,
          "residue: the reads are apart");
    Expr h8 = mk<AND>(mk<NEQ>(select(A, i2), select(A, i5)),
                      mk<NEQ>(select(A, i1), select(A, i6)));
    Ackermannizer ack8;
    ack8.lower(h8);
    // (i2,i5): one symbolic part, an offset pair; (i1,i2), (i1,i5): odd
    // differences, residue; every pair with x (coefficient 1): kept
    CHECK(ack8.stats().offsetPairs == 1 && ack8.stats().residuePairs == 2 &&
              ack8.stats().constraints == 3,
          "residue: coefficient 1 kept: residue " << ack8.stats().residuePairs
                                                  << " constraints "
                                                  << ack8.stats().constraints);
    // the kept pairs still meet: 2y = 2y' + 2 when y = y' + 1 (mod 4)
    Expr h9 = mk<AND>(mk<EQ>(y, mk<BADD>(x, env.in(1))),
                      mk<NEQ>(select(A, mk<BMUL>(two, y)),
                              select(A, mk<BADD>(mk<BMUL>(two, x), two))));
    Ackermannizer ack9;
    CHECK(solve(env, ack9.lower(h9)) == sea::solver::SolverResult::UNSAT,
          "residue: an even difference may meet");
  }
  llvm::outs() << "test: sharing ok\n";
}

void testKept() {
  Env env;
  using op::array::select;
  Expr A = env.arr("A"), B = env.arr("B"), C = env.arr("C"), D = env.arr("D"),
       E = env.arr("E");
  Expr x = env.idx("x"), c = mk<EQ>(x, env.in(0));
  Expr f = mk<AND>(
      mk<AND>(mk<EQ>(select(op::array::store(A, x, env.vn(1)), x), env.vn(1)),
              mk<EQ>(select(B, x), env.vn(2))),
      mk<AND>(mk<EQ>(C, D),
              mk<EQ>(select(mk<ITE>(c, E, B), env.in(3)), env.vn(0))));
  Ackermannizer ack;
  ack.analyze(f);
  CHECK(ack.hasStore(), "store seen");
  Expr g = ack.lower(f);
  const auto &s = ack.stats();
  CHECK(s.arrays == 5, "arrays " << s.arrays);
  // B is under an ite too: kept; nothing is lowered
  CHECK(s.lowered == 0 && s.kept == 5, "lowered " << s.lowered);
  CHECK(g == f, "unchanged");
  Expr h = mk<AND>(f, mk<EQ>(select(env.arr("F"), x), env.vn(5)));
  Ackermannizer ack2;
  Expr g2 = ack2.lower(h);
  CHECK(ack2.stats().lowered == 1 && ack2.stats().kept == 5, "F lowered");
  CHECK(ack2.arrayIndex(env.arr("F")) == 0 && ack2.arrayIndex(B) == -1,
        "table");
  CHECK(solve(env, g2) == solve(env, h), "equisat");
  llvm::outs() << "test: kept ok\n";
}

void testAliasing() {
  Env env;
  using op::array::select;
  Expr A = env.arr("A");
  Expr x = env.idx("x"), y = env.idx("y");
  // x = y and A[x] != A[y]
  Expr f1 = mk<AND>(mk<EQ>(x, y), mk<NEQ>(select(A, x), select(A, y)));
  // x = 5, A[x] = 1, A[5] = 2
  Expr f2 = mk<AND>(mk<EQ>(x, env.in(5)), mk<EQ>(select(A, x), env.vn(1)),
                    mk<EQ>(select(A, env.in(5)), env.vn(2)));
  // nested: A[A[0] extended] where A[0] = 0 and A[A[0]] != A[0]
  Expr a0 = select(A, env.in(0));
  Expr nested = select(A, op::bv::extract(kIdx - 1, 0, a0));
  Expr f3 = mk<AND>(mk<EQ>(a0, env.vn(0)), mk<NEQ>(nested, a0));
  int k = 0;
  for (Expr f : {f1, f2, f3}) {
    ++k;
    Ackermannizer ack;
    Expr g = ack.lower(f);
    CHECK(ack.stats().lowered == 1, "lowered " << k);
    CHECK(solve(env, f) == sea::solver::SolverResult::UNSAT,
          "orig unsat " << k);
    CHECK(solve(env, g) == sea::solver::SolverResult::UNSAT,
          "lowered unsat " << k);
    CHECK(solve(env, g, true) == sea::solver::SolverResult::UNSAT,
          "lowered unsat z3 " << k);
  }
  llvm::outs() << "test: aliasing ok\n";
}

void testModel() {
  Env env;
  using op::array::select;
  Expr A = env.arr("A");
  Expr x = env.idx("x"), y = env.idx("y");
  // A[x] = 7, A[y] = 3, x != 0, A[1] = 7: forces x = 1 or not; y != x
  Expr f = mk<AND>(
      mk<AND>(mk<EQ>(select(A, x), env.vn(7)), mk<EQ>(select(A, y), env.vn(3))),
      mk<AND>(mk<EQ>(select(A, env.in(1)), env.vn(7)), mk<NEQ>(x, env.in(1))));
  for (bool z3 : {false, true}) {
    Ackermannizer ack;
    Expr g = ack.lower(f);
    sea::solver::Solver::model_ref m;
    CHECK(solve(env, g, z3, &m) == sea::solver::SolverResult::SAT, "sat");
    if (!m)
      continue;
    AckermannModel am(*m, ack);
    CHECK(isTrue(am.eval(f, true)),
          "original true in the model (z3=" << z3 << ")");
    // unread index: 0, the read that has no read in the VC defaults
    Expr xv = am.eval(x, true), yv = am.eval(y, true);
    long unread = -1;
    for (long i = 0; i < 8; ++i) {
      Expr iv = env.in(i);
      if (i != 1 && iv != xv && iv != yv) {
        unread = i;
        break;
      }
    }
    Expr cell = am.eval(select(A, env.in(unread)), true);
    CHECK(cell == env.vn(0), "unread cell is 0");
    CHECK(am.defaulted() == 1, "defaulted " << am.defaulted());
    CHECK(am.inconsistent() == 0, "consistent");
    // the array as a term: the same function
    Expr eqv = am.eval(
        mk<EQ>(select(mk<ITE>(mk<EQ>(x, x), A, A), env.in(1)), env.vn(7)),
        true);
    CHECK(isTrue(eqv), "array term (ite) agrees");
    // a store over the lowered array, read at the stored and another index
    Expr st = op::array::store(A, y, env.vn(9));
    CHECK(isTrue(am.eval(mk<AND>(mk<EQ>(select(st, y), env.vn(9)),
                                 mk<EQ>(select(st, x), env.vn(7))),
                         true)),
          "store over a lowered array");
    CHECK(am.function(0).size() == 3,
          "function size " << am.function(0).size());
    m.reset();
    lastSolver.reset();
  }
  llvm::outs() << "test: model ok\n";
}

// random formulas over two arrays, 3-bit indices, 4-bit values
void testRandom() {
  std::mt19937 rng(7);
  int sat = 0, unsat = 0;
  for (int round = 0; round < 400; ++round) {
    Env env;
    using op::array::select;
    Expr arrs[2] = {env.arr("A"), env.arr("B")};
    std::vector<Expr> idxs = {env.idx("x"), env.idx("y"), env.idx("z")};
    std::vector<Expr> vals;
    auto pick = [&](size_t n) { return size_t(rng() % n); };
    for (int k = 0; k < 10; ++k) {
      Expr i;
      Expr p1 = idxs[pick(idxs.size())], p2 = idxs[pick(idxs.size())];
      Expr k1 = env.in(long(pick(8))), k2 = env.in(long(pick(8)));
      switch (pick(7)) {
      case 4: // ite of two pointers with an offset in each branch
        i = mk<ITE>(mk<EQ>(p1, k2), mk<BADD>(p1, k1),
                    mk<BADD>(p2, pick(2) ? k1 : k2));
        break;
      case 5:
        i = mk<BSUB>(mk<BADD>(p1, k1), p2);
        break;
      case 6:
        i = mk<BADD>(mk<BMUL>(k1, p1), mk<BNEG>(k2));
        break;
      case 0:
        i = env.in(long(pick(8)));
        break;
      case 1:
        i = idxs[pick(idxs.size())];
        break;
      case 2:
        i = mk<BADD>(idxs[pick(idxs.size())], env.in(long(pick(8))));
        break;
      default: // a read as an index
        i = vals.empty()
                ? env.in(0)
                : op::bv::extract(kIdx - 1, 0, vals[pick(vals.size())]);
      }
      vals.push_back(select(arrs[pick(2)], i));
    }
    std::vector<Expr> lits;
    for (int k = 0; k < 6; ++k) {
      Expr a = vals[pick(vals.size())];
      Expr b = pick(2) ? vals[pick(vals.size())] : env.vn(long(pick(16)));
      lits.push_back(pick(3) ? mk<EQ>(a, b) : mk<NEQ>(a, b));
    }
    for (int k = 0; k < 2; ++k) {
      Expr a = idxs[pick(idxs.size())];
      Expr b = pick(2) ? idxs[pick(idxs.size())] : env.in(long(pick(8)));
      lits.push_back(pick(2) ? mk<EQ>(a, b) : mk<NEQ>(a, b));
    }
    Expr f = mknary<AND>(lits.begin(), lits.end());
    Ackermannizer ack;
    Expr g = ack.lower(f);
    auto ro = solve(env, f);
    sea::solver::Solver::model_ref m;
    bool z3 = round % 2;
    auto rl = solve(env, g, z3, &m);
    CHECK(ro == rl, "round " << round << ": original and lowered disagree");
    if (rl == sea::solver::SolverResult::SAT) {
      ++sat;
      AckermannModel am(*m, ack);
      CHECK(isTrue(am.eval(f, true)),
            "round " << round << ": original false in model");
      CHECK(am.inconsistent() == 0, "round " << round << ": inconsistent");
      m.reset();
      lastSolver.reset();
    } else {
      ++unsat;
    }
  }
  CHECK(sat > 10 && unsat > 10,
        "both verdicts: sat " << sat << " unsat " << unsat);
  llvm::outs() << "random: sat=" << sat << " unsat=" << unsat << "\n";
  llvm::outs() << "test: random ok\n";
}

/// LowBits::of(e, k) = extract(k-1, 0, e) for random 16-bit terms over 8-
/// and 16-bit variables, every k in [1, 16]: `of != extract` unsat (z3).
void testLowBits() {
  std::mt19937 rng(11);
  int checked = 0;
  for (int round = 0; round < 60; ++round) {
    ExprFactory efac;
    auto var = [&](const char *n, unsigned w) {
      return op::bv::bvConst(mkTerm<std::string>(n, efac), w);
    };
    auto num = [&](long v, unsigned w) {
      return op::bv::bvnum(mpz_class(v), w, efac);
    };
    Expr a8 = var("a", 8), b8 = var("b", 8), c = var("c", 16), d = var("d", 16);
    Expr cond = mk<EQ>(var("e", 8), num(3, 8));
    auto pick = [&](size_t n) { return size_t(rng() % n); };
    std::function<Expr(int)> gen = [&](int depth) -> Expr {
      if (depth == 0) {
        switch (pick(5)) {
        case 0:
          return op::bv::zext(a8, 16);
        case 1:
          return op::bv::sext(b8, 16);
        case 2:
          return c;
        case 3:
          return d;
        default:
          return num(long(pick(70000)) - 2000, 16);
        }
      }
      Expr x = gen(depth - 1), y = gen(depth - 1);
      switch (pick(12)) {
      case 0:
        return mk<BADD>(x, y, gen(depth - 1));
      case 1:
        return mk<BSUB>(x, y);
      case 2:
        return mk<BMUL>(x, y);
      case 3:
        return mk<BNEG>(x);
      case 4:
        return mk<BAND>(x, y);
      case 5:
        return mk<BOR>(x, y);
      case 6:
        return mk<BXOR>(x, mk<BNOT>(y));
      case 7:
        return mk<ITE>(cond, x, y);
      case 8:
        return mk<BSHL>(x, num(long(pick(18)), 16));
      case 9:
        return op::bv::concat(op::bv::extract(7, 0, x),
                              op::bv::extract(15, 8, y));
      case 10:
        return op::bv::zext(op::bv::extract(11, 2, x), 16);
      default:
        return mk<BLSHR>(x, y); // not pushed: extract
      }
    };
    Expr e = gen(3);
    LowBits lb;
    for (unsigned k = 1; k <= 16; ++k) {
      Expr low = lb.of(e, k);
      CHECK(lb.width(low) == k, "round " << round << " k " << k << " width");
      Expr q = mk<NEQ>(low, op::bv::extract(k - 1, 0, e));
      sea::solver::z3_solver_impl z(efac);
      z.add(q);
      CHECK(z.check() == sea::solver::SolverResult::UNSAT,
            "round " << round << " k " << k << ": low bits differ");
      ++checked;
    }
  }
  llvm::outs() << "lowbits: checked=" << checked << "\n";
  llvm::outs() << "test: lowbits ok\n";
}

/// Narrowed offsets: 6-bit addresses, an object at base B (a constant) of
/// size 5 (k = 3); every read is at B + offset, the offset a random term
/// over x, y, z. With in(i) = (i - B) <u 5 conjoined for every read index
/// (A-INB for every read), the lowering with the narrowing agrees with the
/// original on sat / unsat (Yices) and its models are models of the
/// original (AckermannModel, consistent).
void testNarrow() {
  constexpr unsigned kW = 6, kV = 4, kN = 5, kK = 3;
  std::mt19937 rng(13);
  int sat = 0, unsat = 0, narrowed = 0, kpairs = 0;
  for (int round = 0; round < 300; ++round) {
    ExprFactory efac;
    using op::array::select;
    auto num = [&](long v, unsigned w) {
      return op::bv::bvnum(mpz_class(v), w, efac);
    };
    Expr A = op::bind::mkConst(
        mkTerm<std::string>("A", efac),
        sort::arrayTy(op::bv::bvsort(kW, efac), op::bv::bvsort(kV, efac)));
    Expr B = op::bv::bvConst(mkTerm<std::string>("B", efac), kW);
    std::vector<Expr> xs = {
        op::bv::bvConst(mkTerm<std::string>("x", efac), kW),
        op::bv::bvConst(mkTerm<std::string>("y", efac), kW),
        op::bv::bvConst(mkTerm<std::string>("z", efac), kW)};
    auto pick = [&](size_t n) { return size_t(rng() % n); };
    NarrowTable table;
    std::vector<Expr> vals, ins;
    for (int r = 0; r < 7; ++r) {
      Expr off;
      Expr x = xs[pick(3)], y = xs[pick(3)];
      switch (pick(5)) {
      case 0:
        off = num(long(pick(8)), kW);
        break;
      case 1:
        off = mk<BADD>(x, num(long(pick(8)), kW));
        break;
      case 2:
        off = mk<BADD>(x, y, num(long(pick(8)), kW));
        break;
      case 3:
        off = mk<ITE>(mk<EQ>(x, y), mk<BADD>(x, num(1, kW)),
                      mk<BSUB>(y, num(long(pick(8)), kW)));
        break;
      default:
        off = mk<BMUL>(x, num(long(pick(4)) + 1, kW));
      }
      Expr idx = pick(4) ? mk<BADD>(B, off) : B;
      table.byIndex.emplace(idx.get(), NarrowTable::Entry{B, kK});
      ins.push_back(mk<BULT>(mk<BSUB>(idx, B), num(kN, kW)));
      vals.push_back(select(A, idx));
    }
    std::vector<Expr> lits(ins.begin(), ins.end());
    for (int k = 0; k < 5; ++k) {
      Expr a = vals[pick(vals.size())];
      Expr b = pick(2) ? vals[pick(vals.size())] : num(long(pick(16)), kV);
      lits.push_back(pick(3) ? mk<EQ>(a, b) : mk<NEQ>(a, b));
    }
    {
      Expr a = xs[pick(3)], b = pick(2) ? xs[pick(3)] : num(long(pick(8)), kW);
      lits.push_back(pick(2) ? mk<EQ>(a, b) : mk<NEQ>(a, b));
    }
    Expr f = mknary<AND>(lits.begin(), lits.end());
    Ackermannizer ack;
    ack.setNarrow(&table);
    Expr g = ack.lower(f);
    narrowed += ack.stats().narrowReads;
    kpairs += ack.stats().narrowPairs;
    sea::solver::yices_solver_impl so(efac);
    so.add(f);
    auto ro = so.check();
    std::unique_ptr<sea::solver::Solver> sl =
        std::make_unique<sea::solver::yices_solver_impl>(efac);
    sl->add(g);
    auto rl = sl->check();
    CHECK(ro == rl, "round " << round << ": original and narrowed disagree");
    if (rl == sea::solver::SolverResult::SAT) {
      ++sat;
      auto m = sl->get_model();
      AckermannModel am(*m, ack);
      CHECK(isTrue(am.eval(f, true)),
            "round " << round << ": original false in model");
      CHECK(am.inconsistent() == 0, "round " << round << ": inconsistent");
    } else {
      ++unsat;
    }
  }
  CHECK(sat > 10 && unsat > 10,
        "both verdicts: sat " << sat << " unsat " << unsat);
  CHECK(kpairs > 100, "k-bit pairs " << kpairs);
  llvm::outs() << "narrow: sat=" << sat << " unsat=" << unsat
               << " reads=" << narrowed << " k-pairs=" << kpairs << "\n";
  llvm::outs() << "test: narrow ok\n";
}

} // namespace

/// Leaf pairs (LeafDecider): indices never equal leaf by leaf through ites,
/// with the layout values of object constants (facts of every model of the
/// query: the layout equations are conjoined, as in sea-vc).
void testLeaf() {
  Env env;
  using op::array::select;
  Expr A = env.arr("A");
  Expr o1 = env.idx("obj1"), o2 = env.idx("obj2");
  Expr b = mk<EQ>(env.idx("c"), env.in(0));
  Expr layout = mk<AND>(mk<EQ>(o1, env.in(1)), mk<EQ>(o2, env.in(4)));
  std::unordered_map<ENode *, mpz_class> values{{o1.get(), mpz_class(1L)},
                                                {o2.get(), mpz_class(4L)}};
  auto lower = [&](Expr f, Ackermannizer &ack) {
    ack.setValues(values);
    return ack.lower(f);
  };
  // o1 + 1 (2) vs o2 + 2 (6): apart; o1 + 3 (4) vs o2 (4): kept
  {
    Expr f = mk<AND>(layout, mk<NEQ>(select(A, mk<BADD>(o1, env.in(1))),
                                     select(A, mk<BADD>(o2, env.in(2)))));
    Ackermannizer ack;
    Expr g = lower(f, ack);
    CHECK(ack.stats().leafPairs == 1 && ack.stats().constraints == 0,
          "layout: apart: leaf " << ack.stats().leafPairs);
    CHECK(solve(env, g) == sea::solver::SolverResult::SAT,
          "layout: apart: sat");
    Expr h = mk<AND>(
        layout, mk<NEQ>(select(A, mk<BADD>(o1, env.in(3))), select(A, o2)));
    Ackermannizer ack2;
    Expr g2 = lower(h, ack2);
    CHECK(ack2.stats().leafPairs == 0 && ack2.stats().constraints == 1,
          "layout: equal cells kept");
    CHECK(solve(env, g2) == sea::solver::SolverResult::UNSAT,
          "layout: o1 + 3 and o2 are one cell");
  }
  // an ite under an addition: ite(b, o1, o2) + 1 (2 or 5) vs o1 + 6 (7):
  // apart; vs o2 + 1 (5): kept
  {
    Expr it = mk<BADD>(mk<ITE>(b, o1, o2), env.in(1));
    Expr f = mk<AND>(
        layout, mk<NEQ>(select(A, it), select(A, mk<BADD>(o1, env.in(6)))));
    Ackermannizer ack;
    lower(f, ack);
    CHECK(ack.stats().leafPairs == 1, "ite leaves apart");
    Expr h =
        mk<AND>(mk<AND>(layout, mk<NEG>(b)),
                mk<NEQ>(select(A, it), select(A, mk<BADD>(o2, env.in(1)))));
    Ackermannizer ack2;
    Expr g2 = lower(h, ack2);
    CHECK(ack2.stats().leafPairs == 0, "ite leaf may meet: kept");
    CHECK(solve(env, g2) == sea::solver::SolverResult::UNSAT,
          "ite leaf meets when b is false");
  }
  // random: indices over two objects with layout values, ites, offsets;
  // original and lowered agree
  std::mt19937 rng(11);
  int dropped = 0;
  for (int round = 0; round < 300; ++round) {
    Env e2;
    Expr B = e2.arr("B");
    Expr p1 = e2.idx("obj1"), p2 = e2.idx("obj2"), x = e2.idx("x");
    Expr lay = mk<AND>(mk<EQ>(p1, e2.in(1)), mk<EQ>(p2, e2.in(4)));
    std::unordered_map<ENode *, mpz_class> vals{{p1.get(), mpz_class(1L)},
                                                {p2.get(), mpz_class(4L)}};
    auto pick = [&](size_t n) { return size_t(rng() % n); };
    auto atomIdx = [&]() -> Expr {
      Expr base = pick(3) == 0 ? x : (pick(2) ? p1 : p2);
      return mk<BADD>(base, e2.in(long(pick(8))));
    };
    std::vector<Expr> vs;
    for (int k = 0; k < 6; ++k) {
      Expr i = atomIdx();
      if (pick(2))
        i = mk<ITE>(mk<EQ>(x, e2.in(long(pick(8)))), i, atomIdx());
      if (pick(3) == 0)
        i = mk<BADD>(i, e2.in(long(pick(8))));
      vs.push_back(select(B, i));
    }
    std::vector<Expr> lits{lay};
    for (int k = 0; k < 4; ++k) {
      Expr a = vs[pick(vs.size())], c = vs[pick(vs.size())];
      lits.push_back(pick(2) ? mk<EQ>(a, c) : mk<NEQ>(a, c));
    }
    Expr f = mknary<AND>(lits.begin(), lits.end());
    Ackermannizer ack;
    ack.setValues(vals);
    Expr g = ack.lower(f);
    dropped += ack.stats().leafPairs;
    CHECK(solve(e2, f) == solve(e2, g, round % 2),
          "leaf random " << round << ": original and lowered disagree");
  }
  CHECK(dropped > 0, "leaf random: some pairs dropped");
  llvm::outs() << "test: leaf ok (dropped " << dropped << ")\n";
}

int main() {
  testSharing();
  testLeaf();
  testKept();
  testAliasing();
  testModel();
  testRandom();
  testLowBits();
  testNarrow();
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
