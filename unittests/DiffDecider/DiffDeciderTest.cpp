// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- DiffDeciderTest.cpp - tests of the difference decider
//---------------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if a
// check fails. Run by test/Expr/diff-decider.test.
//
// What is tested (include/sea/Expr/DiffDecider.hh, BvRange.hh):
//  - shapes seen in the benchmarks, each NEVER with a can-equal sibling (z3
//    checks both: a = b unsat for NEVER, sat for the sibling): residue,
//    interval on zero-extended bytes and masks, congruence + interval, signed
//    sext representatives (c * sext(x) + K), layout substitution, leaf
//    splitting through ite / bvadd / bvsub with the layout (key-selected
//    objects), the leaf budget;
//  - the width: a 64-bit NEVER is not a k-bit NEVER (k = 11, offsets 0 vs
//    2048, and zext bytes 2048 apart); a k-bit NEVER with its sibling;
//  - seeded random terms at 8 bits: every NEVER / ALWAYS confirmed by z3 (with
//    the layout conjoined), every unsigned range of BvRanges confirmed by z3.
//
//===----------------------------------------------------------------------===//

#include "sea/Expr/DiffDecider.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/Smt/Z3SolverImpl.hh"
#include "sea/Expr/TypeChecker.hh"
#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace expr;
using sea::solver::SolverResult;

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

using V = DiffDecider::Verdict;
using C = DiffDecider::Class;
using P = DiffDecider::Premise;

struct Env {
  ExprFactory efac;
  std::unordered_map<ENode *, mpz_class> values;
  std::vector<Expr> keep;
  Expr var(const std::string &n, unsigned w) {
    return op::bv::bvConst(mkTerm<std::string>(n, efac), w);
  }
  Expr num(long v, unsigned w) { return op::bv::bvnum(mpz_class(v), w, efac); }
  Expr numz(const mpz_class &v, unsigned w) {
    return op::bv::bvnum(v, w, efac);
  }
  /// an object constant with a layout value
  Expr obj(const std::string &n, long v, unsigned w) {
    Expr o = var(n, w);
    values[o.get()] = mpz_class(v);
    keep.push_back(o);
    return o;
  }
  Expr zext(const Expr &e, unsigned w) { return op::bv::zext(e, w); }
  Expr sext(const Expr &e, unsigned w) { return op::bv::sext(e, w); }
  /// the layout facts obj = v
  Expr layout() {
    std::vector<Expr> f{mk<TRUE>(efac)};
    for (const Expr &o : keep) {
      unsigned w = op::bv::widthBvConst(o);
      f.push_back(mk<EQ>(o, numz(values[o.get()], w)));
    }
    return f.size() == 1 ? f[0] : mknary<AND>(f.begin(), f.end());
  }
};

SolverResult z3check(Env &env, const Expr &f) {
  sea::solver::z3_solver_impl s(env.efac);
  s.add(f);
  return s.check();
}

/// a = b is unsat (with the layout) / sat
bool neverByZ3(Env &env, const Expr &a, const Expr &b) {
  return z3check(env, mk<AND>(env.layout(), mk<EQ>(a, b))) ==
         SolverResult::UNSAT;
}
bool canEqualByZ3(Env &env, const Expr &a, const Expr &b) {
  return z3check(env, mk<AND>(env.layout(), mk<EQ>(a, b))) == SolverResult::SAT;
}

const char *name(V v) {
  return v == V::Never ? "never" : v == V::Always ? "always" : "unknown";
}
const char *name(C c) {
  switch (c) {
  case C::None:
    return "none";
  case C::Residue:
    return "residue";
  case C::Interval:
    return "interval";
  case C::Congruence:
    return "congruence";
  case C::Signed:
    return "signed";
  }
  return "?";
}

