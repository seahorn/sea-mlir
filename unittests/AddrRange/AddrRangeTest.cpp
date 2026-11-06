// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- AddrRangeTest.cpp - tests of ARM (AddrRangeMap.hh) -----------------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if a
// check fails. Run by test/Expr/addr-range.test.
//
//  lattice    AddrRange and AddrRangeMap operations (thesis Fig. 5.3, 5.9):
//             join, meet, add, subset; overflow gives top; bottom and top.
//  thesis     Fig. 5.4 (the scalar s1 = [21, 58]), Fig. 5.7 (p1, p2, and the
//             converse of Thm 4 that does not hold), the Ch. 3 pointer
//             obj8 against ite(a, obj16, obj16 + 8).
//  thm4       disjointness on terms: same base (no assumption), different
//             bases (A-INB only when asked), absolute numbers, symbolic
//             addends, top (a base mentioned through another operator), a
//             size-0 object, wraparound at 8 bits, a deep ite chain.
//  semantics  seeded random terms over 8-bit pointers, evaluated under many
//             assignments: every value is in gamma(alpha(term)); a pair that
//             is disjoint without assumptions never has equal values; a pair
//             that is disjoint with A-INB never has equal values on the
//             assignments where the accessed leaves lie in their objects
//             (adjacent objects are in the layout: a leaf of b that lands
//             in a is not in bounds and must not count).
// `--mutant=N` makes one lattice operation wrong (arm_test::mutant): the run
// must fail (test/Expr/addr-range.test checks that every mutant is caught).
//
//===----------------------------------------------------------------------===//

// boost hash templates are only forward-declared by Expr.hh; define them here
// (a Release build does not export the instantiations from libSeaExpr).
#include "sea/Expr/AddrRangeMap.hh"
#include "sea/Expr/Expr.hh"
#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>

using namespace expr;
using llvm::APInt;

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

std::string str(const AddrRange &r) {
  std::string s;
  llvm::raw_string_ostream os(s);
  r.print(os);
  return s;
}
std::string str(const AddrRangeMap &r) {
  std::string s;
  llvm::raw_string_ostream os(s);
  r.print(os);
  return s;
}

//===----------------------------------------------------------------------===//
// lattice
//===----------------------------------------------------------------------===//

/// the interval of a key (bottom if the map has none: a failed check, no crash)
AddrRange at(const AddrRangeMap &m, unsigned key) {
  const AddrRange *r = m.find(key);
  return r ? *r : AddrRange::bot();
}
AddrRange R(unsigned w, int64_t lo, int64_t hi) {
  return AddrRange::range(APInt(w, lo, true), APInt(w, hi, true));
}
AddrRange P(unsigned w, int64_t v) {
  return AddrRange::point(APInt(w, v, true));
}

void testRange() {
  for (unsigned w : {8u, 64u}) {
    AddrRange bot = AddrRange::bot(), top = AddrRange::top();
    // join: hull; bottom is the identity; top absorbs
    CHECK(P(w, 4).join(P(w, 8)) == R(w, 4, 8), "join of points");
    CHECK(R(w, 0, 3).join(R(w, 10, 12)) == R(w, 0, 12), "join hull");
    CHECK(R(w, -5, 3).join(R(w, -9, -7)) == R(w, -9, 3), "join negative");
    CHECK(bot.join(R(w, 1, 2)) == R(w, 1, 2) &&
              R(w, 1, 2).join(bot) == R(w, 1, 2),
          "bottom is the identity of join");
    CHECK(top.join(R(w, 1, 2)).isTop() && R(w, 1, 2).join(top).isTop(),
          "top absorbs join");
    CHECK(bot.join(bot).isBot(), "bottom join bottom");
    // join is an upper bound
    CHECK(R(w, 1, 2).subsetOf(R(w, 1, 2).join(R(w, 7, 9))) &&
              R(w, 7, 9).subsetOf(R(w, 1, 2).join(R(w, 7, 9))),
          "join is an upper bound");
    // meet
    CHECK(R(w, 0, 8).meet(R(w, 8, 12)) == P(w, 8), "touching intervals meet");
    CHECK(R(w, 0, 7).meet(R(w, 8, 12)).isBot(), "disjoint intervals");
    CHECK(R(w, 0, 20).meet(R(w, 4, 8)) == R(w, 4, 8), "meet inside");
    CHECK(R(w, -4, 4).meet(R(w, 0, 100)) == R(w, 0, 4), "meet across zero");
    CHECK(top.meet(R(w, 1, 2)) == R(w, 1, 2) &&
              R(w, 1, 2).meet(top) == R(w, 1, 2),
          "top is the identity of meet");
    CHECK(bot.meet(R(w, 1, 2)).isBot() && R(w, 1, 2).meet(bot).isBot() &&
              bot.meet(top).isBot(),
          "bottom absorbs meet");
    // add
    CHECK(P(w, 4).add(R(w, 8, 16)) == R(w, 12, 20), "add");
    CHECK(R(w, -3, 3).add(R(w, -1, 5)) == R(w, -4, 8), "add negative");
    CHECK(bot.add(R(w, 1, 2)).isBot() && R(w, 1, 2).add(bot).isBot(),
          "bottom absorbs add");
    CHECK(top.add(R(w, 1, 2)).isTop() && R(w, 1, 2).add(top).isTop(),
          "top absorbs add");
    CHECK(top.add(bot).isBot(),
          "top + bottom is bottom (no sum of an empty set)");
    // subset
    CHECK(bot.subsetOf(P(w, 1)) && P(w, 1).subsetOf(top) && top.subsetOf(top) &&
              bot.subsetOf(bot),
          "subset: bottom and top");
    CHECK(!top.subsetOf(R(w, 0, 100)) && !P(w, 1).subsetOf(bot), "subset: not");
    CHECK(R(w, 2, 3).subsetOf(R(w, 1, 4)) && !R(w, 0, 3).subsetOf(R(w, 1, 4)) &&
              !R(w, 2, 5).subsetOf(R(w, 1, 4)),
          "subset: intervals");
    // contains
    CHECK(R(w, -2, 2).contains(APInt(w, -1, true)) &&
              !R(w, -2, 2).contains(APInt(w, 3, true)) &&
              top.contains(APInt(w, 5)) && !bot.contains(APInt(w, 5)),
          "contains");
    // the whole signed range is top
    CHECK(AddrRange::range(APInt::getSignedMinValue(w),
                           APInt::getSignedMaxValue(w))
              .isTop(),
          "the full range is top");
  }
  // overflow: the interval goes to top, it does not wrap
  CHECK(P(8, 100).add(P(8, 100)).isTop(), "overflow above");
  CHECK(P(8, -100).add(P(8, -100)).isTop(), "overflow below");
  CHECK(P(8, -128).add(P(8, -1)).isTop(), "overflow below at the minimum");
  CHECK(P(8, 127).add(P(8, 1)).isTop(), "overflow above at the maximum");
  CHECK(R(8, 0, 100).add(R(8, 0, 27)) == R(8, 0, 127),
        "no overflow at the maximum");
  CHECK(R(8, -100, 0).add(R(8, -28, 0)) == R(8, -128, 0),
        "no overflow at the minimum");
  CHECK(R(8, 0, 100).add(R(8, 0, 28)).isTop(), "one past the maximum: top");
  CHECK(P(64, INT64_MAX).add(P(64, 1)).isTop(), "64-bit overflow");
  CHECK(R(64, 0, INT64_MAX - 1).add(P(64, 1)) == R(64, 1, INT64_MAX),
        "64-bit edge");
}

