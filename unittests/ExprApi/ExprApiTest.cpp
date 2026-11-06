// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ExprApiTest.cpp - tests of the supported Expr helper API -----------===//
//
// Plain-main driver (no gtest): prints `test: <name> ok` lines and
// `failures: N`, exits non-zero if a check fails. Run by
// test/Expr/expr-api.test.
//
// What is tested:
//  - Boolean helpers (include/sea/Expr/ExprOpBool.hh): the local folds of
//    land / lor / lneg / limp / lite; the range forms land(r, efac) /
//    lor(r, efac): empty ranges give true / false, one operand gives it, two
//    operands give what the binary helper gives, more give one flat node in
//    range order (no flattening, no deduplication beyond two operands).
//  - TypeChecker (include/sea/Expr/TypeChecker.hh): function application
//    checks each argument against the declared domain (FDECL) and against a
//    lambda's bound sorts alike; constants (bind::declaredSort agrees);
//    nested binders; zero widths and narrowing extensions; one error channel
//    (check(): culprit + why), cached failures replayed, state reset between
//    queries, reset(); a 200000-deep term without stack overflow. The
//    checker prints nothing (the lit test checks stderr).
//
//===----------------------------------------------------------------------===//

#include "sea/Expr/Expr.hh"
#include "sea/Expr/TypeChecker.hh"
#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

using namespace expr;
using namespace expr::op;

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

struct Env {
  ExprFactory efac;
  Expr t = mk<TRUE>(efac), f = mk<FALSE>(efac);
  Expr b(const std::string &n) {
    return bind::boolConst(mkTerm<std::string>(n, efac));
  }
};

void testBoolBinary() {
  int before = failures;
  Env E;
  Expr x = E.b("x"), y = E.b("y");
  using namespace boolop;

  CHECK(land(x, x) == x, "x && x = x");
  CHECK(land(E.t, x) == x && land(x, E.t) == x, "true is the unit of &&");
  CHECK(land(E.f, x) == E.f && land(x, E.f) == E.f, "false absorbs &&");
  CHECK(land(x, y) == mk<AND>(x, y), "x && y kept in order");

  CHECK(lor(x, x) == x, "x || x = x");
  CHECK(lor(E.f, x) == x && lor(x, E.f) == x, "false is the unit of ||");
  CHECK(lor(E.t, x) == E.t && lor(x, E.t) == E.t, "true absorbs ||");
  CHECK(lor(x, y) == mk<OR>(x, y), "x || y kept in order");

  CHECK(lneg(E.t) == E.f && lneg(E.f) == E.t, "!true / !false");
  CHECK(lneg(lneg(x)) == x, "!!x = x");
  CHECK(lneg(x) == mk<NEG>(x), "!x");

  CHECK(limp(E.t, x) == x, "true -> x = x");
  CHECK(limp(x, E.t) == E.t, "x -> true = true");
  CHECK(limp(E.f, x) == E.t, "false -> x = true");
  CHECK(limp(x, x) == E.t, "x -> x = true");
  CHECK(limp(x, E.f) == mk<IMPL>(x, E.f), "x -> false is not folded");

  CHECK(lite(E.t, x, y) == x && lite(E.f, x, y) == y, "ite on a literal");
  CHECK(lite(y, x, x) == x, "ite with equal arms");
  CHECK(lite(y, x, E.f) == mk<ITE>(y, x, E.f), "ite kept otherwise");

  // complements are not detected (normalization, not a local fold)
  CHECK(land(x, lneg(x)) == mk<AND>(x, mk<NEG>(x)), "x && !x not folded");
  if (failures == before)
    llvm::outs() << "test: bool-binary ok\n";
}

void testBoolRange() {
  int before = failures;
  Env E;
  Expr x = E.b("x"), y = E.b("y"), z = E.b("z");
  using namespace boolop;
  using V = ExprVector;

  // empty ranges: the identities
  CHECK(land(V{}, E.efac) == E.t, "land() = true");
  CHECK(lor(V{}, E.efac) == E.f, "lor() = false");
  // a range of units only
  CHECK(land(V{E.t, E.t}, E.efac) == E.t, "land(true, true) = true");
  CHECK(lor(V{E.f, E.f, E.f}, E.efac) == E.f, "lor(false...) = false");
  // one operand
  CHECK(land(V{x}, E.efac) == x && lor(V{x}, E.efac) == x, "one operand");
  CHECK(land(V{x}) == x, "nonempty land(r)");

  // two operands: the binary helper's result, for every combination
  Expr lits[] = {x, y, E.t, E.f};
  for (Expr a : lits)
    for (Expr b : lits) {
      CHECK(land(V{a, b}, E.efac) == land(a, b), "land range/binary agree");
      CHECK(lor(V{a, b}, E.efac) == lor(a, b), "lor range/binary agree");
    }
  // two operands after the units are dropped
  CHECK(land(V{x, E.t, x}, E.efac) == x, "land(x, true, x) = x");
  CHECK(lor(V{E.f, y, x}, E.efac) == mk<OR>(y, x), "lor(false, y, x)");
  CHECK(land(V{x}, E.efac) == land(x, E.t), "land(x) = x && true");

  // absorbing literal anywhere
  CHECK(land(V{x, y, E.f, z}, E.efac) == E.f, "false absorbs");
  CHECK(lor(V{x, E.t, y, z}, E.efac) == E.t, "true absorbs");

  // more operands: one flat node in range order; no flattening of nested
  // nodes, no deduplication beyond two operands
  CHECK(land(V{x, y, z}, E.efac) == mk<AND>(x, y, z), "flat and, in order");
  CHECK(lor(V{z, y, x}, E.efac) == mk<OR>(z, y, x), "flat or, in order");
  CHECK(land(V{x, y, x}, E.efac) == mk<AND>(x, y, x), "no deduplication");
  Expr xy = mk<AND>(x, y);
  CHECK(land(V{xy, z, E.t}, E.efac) == mk<AND>(xy, z), "no flattening");

  // other range types
  std::vector<Expr> sv = {E.t, x, y};
  CHECK(land(sv, E.efac) == mk<AND>(x, y), "std::vector range");
  llvm::SmallVector<Expr, 4> lv = {E.f, x};
  CHECK(lor(lv, E.efac) == x, "SmallVector range");
  if (failures == before)
    llvm::outs() << "test: bool-range ok\n";
}