/// A NEVER of class \p cls (premise \p prem, split \p split) for a, b, and
/// a sibling a, c that can be equal (the decider must not say NEVER); both
/// confirmed by z3.
void pair(Env &env, unsigned w, const std::string &what, const Expr &a,
          const Expr &b, const Expr &c, C cls, P prem, bool split,
          DiffDecider::Options opt = DiffDecider::Options()) {
  DiffDecider d(w, &env.values, opt);
  DiffDecider::Result r = d.decide(a.get(), b.get());
  CHECK(r.verdict == V::Never && r.cls == cls && r.premise == prem &&
            r.split == split,
        what << ": " << name(r.verdict) << " " << name(r.cls) << " premise "
             << int(r.premise) << " split " << r.split);
  CHECK(neverByZ3(env, a, b), what << ": z3 says the pair can be equal");
  DiffDecider::Result s = d.decide(a.get(), c.get());
  CHECK(s.verdict != V::Never, what << " (sibling): never");
  CHECK(canEqualByZ3(env, a, c), what << " (sibling): z3 says never equal");
  llvm::outs() << "shape " << what << ": " << name(r.verdict) << " "
               << name(r.cls) << (prem == P::Layout ? " layout" : "")
               << (r.split ? " split" : "") << "; sibling " << name(s.verdict)
               << "\n";
}