void testMap() {
  const unsigned w = 8;
  auto B = [&](unsigned k, int64_t lo, int64_t hi) {
    return AddrRangeMap::of(k, R(w, lo, hi));
  };
  AddrRangeMap top = AddrRangeMap::top(), bot = AddrRangeMap::bot();
  CHECK(bot.isBot() && !top.isBot() && top.isTop() && !bot.isTop(),
        "top / bottom");
  CHECK(AddrRangeMap::of(1, AddrRange::bot()).isBot(),
        "a bottom entry is dropped");

  // join: union of the keys, join of the common
  AddrRangeMap j = B(1, 8, 8).join(B(2, 24, 32));
  CHECK(j.size() == 2 && at(j, 1) == P(w, 8) && at(j, 2) == R(w, 24, 32),
        "join disjoint keys: " << str(j));
  AddrRangeMap j2 = j.join(B(2, 8, 16));
  CHECK(at(j2, 2) == R(w, 8, 32) && at(j2, 1) == P(w, 8),
        "join common key (thesis Sec. 5.2): " << str(j2));
  CHECK(bot.join(j) == j && j.join(bot) == j, "bottom is the identity of join");
  CHECK(top.join(j).isTop() && j.join(top).isTop(), "top absorbs join");
  CHECK(j.subsetOf(j2) && !j2.subsetOf(j), "join is an upper bound");

  // add (+#): every entry; absolute + base = base; base + base = top
  AddrRangeMap a = AddrRangeMap::absolute(R(w, 8, 16));
  AddrRangeMap s = B(3, 16, 16).add(a);
  CHECK(s == B(3, 24, 32),
        "thesis: {obj8: [16,16]} + [8,16] = {obj8: [24,32]}: " << str(s));
  CHECK(a.add(B(3, 16, 16)) == B(3, 24, 32), "add commutes (absolute first)");
  CHECK(B(1, 0, 0).add(B(2, 0, 0)).isTop(), "base + base is top");
  CHECK(B(1, 0, 0).add(top).isTop(), "base + top is top");
  CHECK(B(1, 0, 0).add(bot).isBot(), "base + bottom is bottom");
  AddrRangeMap mix = B(1, 0, 0).join(AddrRangeMap::absolute(P(w, 5)));
  AddrRangeMap ms = mix.add(AddrRangeMap::absolute(P(w, 1)));
  CHECK(at(ms, 1) == P(w, 1) && at(ms, AddrRangeMap::kAbs) == P(w, 6) &&
            ms.size() == 2,
        "mixed map + number: " << str(ms));
  // a sum with overflow in one entry is top for that base, not for the map
  AddrRangeMap ov =
      B(1, 0, 0).join(B(2, 100, 100)).add(AddrRangeMap::absolute(P(w, 100)));
  CHECK(at(ov, 1) == P(w, 100) && at(ov, 2).isTop(),
        "overflow in one entry only: " << str(ov));

  // meet: common keys
  AddrRangeMap m =
      B(1, 0, 8).join(B(2, 0, 4)).meet(B(2, 4, 9).join(B(3, 0, 0)));
  CHECK(m.size() == 1 && at(m, 2) == P(w, 4), "meet: " << str(m));
  CHECK(B(1, 0, 0).meet(B(2, 0, 0)).isBot(), "no common key: bottom");
  CHECK(B(1, 0, 3).meet(B(1, 5, 8)).isBot(), "common key, disjoint: bottom");
  CHECK(top.meet(j) == j && j.meet(top) == j, "top is the identity of meet");
  CHECK(bot.meet(j).isBot(), "bottom absorbs meet");
  CHECK(j.subsetOf(top) && bot.subsetOf(j) && !top.subsetOf(j), "subset");

  auto nz = [](unsigned) { return true; };
  auto zero2 = [](unsigned k) { return k != 2; };
  // disjoint: same key, no assumption
  CHECK(AddrRangeMap::disjoint(B(1, 0, 4), B(1, 8, 8), false, nz), "same key");
  CHECK(!AddrRangeMap::disjoint(B(1, 0, 8), B(1, 8, 12), false, nz),
        "touching");
  CHECK(!AddrRangeMap::disjoint(B(1, 0, 0), B(1, 0, 0), true, nz), "equal");
  // different keys: A-INB only
  CHECK(!AddrRangeMap::disjoint(B(1, 0, 0), B(2, 0, 0), false, nz),
        "different bases without A-INB");
  CHECK(AddrRangeMap::disjoint(B(1, 0, 0), B(2, 0, 0), true, nz),
        "different bases with A-INB");
  // a size-0 object (SeaM-null.md): under A-INB (an access of positive
  // width) its entry is vacuous and dropped; never without A-INB
  {
    bool cross = false, zero = false;
    CHECK(AddrRangeMap::disjoint(B(1, 0, 0), B(2, 0, 0), true, zero2, &cross,
                                 &zero) &&
              zero,
          "a size-0 object: vacuous under A-INB");
    CHECK(!AddrRangeMap::disjoint(B(1, 0, 0), B(2, 0, 0), false, zero2),
          "a size-0 object: never different without A-INB");
    // {1, 2} against {1}: the size-0 entry 2 is dropped, 1 against 1 decides
    bool z2 = false;
    CHECK(!AddrRangeMap::disjoint(B(1, 0, 0).join(B(2, 0, 0)), B(1, 0, 0), true,
                                  zero2, nullptr, &z2),
          "a size-0 entry dropped, the rest meets");
    bool z3 = false;
    CHECK(AddrRangeMap::disjoint(B(1, 0, 0).join(B(2, 0, 0)), B(1, 8, 8), true,
                                 zero2, nullptr, &z3) &&
              z3,
          "a size-0 entry dropped, the rest apart");
  }
  // same keys, several: cross pairs need A-INB
  AddrRangeMap p12 = B(1, 0, 0).join(B(2, 0, 0)),
               q12 = B(1, 8, 8).join(B(2, 8, 8));
  CHECK(!AddrRangeMap::disjoint(p12, q12, false, nz) &&
            AddrRangeMap::disjoint(p12, q12, true, nz),
        "{1, 2} against {1, 2}: only with A-INB");
  // absolute
  AddrRangeMap a5 = AddrRangeMap::absolute(P(w, 5)),
               a7 = AddrRangeMap::absolute(P(w, 7));
  CHECK(AddrRangeMap::disjoint(a5, a7, false, nz), "two numbers");
  CHECK(!AddrRangeMap::disjoint(a5, a5, false, nz), "equal numbers");
  CHECK(!AddrRangeMap::disjoint(a5, B(1, 0, 0), true, nz) &&
            !AddrRangeMap::disjoint(B(1, 0, 0), a5, true, nz),
        "a number may be anywhere: not different from a base");
  CHECK(!AddrRangeMap::disjoint(mix, B(2, 0, 0), true, nz),
        "an absolute part is a wildcard");
  CHECK(!AddrRangeMap::disjoint(top, B(1, 0, 0), true, nz) &&
            !AddrRangeMap::disjoint(B(1, 0, 0), top, true, nz),
        "top");
  CHECK(AddrRangeMap::disjoint(bot, B(1, 0, 0), false, nz), "bottom: vacuous");
}