Expr bvc(Env &E, const std::string &n, unsigned w) {
  return bv::bvConst(mkTerm<std::string>(n, E.efac), w);
}
Expr bvs(Env &E, unsigned w) { return bv::bvsort(w, E.efac); }
/// the declaration of a bound variable of a binder
Expr boundDecl(Env &E, const std::string &n, Expr sort) {
  return bind::constDecl(mkTerm<std::string>(n, E.efac), sort);
}

void testTcFunctions() {
  int before = failures;
  Env E;
  TypeChecker tc;
  Expr b8 = bvs(E, 8), b16 = bvs(E, 16);
  Expr x8 = bvc(E, "x8", 8), y16 = bvc(E, "y16", 16);

  // constants: inferred = declared
  CHECK(tc.typeOf(x8) == b8, "constant sort");
  CHECK(bind::declaredSort(x8) == b8, "declaredSort");
  Expr arr =
      bind::mkConst(mkTerm<std::string>("A", E.efac), sort::arrayTy(b16, b8));
  CHECK(tc.typeOf(arr) == bind::declaredSort(arr), "array constant");
  CHECK(bind::isArrayConst(arr) && !bind::isArrayConst(x8), "isArrayConst");

  // f : bv8 x bv16 -> bv8
  ExprVector sig = {b8, b16, b8};
  Expr f = bind::fdecl(mkTerm<std::string>("f", E.efac), sig);
  Expr fsort = tc.typeOf(f);
  CHECK(isOpX<FUNCTIONAL_TY>(fsort) && fsort->arity() == 3 &&
            fsort->arg(0) == b8 && fsort->arg(1) == b16 && fsort->last() == b8,
        "fdecl sort = FUNCTIONAL(domain, range)");
  CHECK(tc.typeOf(bind::fapp(f, x8, y16)) == b8, "well-sorted application");

  // a mismatched argument sort (the old rule compared nothing for BVSORT /
  // INT formals)
  auto r = tc.check(bind::fapp(f, y16, y16));
  CHECK(!r && r.culprit == bind::fapp(f, y16, y16) && !r.unsupported,
        "argument sort mismatch");
  CHECK(r.why.find("argument 1") != std::string::npos, "why names the arg");
  Expr fi = bind::fdecl(mkTerm<std::string>("g", E.efac),
                        ExprVector{sort::intTy(E.efac), b8});
  CHECK(!tc.check(bind::fapp(fi, x8)), "bv argument to an int formal");
  CHECK(!tc.check(bind::fapp(f, x8)), "too few arguments");
  CHECK(!tc.check(mk<FAPP>(x8, x8)), "applying a non-function");

  // lambda i:bv8. ite(i = 0, 1, i): the same functional sort as an fdecl of
  // signature bv8 -> bv8, and applications checked alike
  Expr i = bind::bvar(0, b8);
  Expr zero = bv::bvnum(mpz_class(0UL), 8, E.efac);
  Expr one = bv::bvnum(mpz_class(1UL), 8, E.efac);
  Expr lam =
      mk<LAMBDA>(boundDecl(E, "i", b8), mk<ITE>(mk<EQ>(i, zero), one, i));
  Expr h = bind::fdecl(mkTerm<std::string>("h", E.efac), ExprVector{b8, b8});
  CHECK(tc.typeOf(lam) == tc.typeOf(h), "lambda sort = fdecl sort");
  CHECK(tc.typeOf(bind::fapp(lam, x8)) == b8, "lambda application");
  CHECK(!tc.check(bind::fapp(lam, y16)), "lambda argument mismatch");
  CHECK(tc.typeOf(mk<ITE>(E.b("c"), lam, lam)) == tc.typeOf(lam),
        "ite of lambdas");

  // nested binders: lambda i:bv8. lambda j:bv16. concat(i, j)
  Expr j = bind::bvar(0, b16), i1 = bind::bvar(1, b8);
  Expr inner = mk<LAMBDA>(boundDecl(E, "j", b16), mk<BCONCAT>(i1, j));
  Expr outer = mk<LAMBDA>(boundDecl(E, "i", b8), inner);
  Expr os = tc.typeOf(outer);
  CHECK(isOpX<FUNCTIONAL_TY>(os) && os->arity() == 2 && os->first() == b8 &&
            os->last() == tc.typeOf(inner),
        "nested lambda sort");
  CHECK(tc.typeOf(bind::fapp(bind::fapp(outer, x8), y16)) == bvs(E, 24),
        "nested application");
  CHECK(!tc.check(bind::fapp(bind::fapp(outer, x8), x8)),
        "nested application mismatch");
  // a quantifier over a Boolean body; over a bit-vector body is ill-sorted
  Expr q = mk<FORALL>(boundDecl(E, "k", b8), mk<EQ>(bind::bvar(0, b8), x8));
  CHECK(isOpX<BOOL_TY>(tc.typeOf(q)), "forall");
  CHECK(!tc.check(mk<EXISTS>(boundDecl(E, "k", b8), x8)), "exists over bv");
  if (failures == before)
    llvm::outs() << "test: tc-functions ok\n";
}