void testShapes() {
  constexpr unsigned W = 64;
  Env env;
  Expr p = env.var("p", W);
  Expr b1 = env.var("b1", 8), b2 = env.var("b2", 8);
  Expr x = env.var("x", W), y = env.var("y", W);
  // residue: x * 8 + 2 vs y * 8 + 3 (sibling: y * 8 + 10)
  pair(env, W, "residue", mk<BADD>(mk<BMUL>(env.num(8, W), x), env.num(2, W)),
       mk<BADD>(mk<BMUL>(env.num(8, W), y), env.num(3, W)),
       mk<BADD>(mk<BMUL>(env.num(8, W), y), env.num(10, W)), C::Residue,
       P::None, false);
  // a cursor over loaded bytes: p + zext(b1) + 1 vs p + zext(b2) + 300
  // (sibling + 200: 1 + b1 = 200 + b2 at b1 = 199, b2 = 0)
  pair(env, W, "interval-zext", mk<BADD>(p, env.zext(b1, W), env.num(1, W)),
       mk<BADD>(p, env.zext(b2, W), env.num(300, W)),
       mk<BADD>(p, env.zext(b2, W), env.num(200, W)), C::Interval, P::None,
       false);
  // masked slots: p + (x & 0xf0) vs p + (y & 0xf) + 0x100 (sibling + 0x50)
  pair(env, W, "interval-mask", mk<BADD>(p, mk<BAND>(x, env.num(0xf0, W))),
       mk<BADD>(p, mk<BAND>(y, env.num(0xf, W)), env.num(0x100, W)),
       mk<BADD>(p, mk<BAND>(y, env.num(0xf, W)), env.num(0x50, W)), C::Interval,
       P::None, false);
  // urem / lshr / concat ranges: p + (x urem 16) vs p + concat(b1, b2) >> 4
  // + 16 ... as an interval test: [0, 15] vs [16, 16 + 4095]
  pair(env, W, "interval-urem-lshr", mk<BADD>(p, mk<BUREM>(x, env.num(16, W))),
       mk<BADD>(p,
                env.zext(mk<BLSHR>(op::bv::concat(b1, b2), env.num(4, 16)), W),
                env.num(16, W)),
       mk<BADD>(p,
                env.zext(mk<BLSHR>(op::bv::concat(b1, b2), env.num(4, 16)), W),
                env.num(15, W)),
       C::Interval, P::None, false);
  // congruence + interval: 3 zext(b1) + 1 vs 3 zext(b2) (D = 1 mod 3, in
  // [-764, 766]; sibling 3 zext(b2) + 4: b1 = b2 + 1)
  pair(env, W, "congruence",
       mk<BADD>(mk<BMUL>(env.num(3, W), env.zext(b1, W)), env.num(1, W)),
       mk<BMUL>(env.num(3, W), env.zext(b2, W)),
       mk<BADD>(mk<BMUL>(env.num(3, W), env.zext(b2, W)), env.num(4, W)),
       C::Congruence, P::None, false);
  // congruence needs the interval (width 8): 3a vs 3b + 1, a, b = x, y urem
  // 11 in [0, 10]: D in [-31, 29] holds 0, D = 2 (mod 3), no multiple of
  // 256 in range is 2 mod 3. Near miss 3b + 3 (a = b + 1).
  {
    constexpr unsigned W8 = 8;
    Expr x8 = env.var("x8", W8), y8 = env.var("y8", W8);
    Expr a = mk<BUREM>(x8, env.num(11, W8)), b = mk<BUREM>(y8, env.num(11, W8));
    pair(env, W8, "congruence-needs-interval", mk<BMUL>(env.num(3, W8), a),
         mk<BADD>(mk<BMUL>(env.num(3, W8), b), env.num(1, W8)),
         mk<BADD>(mk<BMUL>(env.num(3, W8), b), env.num(3, W8)), C::Congruence,
         P::None, false);
    // without the interval (x, y unbounded) the congruence alone decides
    // nothing: 3x vs 3y + 1 can be equal
    DiffDecider d(W8, nullptr);
    auto r =
        d.decide(mk<BMUL>(env.num(3, W8), x8).get(),
                 mk<BADD>(mk<BMUL>(env.num(3, W8), y8), env.num(1, W8)).get());
    CHECK(r.verdict == V::Unknown, "congruence without interval");
    CHECK(canEqualByZ3(env, mk<BMUL>(env.num(3, W8), x8),
                       mk<BADD>(mk<BMUL>(env.num(3, W8), y8), env.num(1, W8))),
          "congruence without interval: z3");
  }
  // an nki tensor index: o + 10 sext(i) + 4 vs o + 10 sext(j) + 8 (i, j
  // loaded int32; unsigned, sext spans 2^64; signed, D = 10 (i - j) - 4 is
  // never 0 mod 2^64); sibling + 24: i = j + 2
  Expr i32 = env.var("i", 32), j32 = env.var("j", 32);
  Expr o = env.var("o", W);
  pair(env, W, "signed-sext",
       mk<BADD>(o, mk<BMUL>(env.num(10, W), env.sext(i32, W)), env.num(4, W)),
       mk<BADD>(o, mk<BMUL>(env.num(10, W), env.sext(j32, W)), env.num(8, W)),
       mk<BADD>(o, mk<BMUL>(env.num(10, W), env.sext(j32, W)), env.num(24, W)),
       C::Signed, P::None, false);
  // the same across two objects with the layout: oa + 10 sext(i) + 4 vs
  // ob + 6 (D = 10 sext(i) - 4098, never 0 mod 10); sibling ob + 8 (i = 410)
  Expr oa = env.obj("oa", 0x1000, W), ob = env.obj("ob", 0x2000, W);
  pair(env, W, "signed-sext-layout",
       mk<BADD>(oa, mk<BMUL>(env.num(10, W), env.sext(i32, W)), env.num(4, W)),
       mk<BADD>(ob, env.num(6, W)), mk<BADD>(ob, env.num(8, W)), C::Signed,
       P::Layout, false);
  // the layout alone: oa + zext(b1) vs ob + zext(b2) (0x1000 apart); sibling
  // ob + zext(b2) - 0xf80 (b1 = b2 + 0x80)
  pair(env, W, "layout-interval", mk<BADD>(oa, env.zext(b1, W)),
       mk<BADD>(ob, env.zext(b2, W)),
       mk<BSUB>(mk<BADD>(ob, env.zext(b2, W)), env.num(0xf80, W)), C::Interval,
       P::Layout, false);
  {
    // ALWAYS by the layout: oa + 0x1000 vs ob
    DiffDecider d(W, &env.values);
    auto r = d.decide(mk<BADD>(oa, env.num(0x1000, W)).get(), ob.get());
    CHECK(r.verdict == V::Always && r.premise == P::Layout,
          "always by layout: " << name(r.verdict));
    auto s = d.decide(mk<BADD>(x, env.num(1, W)).get(),
                      mk<BADD>(x, env.num(1, W)).get());
    CHECK(s.verdict == V::Always && s.premise == P::None,
          "always: " << name(s.verdict));
  }
  {
    // the unsplit form first: an ite common to both terms cancels as an atom
    Expr c = mk<EQ>(x, env.num(0, W));
    Expr t = mk<ITE>(c, mk<BADD>(p, env.zext(b1, W)), mk<BMUL>(y, x));
    DiffDecider d(W, &env.values);
    auto r = d.decide(mk<BADD>(t, env.num(1, W)).get(),
                      mk<BADD>(t, env.num(2, W)).get());
    CHECK(r.verdict == V::Never && !r.split, "common ite: " << name(r.verdict));
  }
}