//===----------------------------------------------------------------------===//
// terms
//===----------------------------------------------------------------------===//

struct Fixture {
  unsigned w;
  ExprFactory efac;
  BaseRegistry reg;
  Expr t, f;

  explicit Fixture(unsigned w) : w(w), t(mk<TRUE>(efac)), f(mk<FALSE>(efac)) {}

  /// an object's base: a pointer constant (PtrSort S2e), its erased twin
  /// an alias
  Expr base(const char *name, uint64_t b, uint64_t size) {
    Expr e = op::ptr::ptrConst(mkTerm<std::string>(name, efac), w);
    reg.add(e, name, "heap", APInt(w, b), APInt(w, size), 1);
    reg.addAlias(op::bv::bvConst(mkTerm<std::string>(name, efac), w), e);
    return e;
  }
  Expr num(int64_t n) {
    mpz_class m, v((long)n);
    mpz_ui_pow_ui(m.get_mpz_t(), 2, w);
    mpz_fdiv_r(v.get_mpz_t(), v.get_mpz_t(), m.get_mpz_t());
    return op::bv::bvnum(v, w, efac);
  }
  Expr var(const char *n) {
    return op::bv::bvConst(mkTerm<std::string>(n, efac), w);
  }
  Expr cond(const char *n) {
    return op::bind::boolConst(mkTerm<std::string>(n, efac));
  }
  /// a sum; over a pointer (the first argument) a node per offset
  /// (op::ptr::addressPlus, as OpSem's addr.add)
  Expr add(std::initializer_list<Expr> args) {
    ExprVector v(args);
    if (isPtr(v.front())) {
      Expr p = v.front();
      for (size_t i = 1; i < v.size(); ++i)
        p = op::ptr::addressPlus(p, v[i]);
      return p;
    }
    return mknary<BADD>(v.begin(), v.end());
  }
  static bool isPtr(Expr e) {
    while (isOpX<ITE>(e))
      e = e->arg(1);
    return op::ptr::isPtrConst(e) || op::ptr::isPtrIndex(e) ||
           isOpX<PTR_FROM_BITS>(e) || isOpX<PTR_FROM_INT>(e);
  }
  static bool wellTyped(const Expr &e) {
    TypeChecker tc;
    return !isOpX<ERROR_TY>(tc.typeOf(e));
  }
  Expr sel(const Expr &mem, const Expr &i) { return op::array::select(mem, i); }
};