void testTcWidthsAndErrors() {
  int before = failures;
  Env E;
  TypeChecker tc;
  Expr x8 = bvc(E, "x8", 8);

  // zero / invalid widths
  auto r0 = tc.check(bvs(E, 0));
  CHECK(!r0 && r0.why.find("width 0") != std::string::npos, "width 0 sort");
  CHECK(!tc.check(bvc(E, "z", 0)), "constant of width 0");
  CHECK(!tc.check(bv::bvnum(mpz_class(0UL), 0, E.efac)), "numeral width 0");
  CHECK(!tc.check(mk<BEXTRACT>(mkTerm<unsigned>(8, E.efac),
                               mkTerm<unsigned>(0, E.efac), x8)),
        "extract past the width");
  CHECK(tc.typeOf(bv::extract(7, 4, x8)) == bvs(E, 4), "extract");
  CHECK(!tc.check(bv::zext(x8, 4)), "narrowing zext");
  CHECK(tc.typeOf(bv::sext(x8, 8)) == bvs(E, 8), "sext to the same width");
  CHECK(tc.typeOf(bv::zext(x8, 12)) == bvs(E, 12), "zext");

  // one error channel; failures cached and replayed; the culprit is the
  // innermost failure, also when a later query meets it through the cache
  Expr bad = mk<BADD>(x8, bvc(E, "y4", 4));
  Expr top = mk<EQ>(mk<BADD>(bad, x8), x8);
  auto r1 = tc.check(top);
  CHECK(!r1 && r1.culprit == bad, "culprit = innermost failure");
  CHECK(isOpX<ERROR_TY>(tc.typeOf(top)), "typeOf = ERROR_TY");
  auto r2 = tc.check(top);
  CHECK(!r2 && r2.culprit == bad && r2.why == r1.why, "cached failure");
  Expr other = mk<AND>(E.b("p"), top);
  auto r3 = tc.check(other);
  CHECK(!r3 && r3.culprit == bad, "failure met through the cache");
  // a good query after failed ones: no stale failure
  auto r4 = tc.check(mk<BADD>(x8, x8));
  CHECK(r4 && r4.sort == bvs(E, 8) && !r4.culprit, "state reset");
  // a failure deep in a term already partly cached
  size_t n = tc.cacheSize();
  CHECK(n > 0, "cache filled");
  tc.reset();
  CHECK(tc.cacheSize() == 0, "reset empties the cache");
  auto r5 = tc.check(top);
  CHECK(!r5 && r5.culprit == bad, "after reset");
  if (failures == before)
    llvm::outs() << "test: tc-errors ok\n";
}

void testTcDeep() {
  int before = failures;
  Env E;
  constexpr unsigned depth = 200000;
  Expr x = bvc(E, "x", 32);
  Expr e = x;
  for (unsigned k = 0; k < depth; ++k)
    e = mk<BADD>(e, x);
  {
    TypeChecker tc;
    CHECK(tc.typeOf(e) == bvs(E, 32), "deep chain sort");
    // an ill-sorted leaf at the bottom of the chain
    Expr b = mk<BADD>(x, bvc(E, "w", 16));
    for (unsigned k = 0; k < depth; ++k)
      b = mk<BADD>(b, x);
    auto r = tc.check(b);
    CHECK(!r && isOpX<BADD>(r.culprit) && r.culprit->arity() == 2 &&
              r.culprit->left() == x,
          "deep failure culprit");
    tc.reset();
  }
  if (failures == before)
    llvm::outs() << "test: tc-deep ok\n";
}

} // namespace

int main(int argc, char **argv) {
  testBoolBinary();
  testBoolRange();
  testTcFunctions();
  testTcWidthsAndErrors();
  testTcDeep();
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