/// Key-selected objects (stellar): ite(k1, ga, ite(k2, gb, gc)) + ite(...) -
/// ite(...) + 20 against ite(k4, ga, gb) + 3, with the layout: leaf by leaf
/// through ite, bvadd and bvsub; 27 leaves per index (above 16).
void testSplit() {
  constexpr unsigned W = 64;
  Env env;
  Expr ga = env.obj("ga", 0x1000, W), gb = env.obj("gb", 0x2000, W),
       gc = env.obj("gc", 0x3000, W);
  Expr x = env.var("x", 8), y = env.var("y", 8), z = env.var("z", 8),
       u = env.var("u", 8);
  auto sel = [&](const Expr &k) {
    return mk<ITE>(mk<EQ>(k, env.num(0, 8)), mk<BADD>(ga, env.num(4, W)),
                   mk<ITE>(mk<EQ>(k, env.num(1, 8)),
                           mk<BADD>(gb, env.num(4, W)),
                           mk<BADD>(gc, env.num(4, W))));
  };
  Expr a = mk<BADD>(mk<BSUB>(mk<BADD>(sel(x), sel(y)), sel(z)), env.num(20, W));
  Expr b = mk<BADD>(sel(u), env.num(3, W));
  // a's leaves are g_i + g_j - g_k + 24, multiples of 0x1000 plus 24; b's
  // are g + 7: never equal. Sibling: sel(u) + 20 (x = y = z = u).
  Expr c = mk<BADD>(sel(u), env.num(20, W));
  pair(env, W, "split-bvsub-layout", a, b, c, C::Residue, P::Layout, true);
  // the budget: at most 4 leaves per node, the index is one leaf: undecided
  DiffDecider::Options small;
  small.maxLeaves = 4;
  DiffDecider d(W, &env.values, small);
  auto r = d.decide(a.get(), b.get());
  CHECK(r.verdict == V::Unknown && r.budget,
        "budget: " << name(r.verdict) << " budget " << r.budget);
  DiffDecider::Options pairs;
  pairs.maxLeafPairs = 26;
  DiffDecider d2(W, &env.values, pairs);
  auto r2 = d2.decide(a.get(), b.get());
  CHECK(r2.verdict == V::Unknown && r2.budget,
        "pair budget: " << name(r2.verdict) << " budget " << r2.budget);
  // the conditions are tracked: ite(x = 0, ga + 1, gb + 2) - ite(x = 0, ga,
  // gb) is 1 or 2, never ga + 1 - gb (leaves under opposite conditions);
  // without the layout
  Expr cx = mk<EQ>(x, env.num(0, 8));
  Expr s1 = mk<ITE>(cx, ga, gb);
  Expr s2 =
      mk<ITE>(cx, mk<BADD>(ga, env.num(1, W)), mk<BADD>(gb, env.num(2, W)));
  DiffDecider d3(W, nullptr);
  auto r3 = d3.decide(mk<BSUB>(s2, s1).get(), env.num(0, W).get());
  CHECK(r3.verdict == V::Never && r3.split && r3.premise == P::None,
        "condition tracking: " << name(r3.verdict));
  auto r4 = d3.decide(mk<BSUB>(s2, s1).get(), env.num(2, W).get());
  CHECK(r4.verdict != V::Never, "condition tracking sibling");
  llvm::outs() << "test: split ok\n";
}