/// One test over the terms: the registry has obj8 (8 bytes), obj16 (16), a
/// third object and a size-0 object at the third's base.
struct Arm {
  Fixture F;
  AddrRangeAnalysis an;
  Expr obj8, obj16, obj4, objz, x, a, b, c;
  Expr mem;

  explicit Arm(unsigned w = 64) : F(w), an(F.reg) {
    uint64_t u = w == 8 ? 0x10 : 0x1000;
    obj8 = F.base("obj8", u, 8);
    obj16 = F.base("obj16", 2 * u, 16);
    obj4 = F.base("obj4", 3 * u, 4);
    objz = F.base("objz", 3 * u, 0);
    x = F.var("x");
    a = F.cond("a");
    b = F.cond("b");
    c = F.cond("c");
    mem = op::bind::mkConst(
        mkTerm<std::string>("m", F.efac),
        sort::arrayTy(op::ptr::ptrsort(w, F.efac), op::bv::bvsort(w, F.efac)));
  }
  Expr ite(const Expr &cnd, const Expr &p, const Expr &q) {
    return mk<ITE>(cnd, p, q);
  }
  Expr n(int64_t v) { return F.num(v); }
  Expr plus(const Expr &p, int64_t v) { return F.add({p, n(v)}); }
  /// different, with A-INB / without
  bool diffInb(const Expr &p, const Expr &q) { return an.disjoint(p, q, true); }
  bool diff(const Expr &p, const Expr &q) { return an.disjoint(p, q, false); }
  std::string show(const Expr &p) { return str(an.alpha(p)); }
};

void testThesis() {
  Arm A;
  // Fig. 5.4: s1 = ite(i, 21, 42 + ite(j, 8, 16)); alpha = [21, 58]
  Expr s1 =
      A.ite(A.a, A.n(21), A.F.add({A.n(42), A.ite(A.b, A.n(8), A.n(16))}));
  const AddrRangeMap &m1 = A.an.alpha(s1);
  CHECK(m1.size() == 1 && m1.find(AddrRangeMap::kAbs) &&
            at(m1, AddrRangeMap::kAbs) == R(64, 21, 58),
        "Fig 5.4: " << str(m1));
  // Fig. 5.7: p1 = ite(i, ptr(obj16, 8), ptr(obj8, 16) + ite(j, 8, 16)) =
  // {obj16: [8, 8], obj8: [24, 32]}
  Expr p1 = A.ite(A.a, A.plus(A.obj16, 8),
                  A.F.add({A.plus(A.obj8, 16), A.ite(A.b, A.n(8), A.n(16))}));
  const ObjectBase *o8 = A.F.reg.lookup(A.obj8), *o16 = A.F.reg.lookup(A.obj16);
  const AddrRangeMap &mp1 = A.an.alpha(p1);
  CHECK(mp1.size() == 2 && at(mp1, o16->order) == P(64, 8) &&
            at(mp1, o8->order) == R(64, 24, 32),
        "Fig 5.7: " << str(mp1));
  // p2 = ite(k, obj8, obj16): {obj8: [0, 0], obj16: [0, 0]}; the meet is bottom
  Expr p2 = A.ite(A.c, A.obj8, A.obj16);
  CHECK(A.an.alpha(p1).meet(A.an.alpha(p2)).isBot(),
        "Fig 5.7: p1 meet p2 = bottom");
  // Thm 4: p1 != p2 (cross bases: A-INB)
  CHECK(A.diffInb(p1, p2) && A.diffInb(p2, p1), "Thm 4: p1 != p2");
  CHECK(!A.diff(p1, p2), "without A-INB the cross bases are not separated");
  // the converse does not hold: p1' with the condition negated has the same
  // ARM, p1 != p1' is not proved (and is false: they are different... or not,
  // depending on the condition, which ARM ignores)
  Expr p1n = A.ite(mk<NEG>(A.a), A.plus(A.obj16, 8),
                   A.F.add({A.plus(A.obj8, 16), A.ite(A.b, A.n(8), A.n(16))}));
  CHECK(A.an.alpha(p1n) == A.an.alpha(p1), "negated condition: the same ARM");
  CHECK(!A.diffInb(p1, p1n), "the converse of Thm 4 does not hold");
  // Ch. 3: obj8 against ite(a, obj16, obj16 + 8)
  Expr q = A.ite(A.a, A.obj16, A.plus(A.obj16, 8));
  CHECK(A.diffInb(A.obj8, q) && A.diffInb(q, A.obj8),
        "Ch. 3: obj8 != ite(a, obj16, obj16 + 8)");
  CHECK(!A.diff(A.obj8, q), "Ch. 3 needs A-INB");
  CHECK(!A.diffInb(A.obj16, q), "obj16 may be equal to the ite");
  CHECK(!A.diffInb(A.plus(A.obj16, 8), q), "obj16 + 8 may be equal to the ite");
  CHECK(!A.diffInb(A.plus(A.obj16, 4), q),
        "obj16 + 4 is in the hull [0, 8] of the ite: not decided");
  CHECK(A.diffInb(A.plus(A.obj16, 12), q), "obj16 + 12 is outside the hull");
  CHECK(A.diff(A.plus(A.obj16, 12), q), "... same base: no assumption");
}