/// The width: the decider runs on the terms compared, at their width. Under
/// --narrow-offsets two reads of one object compare their k-bit offsets; a
/// NEVER of the 64-bit offsets is not one of the k-bit ones.
void testWidth() {
  constexpr unsigned W = 64, K = 11;
  Env env;
  Expr b1 = env.var("b1", 8), b2 = env.var("b2", 8);
  {
    // 0 vs 2048: never at 64 bits, always at 11
    DiffDecider d64(W, nullptr), d11(K, nullptr);
    auto r = d64.decide(env.num(0, W).get(), env.num(2048, W).get());
    auto s = d11.decide(env.num(0, K).get(), env.num(2048, K).get());
    CHECK(r.verdict == V::Never, "0 vs 2048 at 64: " << name(r.verdict));
    CHECK(s.verdict == V::Always, "0 vs 2048 at 11: " << name(s.verdict));
  }
  {
    // zext(b1) vs zext(b2) + 2048: never at 64 bits (an interval), can be
    // equal at 11 bits (b1 = b2)
    Expr a64 = env.zext(b1, W),
         b64 = mk<BADD>(env.zext(b2, W), env.num(2048, W));
    Expr a11 = env.zext(b1, K),
         b11 = mk<BADD>(env.zext(b2, K), env.num(2048, K));
    DiffDecider d64(W, nullptr), d11(K, nullptr);
    auto r = d64.decide(a64.get(), b64.get());
    auto s = d11.decide(a11.get(), b11.get());
    CHECK(r.verdict == V::Never && r.cls == C::Interval,
          "2048 apart at 64: " << name(r.verdict));
    CHECK(neverByZ3(env, a64, b64), "2048 apart at 64: z3");
    CHECK(s.verdict != V::Never, "2048 apart at 11: " << name(s.verdict));
    CHECK(canEqualByZ3(env, a11, b11), "2048 apart at 11: z3");
  }
  // a k-bit NEVER and its sibling
  pair(env, K, "k-bit-interval", env.zext(b1, K),
       mk<BADD>(env.zext(b2, K), env.num(300, K)),
       mk<BADD>(env.zext(b2, K), env.num(200, K)), C::Interval, P::None, false);
  llvm::outs() << "test: width ok\n";
}