void testThm4() {
  Arm A;
  // same base: no assumption
  CHECK(A.diff(A.plus(A.obj16, 4), A.plus(A.obj16, 8)), "same base, numerals");
  Expr lo = A.ite(A.a, A.obj16, A.plus(A.obj16, 4)),
       hi = A.ite(A.b, A.plus(A.obj16, 8), A.plus(A.obj16, 12));
  CHECK(A.diff(lo, hi) && A.diff(hi, lo),
        "same base, ite intervals [0,4] [8,12]");
  Expr touch = A.ite(A.b, A.plus(A.obj16, 4), A.plus(A.obj16, 12));
  CHECK(!A.diff(lo, touch), "same base, intervals [0,4] [4,12] meet");
  // the interval is a hull: 6 is in [4, 8] although it is not a leaf (no
  // precision claimed, only soundness)
  CHECK(!A.diff(A.ite(A.a, A.plus(A.obj16, 4), A.plus(A.obj16, 8)),
                A.plus(A.obj16, 6)),
        "the hull is not decided");
  // symbolic addend: top for that base only
  Expr sym = A.F.add({A.obj8, A.x});
  CHECK(A.show(sym) == "{#0: T}", "base + x: " << A.show(sym));
  CHECK(A.diffInb(sym, A.plus(A.obj16, 8)), "obj8 + x != obj16 + 8 (A-INB)");
  CHECK(!A.diff(sym, A.plus(A.obj16, 8)), "... not without");
  CHECK(!A.diffInb(sym, A.plus(A.obj8, 8)), "obj8 + x may be obj8 + 8");
  // a symbolic sum with no base: an unknown number; it may be anywhere
  CHECK(!A.diffInb(A.x, A.obj8) && !A.diffInb(A.obj8, A.F.sel(A.mem, A.n(0))),
        "an unknown number or a loaded value is not different from a base");
  CHECK(!A.diffInb(A.F.add({A.x, A.n(8)}), A.plus(A.obj8, 8)), "x + 8");
  // a loaded offset is covered by the access condition (A-INB), as in the view
  Expr ld = A.F.sel(A.mem, A.n(16));
  CHECK(A.diffInb(A.F.add({A.obj8, ld}), A.plus(A.obj16, 8)),
        "obj8 + load != obj16 + 8: a loaded offset keeps the base (A-INB)");
  // numbers
  Expr ab = A.ite(A.a, A.n(5), A.n(7));
  CHECK(A.diff(ab, A.n(9)) && A.diff(A.n(3), ab), "numbers, no assumption");
  CHECK(!A.diff(ab, A.n(6)) && !A.diff(ab, A.n(7)),
        "numbers that may be equal");
  // top: a base mentioned through bvsub, or bvadd of two bases
  Expr cast = A.F.add(
      {A.obj8, mk<BSUB>(A.obj16, A.obj8)}); // = obj16, no pointer into obj8
  CHECK(A.an.alpha(cast).isTop(),
        "obj8 + (obj16 - obj8) is top: " << A.show(cast));
  CHECK(!A.diffInb(cast, A.plus(A.obj16, 8)) && !A.diffInb(cast, A.obj16),
        "top is never different");
  Expr two = A.F.add({A.obj8, A.obj16});
  CHECK(A.an.alpha(two).isTop(), "two bases: top");
  Expr masked = mk<BAND>(A.obj8, A.n(~7));
  CHECK(A.an.alpha(masked).isTop(), "a mask of a base: top");
  Expr product = mk<BMUL>(A.x, A.n(4));
  CHECK(A.an.alpha(product) == AddrRangeMap::absolute(AddrRange::top()),
        "x * 4: an unknown number");
  CHECK(A.an.alpha(A.F.add({A.obj8, product})) ==
            AddrRangeMap::of(A.F.reg.lookup(A.obj8)->order, AddrRange::top()),
        "base + x * 4: top for the base");
  // an ite that mixes a pointer and a number: the number is a wildcard
  Expr mix = A.ite(A.a, A.obj8, A.n(5));
  CHECK(!A.diffInb(mix, A.obj16) && !A.diffInb(mix, A.n(7)),
        "pointer-or-number: the number may be anywhere, the pointer is not a "
        "number");
  // size-0 object: not different from its neighbour at the same base by
  // value (no claim without A-INB), but an access of positive width is never
  // in it: vacuous under A-INB (SeaM-null.md)
  CHECK(!A.diff(A.objz, A.obj4) && !A.diff(A.plus(A.obj4, 2), A.objz),
        "size 0: no claim without A-INB");
  CHECK(A.diffInb(A.objz, A.obj4) && A.diffInb(A.objz, A.obj8),
        "size 0: vacuous under A-INB");
  // overflow inside an ite: top for the base only. Numerals fold into a
  // node's constant mod 2^w (addressPlus), so the overflow is in the index
  // ranges: two indices in [0, INT64_MAX] each
  Expr hiIdx = A.ite(A.b, A.n(INT64_MAX), A.n(0));
  Expr big = A.ite(A.a, A.plus(A.obj8, 4), A.F.add({A.obj16, hiIdx, hiIdx}));
  CHECK(at(A.an.alpha(big), A.F.reg.lookup(A.obj16)->order).isTop(),
        "offsets whose ranges overflow: top for that base: " << A.show(big));
  // caching: once per node
  uint64_t n0 = A.an.nodes();
  A.an.alpha(big);
  CHECK(A.an.nodes() == n0, "alpha is cached per node");
  // a deep ite chain (iterative: no stack overflow)
  Expr chain = A.plus(A.obj8, 0);
  for (int i = 0; i < 3000; ++i)
    chain = A.ite(A.F.cond(("c" + std::to_string(i)).c_str()),
                  A.plus(A.obj16, i % 8), chain);
  const AddrRangeMap &ch = A.an.alpha(chain);
  CHECK(ch.size() == 2 && at(ch, A.F.reg.lookup(A.obj16)->order) == R(64, 0, 7),
        "deep chain: " << str(ch));
  CHECK(A.diffInb(chain, A.plus(A.obj4, 1)), "deep chain against obj4");

  // nodes and markers (PtrSort S2a): the map of the sum a node erases to
  using op::PtrOpKind;
  Expr nd8 = op::ptr::ptrIndex(PtrOpKind::PTR_INDEX_NUSW, A.obj16, {}, 8, 0);
  CHECK(A.an.alpha(nd8) == A.an.alpha(A.plus(A.obj16, 8)),
        "node(obj16, c = 8) = obj16 + 8: " << A.show(nd8));
  Expr ndk = op::ptr::ptrIndex(PtrOpKind::PTR_INDEX, A.obj16, {}, 4, 4);
  CHECK(A.an.alpha(ndk) == A.an.alpha(A.plus(A.obj16, 8)), "c + K");
  Expr ndx = op::ptr::ptrIndex(PtrOpKind::PTR_INDEX, A.obj8, {{A.x, 4}}, 0, 0);
  CHECK(A.an.alpha(ndx) == A.an.alpha(A.F.add({A.obj8, product})),
        "node(obj8, x * 4) = obj8 + x * 4: " << A.show(ndx));
  Expr ab1 = op::ptr::ptrIndex(PtrOpKind::PTR_INDEX, A.obj16, {{ab, 1}}, 0, 0);
  CHECK(at(A.an.alpha(ab1), A.F.reg.lookup(A.obj16)->order) == R(64, 5, 7),
        "stride 1: the index's interval: " << A.show(ab1));
  Expr ndb =
      op::ptr::ptrIndex(PtrOpKind::PTR_INDEX, A.obj8, {{A.obj16, 2}}, 0, 0);
  CHECK(A.an.alpha(ndb).isTop(), "a base in a product: top");
  CHECK(A.diffInb(ndx, nd8), "node(obj8, x * 4) != node(obj16, 8) (A-INB)");
  CHECK(A.an.alpha(mk<ADDR>(nd8)) == A.an.alpha(nd8),
        "an observation is its argument");
  CHECK(A.an.alpha(mk<PTR_FROM_INT>(A.x)) ==
            AddrRangeMap::absolute(AddrRange::top()),
        "a constructor has no provenance");
}

void testWrap8() {
  Arm A(8);
  // at 8 bits: -1 and 1 are different offsets; 127 + 1 overflows
  CHECK(A.diff(A.plus(A.obj16, 0xff), A.plus(A.obj16, 1)),
        "obj + (-1) != obj + 1");
  CHECK(A.diff(A.plus(A.obj16, 0x80), A.plus(A.obj16, 0x7f)), "-128 vs 127");
  // numerals fold into the node's constant mod 256 (addressPlus): 0x7f +
  // 0x7f is 0xfe = -2, exactly
  Expr s = A.F.add({A.obj16, A.n(0x7f), A.n(0x7f)});
  CHECK(A.show(s) == "{#1: [-2, -2]}", "folded sum: " << A.show(s));
  CHECK(!A.diff(s, A.plus(A.obj16, 0xfe)), "the wrapped sum is obj + 0xfe");
  // the ranges of two indices overflow: top for the base
  Expr hi = A.ite(A.a, A.n(0x7f), A.n(0));
  Expr so = A.F.add({A.obj16, hi, hi});
  CHECK(at(A.an.alpha(so), A.F.reg.lookup(A.obj16)->order).isTop(),
        "overflowing index ranges: top for the base: " << A.show(so));
  Expr two = A.ite(A.a, A.plus(A.obj16, 0x7f), A.plus(A.obj16, 0x80));
  CHECK(A.show(two) == "{#1: [-128, 127]}" || A.show(two) == "{#1: T}",
        "hull of 127 and -128: " << A.show(two));
  CHECK(!A.diff(two, A.plus(A.obj16, 3)), "a full hull may be anything");
}

//===----------------------------------------------------------------------===//
// semantics: random terms, evaluated
//===----------------------------------------------------------------------===//

struct Rng {
  unsigned long s;
  explicit Rng(unsigned long seed) : s(seed) {}
  unsigned next(unsigned n) {
    s = s * 6364136223846793005UL + 1442695040888963407UL;
    return (s >> 33) % n;
  }
};

struct Val {
  unsigned v;    // value, 8 bits
  int prov = -1; // registry order of the leaf base it is a pointer into, or -1
};

struct Sem {
  Fixture F{8};
  AddrRangeAnalysis an{F.reg};
  std::vector<Expr> bases;
  Expr x, y, s1, s2, mem;
  std::vector<Expr> conds;
  std::map<Expr, unsigned> env; // variables and selects
  std::map<Expr, bool> cenv;    // conditions
  Rng rng;