/// Random terms at 8 bits over 3-bit variables, objects with layout values
/// and shared ite conditions: every NEVER / ALWAYS is checked by z3, and so
/// is every unsigned range.
void testRandom() {
  constexpr unsigned W = 8, N = 3;
  std::mt19937 rng(11);
  auto pick = [&](size_t n) { return size_t(rng() % n); };
  std::map<std::string, int> seen;
  int ranges = 0;
  for (int round = 0; round < 60; ++round) {
    Env env;
    std::vector<Expr> small = {env.var("a", N), env.var("b", N),
                               env.var("c", N)};
    std::vector<Expr> wide = {env.var("x", W), env.var("y", W),
                              env.obj("o1", long(pick(64)), W),
                              env.obj("o2", long(pick(64)), W)};
    std::vector<Expr> conds = {mk<EQ>(small[0], env.num(0, N)),
                               mk<EQ>(small[1], small[2])};
    std::function<Expr(int)> gen = [&](int depth) -> Expr {
      int k = depth <= 0 ? int(pick(8)) : int(pick(16));
      switch (k) {
      case 0:
        return env.num(long(pick(64)), W);
      case 1:
        return wide[pick(wide.size())];
      case 2:
        return env.zext(small[pick(3)], W);
      case 3:
        return env.sext(small[pick(3)], W);
      case 4:
        return mk<BAND>(wide[pick(2)], env.num(long(pick(64)), W));
      case 5:
        return mk<BUREM>(wide[pick(2)], env.num(long(pick(8)), W));
      case 6:
        return env.zext(op::bv::concat(small[pick(3)], small[pick(3)]), W);
      case 7:
        return mk<BLSHR>(wide[pick(2)], env.num(long(pick(7)), W));
      case 8:
      case 9:
        return mk<BADD>(gen(depth - 1), gen(depth - 1));
      case 10:
        return mk<BSUB>(gen(depth - 1), gen(depth - 1));
      case 11:
        return mk<BMUL>(env.num(long(pick(64)), W), gen(depth - 1));
      case 12:
      case 13:
        return mk<ITE>(conds[pick(2)], gen(depth - 1), gen(depth - 1));
      case 14:
        return mk<BNEG>(gen(depth - 1));
      default:
        return op::bv::extract(
            W - 1, 0, mk<BSHL>(gen(depth - 1), env.num(long(pick(4)), W)));
      }
    };
    DiffDecider::Options opt;
    opt.maxLeaves = 8 + pick(32);
    DiffDecider d(W, &env.values, opt);
    BvRanges rv(&env.values);
    std::vector<Expr> terms;
    for (int t = 0; t < 12; ++t)
      terms.push_back(gen(3));
    for (const Expr &t : terms) {
      BvInterval r = rv.unsignedRange(t);
      if (!r.known)
        continue;
      ++ranges;
      Expr out = mk<OR>(mk<BULT>(t, env.numz(r.lo, W)),
                        mk<BUGT>(t, env.numz(r.hi, W)));
      CHECK(z3check(env, mk<AND>(env.layout(), out)) == SolverResult::UNSAT,
            "round " << round << ": range [" << r.lo.to_string() << ", "
                     << r.hi.to_string() << "] of " << *t);
    }
    for (size_t i = 0; i < terms.size(); ++i)
      for (size_t j = i + 1; j < terms.size(); ++j) {
        auto r = d.decide(terms[i].get(), terms[j].get());
        std::string key = name(r.verdict);
        if (r.verdict == V::Never) {
          key += std::string(".") + name(r.cls) +
                 (r.premise == P::Layout ? ".layout" : "") +
                 (r.split ? ".split" : "");
          CHECK(neverByZ3(env, terms[i], terms[j]),
                "round " << round << ": wrong never (" << key << ") "
                         << *terms[i] << " vs " << *terms[j]);
        } else if (r.verdict == V::Always) {
          CHECK(z3check(env,
                        mk<AND>(env.layout(), mk<NEQ>(terms[i], terms[j]))) ==
                    SolverResult::UNSAT,
                "round " << round << ": wrong always " << *terms[i] << " vs "
                         << *terms[j]);
        }
        ++seen[key];
      }
  }
  for (const auto &[k, n] : seen)
    llvm::outs() << "random " << k << ": " << n << "\n";
  llvm::outs() << "random ranges: " << ranges << "\n";
  CHECK(seen["never.interval"] > 0 && seen["never.congruence"] > 0 &&
            seen["never.signed"] + seen["never.signed.layout"] > 0,
        "every class fires");
  llvm::outs() << "test: random ok\n";
}

} // namespace

/// bvsub is binary: a 3-ary bvsub is an atom of the linear forms (not
/// a - b - c, which the marshallers would read as a - b) and is ill-sorted.
void testNarySub() {
  constexpr unsigned W = 8;
  Env env;
  Expr a = env.var("a", W), b = env.var("b", W), c = env.var("c", W);
  Expr s3 = mk<BSUB>(a, b, c);
  ackermann_detail::LinearForms lf(W);
  const ackermann_detail::Linear &l3 = lf.of(&*s3);
  CHECK(l3.coef.size() == 1 && l3.coef.count(&*s3) == 1, "3-ary bvsub: atom");
  Expr s2 = mk<BSUB>(a, b);
  CHECK(lf.of(&*s2).coef.size() == 2, "2-ary bvsub: a - b");
  TypeChecker tc;
  CHECK(tc.typeOf(s3) == sort::errorTy(env.efac), "3-ary bvsub: ill-sorted");
  CHECK(tc.typeOf(s2) != sort::errorTy(env.efac), "2-ary bvsub: sorted");
}

int main() {
  testNarySub();
  testShapes();
  llvm::outs() << "test: shapes ok\n";
  testSplit();
  testWidth();
  testRandom();
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