  explicit Sem(unsigned long seed) : rng(seed) {
    // adjacent objects: b (0x08, 8) then a (0x10, 8): b + 8 is a's base. And
    // an object c (0x40, 16), a size-0 object z at c's base, d near the top
    // of the 8-bit space (0xf8, 8: its end wraps to 0).
    bases.push_back(F.base("a", 0x10, 8));
    bases.push_back(F.base("b", 0x08, 8));
    bases.push_back(F.base("c", 0x40, 16));
    bases.push_back(F.base("z", 0x40, 0));
    bases.push_back(F.base("d", 0xf8, 8));
    x = F.var("x");
    y = F.var("y");
    mem = op::bind::mkConst(
        mkTerm<std::string>("m", F.efac),
        sort::arrayTy(op::ptr::ptrsort(8, F.efac), op::bv::bvsort(8, F.efac)));
    s1 = F.sel(mem, mk<PTR_FROM_INT>(F.num(1)));
    s2 = F.sel(mem, mk<PTR_FROM_INT>(F.num(2)));
    for (const char *n : {"c0", "c1", "c2"})
      conds.push_back(F.cond(n));
  }

  Expr leafNum() {
    static const int vals[] = {0,    1,    2,    3,    4,    7,
                               8,    9,    15,   16,   0x7f, 0x80,
                               0xff, 0xfe, 0xf8, 0xfc, 0x40, 0x41};
    return F.num(vals[rng.next(sizeof vals / sizeof *vals)]);
  }
  /// an integer term (PtrSort S2e: integers and pointers apart; an address
  /// in an integer is an observation)
  Expr term(unsigned depth) {
    unsigned k = rng.next(depth ? 11 : 4);
    switch (k) {
    case 0:
      return leafNum();
    case 1:
      return rng.next(2) ? x : y;
    case 2:
      return rng.next(2) ? s1 : s2;
    case 3:
      return mk<PTR_TO_INT>(bases[rng.next(bases.size())]);
    case 4:
    case 5: {
      ExprVector args{term(depth - 1), term(depth - 1)};
      if (rng.next(3) == 0)
        args.push_back(term(depth - 1));
      return mknary<BADD>(args.begin(), args.end());
    }
    case 6:
      return mk<BSUB>(term(depth - 1), term(depth - 1));
    case 7:
      return mk<PTR_TO_INT>(ptr(depth - 1));
    case 8:
      return mk<ADDR>(ptr(depth - 1));
    case 9:
      return mk<PTR_DIFF>(ptr(depth - 1), ptr(depth - 1));
    default:
      return mk<ITE>(conds[rng.next(conds.size())], term(depth - 1),
                     term(depth - 1));
    }
  }

  /// a pointer term: bases, nodes (numeral and symbolic offsets), ites, and
  /// the pointers without provenance (from an integer, from loaded bits)
  Expr ptr(unsigned depth) {
    unsigned k = rng.next(depth ? 8 : 2);
    switch (k) {
    case 0:
      return bases[rng.next(bases.size())];
    case 1:
      return F.add({bases[rng.next(bases.size())], leafNum()});
    case 2:
    case 3:
      return F.add({ptr(depth - 1), leafNum()});
    case 4:
      return F.add({ptr(depth - 1), term(depth - 1)});
    case 5:
      return rng.next(2) ? mk<PTR_FROM_INT>(term(depth - 1))
                         : mk<PTR_FROM_BITS>(rng.next(2) ? s1 : s2);
    default:
      return mk<ITE>(conds[rng.next(conds.size())], ptr(depth - 1),
                     ptr(depth - 1));
    }
  }
  Expr any(unsigned depth) { return rng.next(2) ? ptr(depth) : term(depth); }

  const ObjectBase *obj(int order) { return &F.reg.objects()[order]; }

  Val eval(const Expr &e) {
    unsigned w;
    if (op::bv::isBvNum(e, w))
      return {(unsigned)(op::bv::toMpz(e).get_ui() & 0xff), -1};
    if (const ObjectBase *o = F.reg.lookup(e))
      return {(unsigned)o->base.getZExtValue(), (int)o->order};
    if (isOpX<ITE>(e)) {
      auto it = cenv.find(e->arg(0));
      return eval(it->second ? e->arg(1) : e->arg(2));
    }
    if (isOpX<BADD>(e)) {
      unsigned sum = 0;
      int prov = -1, ptrs = 0;
      for (size_t i = 0; i < e->arity(); ++i) {
        Val a = eval(e->arg(i));
        sum += a.v;
        if (a.prov >= 0) {
          prov = a.prov;
          ++ptrs;
        }
      }
      return {sum & 0xff, ptrs == 1 ? prov : -1};
    }
    if (isOpX<BSUB>(e) || isOpX<PTR_DIFF>(e))
      return {(eval(e->arg(0)).v - eval(e->arg(1)).v) & 0xff, -1};
    // a node: its base's provenance, its address the sum (mod 2^8)
    if (op::ptr::isPtrIndex(e)) {
      Val b = eval(op::ptr::base(e));
      mpz_class off = op::ptr::constant(e) + op::ptr::byteOffset(e);
      for (size_t i = 0; i < op::ptr::numIndices(e); ++i)
        off = off +
              mpz_class(eval(op::ptr::index(e, i)).v) * op::ptr::stride(e, i);
      mpz_class v = mpz_class(b.v) + off, r;
      mpz_fdiv_r_ui(r.get_mpz_t(), v.get_mpz_t(), 256);
      return {(unsigned)r.get_ui(), b.prov};
    }
    // observations keep the provenance of their pointer (ARM reads an
    // address through the marker); constructors from integers have none
    if (op::ptr::isObservation(e))
      return eval(e->arg(0));
    if (isOpX<PTR_FROM_INT>(e) || isOpX<PTR_FROM_BITS>(e))
      return {eval(e->arg(0)).v, -1};
    auto it = env.find(e);
    assert(it != env.end());
    return {it->second, -1};
  }

  /// value in gamma(alpha): some entry (key, I) with I containing the signed
  /// offset value - base (the absolute entry: the signed value)
  bool inGamma(const AddrRangeMap &m, unsigned v) {
    if (m.isTop())
      return true;
    for (const auto &e : m.entries()) {
      unsigned off =
          e.first == AddrRangeMap::kAbs
              ? v
              : (v - (unsigned)obj(e.first)->base.getZExtValue()) & 0xff;
      if (e.second.contains(APInt(8, off)))
        return true;
    }
    return false;
  }

  bool inBounds(const Val &v) {
    if (v.prov < 0)
      return true;
    const ObjectBase *o = obj(v.prov);
    return o->size.getZExtValue() >= 1 &&
           ((v.v - o->base.getZExtValue()) & 0xff) < o->size.getZExtValue();
  }

  void assign(Rng &r) {
    static const unsigned vals[] = {0, 1,    2,    3,    7,    8,
                                    9, 0x10, 0x7f, 0x80, 0xff, 0xf8};
    for (const Expr &c : conds)
      cenv[c] = r.next(2);
    env[x] = r.next(3) ? vals[r.next(sizeof vals / sizeof *vals)] : r.next(256);
    env[y] = r.next(3) ? vals[r.next(sizeof vals / sizeof *vals)] : r.next(256);
    env[s1] = r.next(256);
    env[s2] = vals[r.next(sizeof vals / sizeof *vals)];
  }
};

void testSemantics(unsigned count) {
  Sem S(20260501);
  Rng ar(7);
  unsigned unsound = 0, bad = 0, claims = 0, claimsInb = 0;
  for (unsigned i = 0; i < count; ++i) {
    Expr p = S.any(3), q = S.any(3);
    if (!Fixture::wellTyped(p) || !Fixture::wellTyped(q)) {
      ++bad;
      llvm::errs() << "FAIL: ill-typed generated term\n";
    }
    const AddrRangeMap &mp = S.an.alpha(p), &mq = S.an.alpha(q);
    bool d0 = S.an.disjoint(p, q, false), d1 = S.an.disjoint(p, q, true);
    claims += d0;
    claimsInb += d1;
    if (d0 && !d1) {
      ++bad;
      llvm::errs() << "FAIL: disjoint without A-INB but not with\n";
    }
    for (unsigned k = 0; k < 40; ++k) {
      S.assign(ar);
      Val vp = S.eval(p), vq = S.eval(q);
      if (!S.inGamma(mp, vp.v) || !S.inGamma(mq, vq.v)) {
        ++unsound;
        if (unsound < 5) {
          std::ostringstream os;
          os << *p << "  alpha " << str(mp) << " value " << vp.v << "  /  "
             << *q << "  alpha " << str(mq) << " value " << vq.v;
          llvm::errs() << "FAIL: value outside gamma(alpha): " << os.str()
                       << "\n";
        }
      }
      bool inb = S.inBounds(vp) && S.inBounds(vq);
      if ((d0 || (d1 && inb)) && vp.v == vq.v) {
        ++unsound;
        if (unsound < 5) {
          std::ostringstream os;
          os << *p << " alpha " << str(mp) << " / " << *q << " alpha "
             << str(mq) << " both " << vp.v
             << (d0 ? " (no A-INB)" : " (A-INB)");
          llvm::errs() << "FAIL: disjoint terms with equal values: " << os.str()
                       << "\n";
        }
      }
    }
  }
  CHECK(unsound == 0, "semantics: " << unsound << " wrong claims");
  CHECK(bad == 0, "semantics: monotone in A-INB");
  // the test must exercise the claims (a mutant that never claims is useless)
  CHECK(claims >= count / 100 && claimsInb >= count / 10,
        "semantics: too few disjoint pairs: " << claims << " / " << claimsInb);
  llvm::outs() << "semantics: pairs=" << count << " disjoint=" << claims
               << " disjoint-with-A-INB=" << claimsInb << " wrong=" << unsound
               << "\n";
}

} // namespace

int main(int argc, char **argv) {
  unsigned count = 3000;
  for (int i = 1; i < argc; ++i) {
    if (!strncmp(argv[i], "--mutant=", 9))
      arm_test::mutant = atoi(argv[i] + 9);
    else
      count = atoi(argv[i]);
  }
  struct Test {
    const char *name;
    void (*fn)();
  } tests[] = {{"lattice-range", testRange},
               {"lattice-map", testMap},
               {"thesis", testThesis},
               {"thm4", testThm4},
               {"wrap8", testWrap8}};
  for (const Test &t : tests) {
    int before = failures;
    t.fn();
    llvm::outs() << "test: " << t.name << " "
                 << (failures == before ? "ok" : "FAILED") << "\n";
  }
  int before = failures;
  testSemantics(count);
  llvm::outs() << "test: semantics " << (failures == before ? "ok" : "FAILED")
               << "\n";
  llvm::outs() << "mutant: " << arm_test::mutant << "\n";
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
