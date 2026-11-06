// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- BvRulesTest.cpp - tests of the bit-vector / Boolean rules ----------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if a
// check fails. Run by test/Expr/bv-rules.test.
//
// One test per rule group of include/sea/Expr/BvRules.hh. Every
// case checks
//  - the rewritten term (structurally; terms are hash-consed and the order of
//    the arguments of an AC operator is the creation order of the fixture's
//    variables: x, y, z) and that the named counter fired,
//  - soundness with Yices 2 and with z3: `in = out` is valid with NO assumption
//    (these rules use none).
//  - that the result type-checks (Expr's TypeChecker, the check sea-vc's
//    --vc-typecheck runs on the final query: an extract with high >= width,
//    a numeral of the wrong width, ... is a failure).
// A seeded random test rewrites many small bit-vector / Boolean terms (also
// over a registered object base) and checks the same, that the result is a
// normal form (a fresh rewriter leaves it alone), and that two runs of the
// same terms in two fresh factories print the same result (determinism).
//
//===----------------------------------------------------------------------===//

// boost hash templates are only forward-declared by Expr.hh; define them here
// (a Release build does not export the instantiations from libSeaExpr).
#include "sea/Expr/Expr.hh"
#include "sea/Expr/SeaMRules.hh"
#include "sea/Expr/Smt/Yices2SolverImpl.hh"
#include "sea/Expr/Smt/Z3SolverImpl.hh"
#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <initializer_list>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace expr;

namespace {

/// \p e as the solvers see it: nodes and markers erased (the boundary's
/// expansion; S2c keeps nodes at every position, so a rewritten term may
/// hold them)
Expr erased(const Expr &e) {
  PtrIndexExpander x;
  return x.has(e) ? x.expand(e) : e;
}

int failures = 0;

/// \p e type-checks (the final-query check of sea-vc --vc-typecheck);
/// otherwise \p why says where
bool wellTyped(const Expr &e, std::string *why = nullptr) {
  TypeChecker tc;
  TypeChecker::Result r = tc.check(e);
  if (!r && why) {
    std::ostringstream os;
    if (r.culprit)
      os << *r.culprit << " (" << r.why << ")";
    *why = os.str();
  }
  return static_cast<bool>(r);
}

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

/// The rules with and without the memory rules; no assumption.
SeaMOptions simpOpts() {
  SeaMOptions o;
  o.simplify = true;
  o.memoryRules = false;
  o.assumeInBounds = false;
  o.arm = false;
  o.storeMap = false;
  return o;
}

struct Fixture {
  ExprFactory efac;
  BaseRegistry reg;
  Expr x, y, z, o1; // 8-bit variables, the object base
  Expr p, q;        // Booleans
  Expr t, f;

  Fixture() : t(mk<TRUE>(efac)), f(mk<FALSE>(efac)) {
    x = var("x", 8);
    y = var("y", 8);
    z = var("z", 8);
    o1 = var("obj1", 8);
    reg.add(o1, "obj1", "heap", llvm::APInt(8, 0x40), llvm::APInt(8, 0x10), 4);
    p = op::bind::boolConst(mkTerm<std::string>("p", efac));
    q = op::bind::boolConst(mkTerm<std::string>("q", efac));
  }

  Expr var(const char *n, unsigned w) {
    return op::bv::bvConst(mkTerm<std::string>(n, efac), w);
  }
  Expr num(unsigned long n, unsigned w = 8) {
    return op::bv::bvnum(mpz_class(n), w, efac);
  }
  Expr num_(const mpz_class &n, unsigned w) {
    return op::bv::bvnum(n, w, efac);
  }
  template <typename Op>
  Expr nary(std::initializer_list<Expr> args) {
    ExprVector v(args);
    return mknary<Op>(v.begin(), v.end());
  }
  Expr ex(unsigned h, unsigned l, const Expr &e) {
    return op::bv::extract(h, l, e);
  }
  Expr cat(std::initializer_list<Expr> args) { return nary<BCONCAT>(args); }

  Expr rewrite(const Expr &e, RewriteStats &stats,
               SeaMOptions opts = simpOpts()) {
    SeaMRewriter rw(efac, reg, opts);
    Expr r = rw.rewrite(e);
    stats = rw.stats();
    return r;
  }

  /// `a = b` is valid (no assumption) for Yices AND for z3 (the default
  /// solver): the two marshallers encode the n-ary operators the AC rules
  /// build separately (z3 once dropped every operand after the second of an
  /// n-ary bvmul / bvxor, and only Yices was checked).
  template <typename S>
  bool validWith(const Expr &lhs, const Expr &rhs) {
    S s(efac);
    s.add(mk<NEG>(mk<EQ>(erased(lhs), erased(rhs))));
    if (getenv("BV_TEST_DUMP"))
      s.to_smt_lib(llvm::errs());
    return s.check() == sea::solver::SolverResult::UNSAT;
  }
  bool equivalent(const Expr &lhs, const Expr &rhs) {
    return validWith<sea::solver::yices_solver_impl>(lhs, rhs) &&
           validWith<sea::solver::z3_solver_impl>(lhs, rhs);
  }

  /// \p in rewrites to \p expect, the counters fired, and the rewriting is
  /// sound with no assumption.
  void expect(const char *name, const Expr &in, const Expr &expect,
              std::initializer_list<const char *> counters) {
    RewriteStats st;
    Expr out = rewrite(in, st);
    CHECK(out == expect,
          name << ": got " << str(out) << ", expected " << str(expect));
    std::string why;
    CHECK(wellTyped(out, &why), name << ": result ill-typed: " << why);
    // "a|b": one of them (SeaM's own compare rules decide some of the cases
    // first: the result is the same)
    for (const char *k : counters) {
      std::string all = k;
      bool fired = false;
      for (size_t b = 0, e; b <= all.size(); b = e + 1) {
        e = all.find('|', b);
        if (e == std::string::npos)
          e = all.size();
        fired = fired || st.get(all.substr(b, e - b)) > 0;
      }
      CHECK(fired, name << ": counter " << k << " did not fire");
    }
    CHECK(equivalent(in, out),
          name << ": unsound (" << str(in) << " vs " << str(out) << ")");
  }
  /// \p in is left alone (a normal form), no counter named \p quiet fired
  void unchanged(const char *name, const Expr &in) {
    RewriteStats st;
    Expr out = rewrite(in, st);
    CHECK(out == in, name << ": got " << str(out) << ", expected no change");
  }
};

//===----------------------------------------------------------------------===//
// extract / concat
//===----------------------------------------------------------------------===//

void testExtract() {
  Fixture F;
  Expr c32 = F.var("c32", 32);
  // full width
  F.expect("full", F.ex(7, 0, F.x), F.x, {"bv.extract.full"});
  // numeral: 0xa5 bits [6, 3] = 0b0100 = 4
  F.expect("num", F.ex(6, 3, F.num(0xa5)), F.num(4, 4), {"bv.extract.num"});
  // extract of extract
  F.expect("extract", F.ex(3, 2, F.ex(7, 4, c32)), F.ex(7, 6, c32),
           {"bv.extract.extract"});
  F.expect("extract-full", F.ex(7, 0, F.ex(15, 8, c32)), F.ex(15, 8, c32),
           {"bv.extract.full"});
  // extract of concat: one part (whole, or cut)
  Expr a = F.var("a", 8), b = F.var("b", 8), c = F.var("c", 8),
       d = F.var("d", 8);
  Expr w = F.cat({a, b, c, d}); // a is bits [31, 24]
  F.expect("concat-whole", F.ex(31, 24, w), a, {"bv.extract.concat"});
  F.expect("concat-low", F.ex(7, 0, w), d, {"bv.extract.concat"});
  F.expect("concat-cut", F.ex(19, 18, w), F.ex(3, 2, b), {"bv.extract.concat"});
  // several parts: the outer ones cut
  F.expect("concat-span", F.ex(21, 12, w),
           F.cat({F.ex(5, 0, b), F.ex(7, 4, c)}), {"bv.extract.concat"});
  F.expect("concat-two-whole", F.ex(31, 16, w), F.cat({a, b}),
           {"bv.extract.concat"});
  // a part that is an extract: extract of extract applies below
  Expr w2 = F.cat({F.ex(15, 8, c32), F.ex(7, 0, F.var("d32", 32))});
  F.expect("concat-extract-part", F.ex(15, 12, w2), F.ex(15, 12, c32),
           {"bv.extract.concat", "bv.extract.extract"});
  // not a redex
  F.unchanged("partial", F.ex(6, 1, F.x));
}

void testConcat() {
  Fixture F;
  Expr c32 = F.var("c32", 32);
  Expr a = F.var("a", 8), b = F.var("b", 8);
  // flatten
  F.expect("flatten", F.cat({F.cat({a, b}), F.x}), F.cat({a, b, F.x}),
           {"bv.concat.flatten"});
  // numerals
  F.expect("num", F.cat({F.num(0x12), F.num(0x34), a}),
           F.cat({F.num(0x1234, 16), a}), {"bv.concat.num"});
  // adjacent extracts of one term merge: the byte reassembly
  F.expect("merge", F.cat({F.ex(31, 24, c32), F.ex(23, 16, c32)}),
           F.ex(31, 16, c32), {"bv.concat.merge"});
  F.expect("reassemble",
           F.cat({F.ex(31, 24, c32), F.ex(23, 16, c32), F.ex(15, 8, c32),
                  F.ex(7, 0, c32)}),
           c32, {"bv.concat.merge", "bv.extract.full"});
  F.expect("partial-merge",
           F.cat({a, F.ex(31, 24, c32), F.ex(23, 16, c32), F.ex(7, 0, c32)}),
           F.cat({a, F.ex(31, 16, c32), F.ex(7, 0, c32)}), {"bv.concat.merge"});
  // reassembly of the bytes of a concat: extract picks them, concat merges
  Expr w = F.cat({a, b, F.x, F.y});
  F.expect(
      "reassemble-concat",
      F.cat({F.ex(31, 24, w), F.ex(23, 16, w), F.ex(15, 8, w), F.ex(7, 0, w)}),
      w, {"bv.extract.concat"});
  // not adjacent, or different terms
  F.unchanged("gap", F.cat({F.ex(31, 24, c32), F.ex(15, 8, c32)}));
  F.unchanged("two-terms",
              F.cat({F.ex(15, 8, c32), F.ex(7, 0, F.var("d32", 32))}));
  F.unchanged("swapped", F.cat({F.ex(23, 16, c32), F.ex(31, 24, c32)}));
}

//===----------------------------------------------------------------------===//
// AC operators
//===----------------------------------------------------------------------===//

void testAc() {
  Fixture F;
  // bvadd: sorted (x < y < z by creation), bases first, numeral last
  F.expect("add-sort", F.nary<BADD>({F.z, F.y, F.x}),
           F.nary<BADD>({F.x, F.y, F.z}), {"add.normalize"});
  F.expect("add-base-first", F.nary<BADD>({F.z, F.num(4), F.o1}),
           F.nary<BADD>({F.o1, F.z, F.num(4)}), {"add.normalize"});
  // bvand / bvor: flatten, sort, idempotence, numerals
  F.expect("and-sort", F.nary<BAND>({F.y, F.x}), F.nary<BAND>({F.x, F.y}),
           {"bv.ac.and"});
  F.expect("and-flatten", F.nary<BAND>({F.nary<BAND>({F.z, F.x}), F.y}),
           F.nary<BAND>({F.x, F.y, F.z}), {"bv.ac.and"});
  F.expect("and-idem", F.nary<BAND>({F.x, F.y, F.x}), F.nary<BAND>({F.x, F.y}),
           {"bv.ac.and"});
  F.expect("and-self", F.nary<BAND>({F.x, F.x}), F.x, {"bv.ac.and"});
  F.expect("and-zero", F.nary<BAND>({F.x, F.num(0xf0), F.num(0x0f)}), F.num(0),
           {"bv.ac.and"});
  F.expect("and-ones", F.nary<BAND>({F.x, F.num(0xff)}), F.x, {"bv.ac.and"});
  F.expect("and-fold", F.nary<BAND>({F.num(0x3c), F.x, F.num(0x0f)}),
           F.nary<BAND>({F.x, F.num(0x0c)}), {"bv.ac.and"});
  F.expect("or-sort-idem", F.nary<BOR>({F.y, F.x, F.y}),
           F.nary<BOR>({F.x, F.y}), {"bv.ac.or"});
  F.expect("or-ones", F.nary<BOR>({F.x, F.num(0xf0), F.num(0x0f)}), F.num(0xff),
           {"bv.ac.or"});
  F.expect("or-zero", F.nary<BOR>({F.x, F.num(0)}), F.x, {"bv.ac.or"});
  // bvxor: x ^ x = 0
  F.expect("xor-self", F.nary<BXOR>({F.x, F.x}), F.num(0), {"bv.ac.xor"});
  F.expect("xor-cancel", F.nary<BXOR>({F.x, F.y, F.x}), F.y, {"bv.ac.xor"});
  F.expect("xor-fold", F.nary<BXOR>({F.num(0x0f), F.y, F.num(0xff)}),
           F.nary<BXOR>({F.y, F.num(0xf0)}), {"bv.ac.xor"});
  // the solver marshallers encode every operand of an n-ary term (z3 once kept
  // only the first two of a bvmul / bvxor): x*y*z = (x*y)*z and differs from
  // x*y; the same for xor, and, or, add
  {
    auto enc = [&](const char *name, Expr nary3, Expr fold2, Expr two) {
      CHECK(F.validWith<sea::solver::z3_solver_impl>(nary3, fold2),
            name << ": z3 drops an operand of the n-ary term");
      CHECK(F.validWith<sea::solver::yices_solver_impl>(nary3, fold2),
            name << ": yices drops an operand of the n-ary term");
      CHECK(!F.validWith<sea::solver::z3_solver_impl>(nary3, two),
            name << ": z3 equates the n-ary term with its first two operands");
      CHECK(
          !F.validWith<sea::solver::yices_solver_impl>(nary3, two),
          name << ": yices equates the n-ary term with its first two operands");
    };
    enc("enc-mul", F.nary<BMUL>({F.x, F.y, F.z}),
        mk<BMUL>(mk<BMUL>(F.x, F.y), F.z), mk<BMUL>(F.x, F.y));
    enc("enc-xor", F.nary<BXOR>({F.x, F.y, F.z}),
        mk<BXOR>(mk<BXOR>(F.x, F.y), F.z), mk<BXOR>(F.x, F.y));
    enc("enc-and", F.nary<BAND>({F.x, F.y, F.z}),
        mk<BAND>(mk<BAND>(F.x, F.y), F.z), mk<BAND>(F.x, F.y));
    enc("enc-or", F.nary<BOR>({F.x, F.y, F.z}), mk<BOR>(mk<BOR>(F.x, F.y), F.z),
        mk<BOR>(F.x, F.y));
    enc("enc-add", F.nary<BADD>({F.x, F.y, F.z}),
        mk<BADD>(mk<BADD>(F.x, F.y), F.z), mk<BADD>(F.x, F.y));
  }
  // bvmul: numerals folded, zero, one
  F.expect("mul-sort", F.nary<BMUL>({F.y, F.x}), F.nary<BMUL>({F.x, F.y}),
           {"bv.ac.mul"});
  F.expect("mul-fold", F.nary<BMUL>({F.num(3), F.x, F.num(5)}),
           F.nary<BMUL>({F.x, F.num(15)}), {"bv.ac.mul"});
  F.expect("mul-mod", F.nary<BMUL>({F.num(16), F.x, F.num(16)}), F.num(0),
           {"bv.ac.mul"});
  F.expect("mul-one", F.nary<BMUL>({F.x, F.num(1)}), F.x, {"bv.ac.mul"});
  F.expect("mul-nodup", F.nary<BMUL>({F.x, F.x}), F.nary<BMUL>({F.x, F.x}), {});
  // equal terms become one node: (x & y) = (y & x) is true
  F.expect("and-commutes",
           mk<EQ>(F.nary<BAND>({F.x, F.y}), F.nary<BAND>({F.y, F.x})), F.t,
           {"bv.eq.refl|cmp.refl|cmp.cancel"});
  F.expect("bool-and-sort", mk<AND>(F.q, F.p), mk<AND>(F.p, F.q), {"bool.and"});
  F.expect("bool-or-flatten", mk<OR>(mk<OR>(F.q, F.p), F.p), mk<OR>(F.p, F.q),
           {"bool.or"});
  F.expect("bool-and-compl", mk<AND>(F.p, mk<NEG>(F.p), F.q), F.f,
           {"bool.and"});
  F.expect("bool-or-compl", mk<OR>(F.p, F.q, mk<NEG>(F.p)), F.t, {"bool.or"});
  F.expect("bool-and-unit", mk<AND>(F.t, F.p, F.t), F.p, {"bool.and"});
  F.expect("bool-or-zero", mk<OR>(F.f, F.p, F.t), F.t, {"bool.or"});
}

//===----------------------------------------------------------------------===//
// equality
//===----------------------------------------------------------------------===//

void testEq() {
  Fixture F;
  auto add = [&](std::initializer_list<Expr> a) { return F.nary<BADD>(a); };
  F.expect("refl", mk<EQ>(add({F.x, F.y}), add({F.y, F.x})), F.t,
           {"bv.eq.refl|cmp.refl|cmp.cancel"});
  // numerals to the right, folded
  F.expect("num-right", mk<EQ>(add({F.x, F.num(8)}), F.num(20)),
           mk<EQ>(F.x, F.num(12)), {"bv.eq.linear|cmp.cancel"});
  F.expect("num-right-mod", mk<EQ>(add({F.x, F.num(0xf0)}), F.num(0x10)),
           mk<EQ>(F.x, F.num(0x20)), {"bv.eq.linear|cmp.cancel"});
  F.expect("num-left", mk<EQ>(F.num(20), add({F.num(8), F.x})),
           mk<EQ>(F.x, F.num(12)), {"bv.eq.linear|cmp.cancel"});
  // a + c1 = a + c2
  F.expect("same-term-const-false",
           mk<EQ>(add({F.x, F.num(3)}), add({F.x, F.num(5)})), F.f,
           {"bv.eq.linear|cmp.cancel"});
  F.expect("same-term-const-true",
           mk<EQ>(add({F.x, F.num(3)}), add({F.x, F.num(3)})), F.t,
           {"bv.eq.refl|cmp.refl|cmp.cancel"});
  // common addends cancel (any term, also a product, not a registered base)
  Expr m = F.nary<BMUL>({F.z, F.num(4)});
  F.expect("cancel", mk<EQ>(add({F.x, F.y}), add({F.z, F.y})), mk<EQ>(F.x, F.z),
           {"bv.eq.linear|cmp.cancel"});
  F.expect("cancel-mul",
           mk<EQ>(add({F.x, m, F.num(7)}), add({F.y, m, F.num(2)})),
           mk<EQ>(add({F.x, F.num(5)}), F.y), {"bv.eq.linear|cmp.cancel"});
  F.expect("cancel-all", mk<EQ>(add({F.x, m}), add({m, F.x})), F.t,
           {"bv.eq.refl|cmp.refl|cmp.cancel"});
  F.expect("cancel-to-const", mk<EQ>(add({F.x, F.y}), add({F.x, F.num(9)})),
           mk<EQ>(F.y, F.num(9)), {"bv.eq.linear|cmp.cancel"});
  // orientation: the smaller id first (x < y)
  F.expect("order", mk<EQ>(F.y, F.x), mk<EQ>(F.x, F.y), {"bv.eq.order"});
  F.expect("order-sums", mk<EQ>(add({F.y, F.num(1)}), F.x),
           mk<EQ>(add({F.x, F.num(0xff)}), F.y), {"bv.eq.linear|cmp.cancel"});
  F.unchanged("canonical", mk<EQ>(add({F.x, F.num(1)}), F.y));
  // numerals: x + 4 = y + 4
  F.expect("equal-numerals", mk<EQ>(add({F.x, F.num(4)}), add({F.y, F.num(4)})),
           mk<EQ>(F.x, F.y), {"bv.eq.linear|cmp.cancel"});
  // low zero bits: 24 * x is a multiple of 8
  F.expect("low-zero", mk<EQ>(F.nary<BMUL>({F.x, F.num(24)}), F.num(7)), F.f,
           {"bv.eq.low-zero"});
  F.expect("low-zero-sum",
           mk<EQ>(add({F.nary<BMUL>({F.x, F.num(8)}),
                       F.nary<BMUL>({F.y, F.num(16)}), F.num(3)}),
                  F.num(8)),
           F.f, {"bv.eq.low-zero"});
  F.unchanged("low-zero-ok", mk<EQ>(F.nary<BMUL>({F.x, F.num(24)}), F.num(16)));
  // zeroLow, one case per branch not covered above (BMUL, BADD): each
  // expectation checked against BvRules.hh zeroLow, mirrored by the Lean
  // HasLowZero lemmas (SeaM/TargetLowZeroBits.lean)
  //  - BAND: the max of its operands (an opaque operand counts 0)
  F.expect("low-zero-and", mk<EQ>(F.nary<BAND>({F.x, F.num(8)}), F.num(4)), F.f,
           {"bv.eq.low-zero"});
  F.expect("low-zero-and-opaque",
           mk<EQ>(F.nary<BAND>({mk<BLSHR>(F.x, F.y), F.num(8)}), F.num(4)), F.f,
           {"bv.eq.low-zero"});
  //  - BOR / BXOR: the min (x has none): no refusal
  F.unchanged("low-zero-or", mk<EQ>(F.nary<BOR>({F.x, F.num(8)}), F.num(4)));
  F.unchanged("low-zero-xor", mk<EQ>(F.nary<BXOR>({F.x, F.num(8)}), F.num(4)));
  //  - BSHL by a numeral k: k plus the operand's, capped at w; k >= w is w
  F.expect("low-zero-shl", mk<EQ>(mk<BSHL>(F.x, F.num(3)), F.num(4)), F.f,
           {"bv.eq.low-zero"});
  F.expect("low-zero-shl-sat", mk<EQ>(mk<BSHL>(F.x, F.num(8)), F.num(6)), F.f,
           {"bv.eq.low-zero"});
  {
    // (x << 8) = 0 holds: the saturated count refuses no numeral it equals
    RewriteStats st;
    Expr in = mk<EQ>(mk<BSHL>(F.x, F.num(8)), F.num(0));
    Expr out = F.rewrite(in, st);
    CHECK(out != F.f, "low-zero-shl-sat-zero: refused");
    CHECK(st.get("bv.eq.low-zero") == 0,
          "low-zero-shl-sat-zero: counter bv.eq.low-zero fired");
    CHECK(F.equivalent(in, out), "low-zero-shl-sat-zero: unsound");
  }
  //  - ITE: the min of the arms
  F.expect("low-zero-ite",
           mk<EQ>(mk<ITE>(F.p, F.nary<BMUL>({F.x, F.num(8)}),
                          F.nary<BMUL>({F.y, F.num(16)})),
                  F.num(4)),
           F.f, {"bv.eq.low-zero"});
  // Boolean literals
  F.expect("bool-true", mk<EQ>(F.p, F.t), F.p, {"bv.eq.bool"});
  F.expect("bool-false", mk<EQ>(F.f, F.p), mk<NEG>(F.p), {"bv.eq.bool"});
  // ite over numerals against a numeral
  Expr i = mk<ITE>(F.p, F.num(1), F.num(0));
  F.expect("ite-const", mk<EQ>(i, F.num(1)), F.p,
           {"bv.eq.ite-const|cmp.ite-push|cmp.bvnum"});
  F.expect("ite-const-neg", mk<EQ>(F.num(0), i), mk<NEG>(F.p),
           {"bv.eq.ite-const|cmp.ite-push|cmp.bvnum"});
  F.expect("ite-const-false", mk<EQ>(i, F.num(2)), F.f,
           {"bv.eq.ite-const|cmp.ite-push|cmp.bvnum"});
  // the SeaM view of pointers still works: same base, common base cancels
  F.expect("same-base", mk<EQ>(add({F.o1, F.x}), add({F.o1, F.y})),
           mk<EQ>(F.x, F.y), {"cmp.same-base"});
}

//===----------------------------------------------------------------------===//
// Boolean ite
//===----------------------------------------------------------------------===//

void testIte() {
  Fixture F;
  Expr r = F.var("r", 1);
  Expr rb = op::bind::boolConst(mkTerm<std::string>("r", F.efac));
  (void)r;
  F.expect("and", mk<ITE>(F.p, F.q, F.f), mk<AND>(F.p, F.q),
           {"bool.ite-and-or"});
  F.expect("or-neg", mk<ITE>(F.p, F.q, F.t), mk<OR>(F.q, mk<NEG>(F.p)),
           {"bool.ite-and-or"});
  F.expect("neg-and", mk<ITE>(F.p, F.f, F.q), mk<AND>(F.q, mk<NEG>(F.p)),
           {"bool.ite-and-or"});
  F.expect("or", mk<ITE>(F.p, F.t, F.q), mk<OR>(F.p, F.q), {"bool.ite-and-or"});
  F.expect("cond-arm-then", mk<ITE>(F.p, F.p, F.q), mk<OR>(F.p, F.q),
           {"bool.ite-and-or"});
  F.expect("cond-arm-else", mk<ITE>(F.p, F.q, F.p), mk<AND>(F.p, F.q),
           {"bool.ite-and-or"});
  // negated condition: flipped
  F.expect("neg-cond", mk<ITE>(mk<NEG>(F.p), F.x, F.y), mk<ITE>(F.p, F.y, F.x),
           {"bool.ite-neg-cond"});
  // the same condition in an arm
  F.expect("same-cond-else", mk<ITE>(F.p, F.x, mk<ITE>(F.p, F.y, F.z)),
           mk<ITE>(F.p, F.x, F.z), {"bool.ite-same-cond"});
  F.expect("same-cond-then", mk<ITE>(F.p, mk<ITE>(F.p, F.x, F.y), F.z),
           mk<ITE>(F.p, F.x, F.z), {"bool.ite-same-cond"});
  // the same value on two paths
  F.expect("merge-else", mk<ITE>(F.p, F.x, mk<ITE>(F.q, F.x, F.y)),
           mk<ITE>(mk<OR>(F.p, F.q), F.x, F.y), {"bv.ite-merge"});
  F.expect("merge-then", mk<ITE>(F.p, mk<ITE>(F.q, F.x, F.y), F.y),
           mk<ITE>(mk<AND>(F.p, F.q), F.x, F.y), {"bv.ite-merge"});
  // equal arms after normalization
  F.expect("equal-arms",
           mk<ITE>(F.p, F.nary<BAND>({F.x, F.y}), F.nary<BAND>({F.y, F.x})),
           F.nary<BAND>({F.x, F.y}), {"ite.same"});
  // implication
  F.expect("impl-true", mk<IMPL>(F.t, F.p), F.p, {"bool.impl"});
  F.expect("impl-self", mk<IMPL>(F.p, F.p), F.t, {"bool.impl"});
  F.expect("impl-false", mk<IMPL>(F.p, F.f), mk<NEG>(F.p), {"bool.impl"});
  F.unchanged("impl", mk<IMPL>(F.p, F.q));
  (void)rb;
}

//===----------------------------------------------------------------------===//
// random terms: sound, a normal form, deterministic
//===----------------------------------------------------------------------===//

struct Rng {
  unsigned long s;
  explicit Rng(unsigned long seed) : s(seed) {}
  unsigned next(unsigned n) {
    s = s * 6364136223846793005UL + 1442695040888963407UL;
    return (s >> 33) % n;
  }
};

struct Gen {
  Fixture &F;
  Rng rng;
  Gen(Fixture &F, unsigned long seed) : F(F), rng(seed) {}

  /// a bit-vector term of width \p w
  Expr bv(unsigned w, unsigned depth) {
    if (depth == 0)
      return leaf(w);
    unsigned k = rng.next(w == 8 ? 12 : 7);
    switch (k) {
    case 0:
      return leaf(w);
    case 1: { // extract of a wider term
      unsigned up = w < 16 ? 1 + rng.next(std::min(8u, 16 - w)) : 0;
      if (!up)
        return leaf(w);
      unsigned l = rng.next(up + 1);
      return F.ex(l + w - 1, l, bv(w + up, depth - 1));
    }
    case 2: { // concat of two parts
      if (w < 2)
        return leaf(w);
      unsigned a = 1 + rng.next(w - 1);
      return F.cat({bv(a, depth - 1), bv(w - a, depth - 1)});
    }
    case 3: { // the bytes of a term, in order (reassembly), cut anywhere
      if (w < 2)
        return leaf(w);
      Expr t = bv(w, depth - 1);
      unsigned a = 1 + rng.next(w - 1);
      return F.cat({F.ex(w - 1, w - a, t), F.ex(w - a - 1, 0, t)});
    }
    case 4:
      return mk<ITE>(boolean(depth - 1), bv(w, depth - 1), bv(w, depth - 1));
    case 5:
    case 6: {
      Expr a = bv(w, depth - 1), b = bv(w, depth - 1);
      switch (rng.next(5)) {
      case 0:
        return F.nary<BADD>(
            {a, b, rng.next(2) ? bv(w, depth - 1) : F.num(rng.next(5), w)});
      case 1:
        return F.nary<BMUL>({a, rng.next(2) ? b : F.num(1 + rng.next(8), w)});
      case 2:
        return F.nary<BAND>({a, b});
      case 3:
        return F.nary<BOR>({a, b});
      default:
        return F.nary<BXOR>({a, b});
      }
    }
    case 7:
      return F.nary<BADD>(
          {bv(w, depth - 1), F.num(rng.next(1u << std::min(w, 8u)), w)});
    case 8:
      return F.nary<BSUB>({bv(w, depth - 1), bv(w, depth - 1)});
    case 9:
      return F.nary<BAND>(
          {bv(w, depth - 1), bv(w, depth - 1), bv(w, depth - 1)});
    case 10:
      return F.nary<BXOR>(
          {bv(w, depth - 1), bv(w, depth - 1), bv(w, depth - 1)});
    default:
      return F.nary<BMUL>({bv(w, depth - 1),
                           F.num(rng.next(1u << std::min(w, 8u)), w),
                           bv(w, depth - 1)});
    }
  }

  Expr leaf(unsigned w) {
    if (w == 8) {
      switch (rng.next(7)) {
      case 0:
        return F.x;
      case 1:
        return F.y;
      case 2:
        return F.z;
      case 3:
        return F.o1;
      default:
        return F.num(rng.next(4) == 0 ? 0xff : rng.next(40), 8);
      }
    }
    if (rng.next(3) == 0)
      return F.num(rng.next(1u << std::min(w, 8u)), w);
    // a slice of a variable: an extract of an 8-bit leaf (w <= 8)
    if (w <= 8) {
      unsigned l = rng.next(8 - w + 1);
      return F.ex(l + w - 1, l, leaf(8));
    }
    return F.cat({leaf(8), leaf(w - 8)});
  }

  Expr boolean(unsigned depth) {
    if (depth == 0)
      return rng.next(3) == 0 ? F.p
                              : (rng.next(2) ? F.q : (rng.next(2) ? F.t : F.f));
    switch (rng.next(9)) {
    case 0:
      return mk<EQ>(bv(8, depth - 1), bv(8, depth - 1));
    case 1:
      return mk<EQ>(bv(4, depth - 1), bv(4, depth - 1));
    case 2:
      return mk<BULT>(bv(8, depth - 1), bv(8, depth - 1));
    case 3:
      return mk<AND>(boolean(depth - 1), boolean(depth - 1));
    case 4:
      return mk<OR>(boolean(depth - 1), boolean(depth - 1));
    case 5:
      return mk<NEG>(boolean(depth - 1));
    case 6:
      return mk<ITE>(boolean(depth - 1), boolean(depth - 1),
                     boolean(depth - 1));
    case 7:
      return mk<IMPL>(boolean(depth - 1), boolean(depth - 1));
    default:
      return mk<EQ>(boolean(depth - 1), boolean(depth - 1));
    }
  }
};

//===----------------------------------------------------------------------===//
// defensive guards (Lean reviews 18-20): an input without a width, or out of
// the type checker's range, is left alone; extract's range is checked
//===----------------------------------------------------------------------===//

/// \p fn aborts (an assert) in a child process; its stderr is discarded
bool aborts(void (*fn)(Fixture &), Fixture &F) {
  llvm::outs().flush();
  llvm::errs().flush();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0)
      dup2(dn, 2);
    fn(F);
    _exit(0);
  }
  int st = 0;
  if (pid < 0 || waitpid(pid, &st, 0) != pid)
    return false;
  return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}

void testGuards() {
  Fixture F;
  Expr b8 = op::bv::bvsort(8, F.efac), b4 = op::bv::bvsort(4, F.efac);
  Expr v0 = F.var("v0", 0); // a width-0 constant (not a VC term)
  auto unfired = [&](const char *name, const Expr &in, const char *counter) {
    RewriteStats st;
    Expr out = F.rewrite(in, st);
    CHECK(out == in, name << ": got " << str(out) << ", expected no change");
    CHECK(st.get(counter) == 0, name << ": counter " << counter << " fired");
  };

  // sub.self: a - a with no width (ill-sorted a, a width-0 term) is left
  // alone; never a width-0 numeral
  F.expect("sub-self", mk<BSUB>(F.x, F.x), F.num(0), {"sub.self"});
  unfired("sub-self-bool", mk<BSUB>(F.p, F.p), "sub.self");
  unfired("sub-self-width0", mk<BSUB>(v0, v0), "sub.self");
  // diff.self, its pointer twin: the same guard
  unfired("diff-self-bool", mk<PTR_DIFF>(F.p, F.p), "diff.self");
  unfired("diff-self-width0", mk<PTR_DIFF>(v0, v0), "diff.self");

  // bv.sext.num / bv.zext.num: only on a well-sorted input
  F.expect("sext-num", mk<BSEXT>(F.num(0xff), op::bv::bvsort(16, F.efac)),
           F.num(0xffff, 16), {"bv.sext.num"});
  F.expect("zext-num", mk<BZEXT>(F.num(0xff), op::bv::bvsort(16, F.efac)),
           F.num(0xff, 16), {"bv.zext.num"});
  Expr n0 = F.num(0, 0); // a width-0 numeral: 2^(0-1) would wrap
  unfired("sext-width0", mk<BSEXT>(n0, b8), "bv.sext.num");
  unfired("zext-width0", mk<BZEXT>(n0, b8), "bv.zext.num");
  unfired("sext-narrow", mk<BSEXT>(F.num(0xff), b4), "bv.sext.num");
  unfired("zext-narrow", mk<BZEXT>(F.num(0xff), b4), "bv.zext.num");

  // extract range: high < width, decided where the width is known cheaply
  Expr c16 = F.cat({F.x, F.y});
  CHECK(op::bv::extractInRange(7, F.x), "range: x[7] in range");
  CHECK(!op::bv::extractInRange(8, F.x), "range: x[8] out of range");
  CHECK(!op::bv::extractInRange(8, F.num(3)), "range: numeral");
  CHECK(op::bv::extractInRange(15, c16), "range: concat[15] in range");
  CHECK(!op::bv::extractInRange(16, c16), "range: concat[16] out of range");
  CHECK(!op::bv::extractInRange(4, F.ex(7, 4, F.x)), "range: extract");
  CHECK(!op::bv::extractInRange(8, mk<BZEXT>(F.x, b8)), "range: zext");
  // a width that needs inference: not decided here (no per-term type check
  // in the constructor) ...
  Expr sum = F.nary<BADD>({F.x, F.y});
  CHECK(op::bv::extractInRange(8, sum), "range: bvadd is not decided");
  // ... the final query's type check catches it
  CHECK(!wellTyped(mk<EQ>(F.ex(8, 1, sum), F.num(0))),
        "range: x+y [8:1] passes the type check");
  CHECK(wellTyped(mk<EQ>(F.ex(7, 0, sum), F.num(0))),
        "range: x+y [7:0] fails the type check");
#ifndef NDEBUG
  // and the constructor's assert, where the width is known
  CHECK(aborts([](Fixture &G) { (void)op::bv::extract(8, 0, G.x); }, F),
        "range: bv::extract(8, 0, x:bv8) does not abort");
  CHECK(aborts(
            [](Fixture &G) { (void)op::bv::extract(16, 9, G.cat({G.x, G.y})); },
            F),
        "range: bv::extract(16, 9, x ++ y) does not abort");
  CHECK(!aborts(
            [](Fixture &G) { (void)op::bv::extract(15, 9, G.cat({G.x, G.y})); },
            F),
        "range: bv::extract(15, 9, x ++ y) aborts");
#endif
}

//===----------------------------------------------------------------------===//
// Lean views
//===----------------------------------------------------------------------===//

/// The Lean formalization (seam-lean: SeaM/PointerExtensionRules.lean,
/// SeaM/TargetExtensionRules.lean, SeaM/TargetLowZeroBits.lean) has no zext,
/// sext or shl operator: it writes them as views over concat, extract, ite,
/// numerals and mul, and proves them equal to BitVec.setWidth, signExtend
/// and <<<. That links the Lean rules to production only if each view means
/// what Expr's own operator (BZEXT, BSEXT, BSHL, as marshalled to the solver)
/// means. One validity query per view, width and amount, for all inputs,
/// with z3 and Yices:
///   zeroExtendBy e x = concat(0[e], x)                      = zext(x, w+e)
///   signExtendBy e x = concat(ite(x[w-1:w-1] = 1, 1..1[e], 0[e]), x)
///                                                           = sext(x, w+e)
///   shiftLeftBy  k x = x * (2^k mod 2^w)                    = x << k
/// and x << k = 0 for k >= w. Lean's zeroExtendTo / signExtendTo are the
/// By views cast to target = w + e.
void testViews() {
  Fixture F;
  unsigned queries = 0;
  auto valid = [&](const char *name, unsigned w, unsigned a, const Expr &lhs,
                   const Expr &rhs) {
    queries += 2;
    CHECK(F.validWith<sea::solver::z3_solver_impl>(lhs, rhs),
          name << " w=" << w << " a=" << a << ": not valid with z3");
    CHECK(F.validWith<sea::solver::yices_solver_impl>(lhs, rhs),
          name << " w=" << w << " a=" << a << ": not valid with yices");
  };
  auto ones = [&](unsigned e) {
    mpz_class v;
    mpz_ui_pow_ui(v.get_mpz_t(), 2, e);
    return F.num_(v - 1, e);
  };
  for (unsigned w : {1u, 3u, 8u, 32u, 64u}) {
    Expr x = F.var(("vx" + std::to_string(w)).c_str(), w);
    // extensions; extra = 0 is below: Lean's concat with a width-0 literal
    // is x, and SMT has no width-0 literal
    for (unsigned e : {1u, 2u, 5u, 8u, 32u, 64u}) {
      Expr bs = op::bv::bvsort(w + e, F.efac);
      Expr zview = F.cat({F.num(0, e), x});
      Expr sbit = mk<EQ>(F.ex(w - 1, w - 1, x), F.num(1, 1));
      Expr sview = F.cat({mk<ITE>(sbit, ones(e), F.num(0, e)), x});
      CHECK(wellTyped(mk<EQ>(zview, mk<BZEXT>(x, bs))), "zext view ill-typed");
      CHECK(wellTyped(mk<EQ>(sview, mk<BSEXT>(x, bs))), "sext view ill-typed");
      valid("zeroExtendBy", w, e, zview, mk<BZEXT>(x, bs));
      valid("signExtendBy", w, e, sview, mk<BSEXT>(x, bs));
    }
    // shifts by a numeral: the amount is a numeral of width w, so k < 2^w
    // (k = w + 3 does not fit at w = 1); every k >= w is covered by the
    // symbolic-amount check below
    mpz_class lim;
    mpz_ui_pow_ui(lim.get_mpz_t(), 2, w);
    for (unsigned k : {0u, 1u, w - 1, w, w + 3}) {
      if (mpz_class((unsigned long)k) >= lim)
        continue;
      mpz_class p;
      mpz_ui_pow_ui(p.get_mpz_t(), 2, k);
      p = p % lim; // BitVec.ofNat w (2^k)
      Expr shl = mk<BSHL>(x, F.num(k, w));
      valid("shiftLeftBy", w, k, mk<BMUL>(x, F.num_(p, w)), shl);
      if (k >= w)
        valid("shiftLeftBy-zero", w, k, shl, F.num(0, w));
    }
    // x << s = 0 for every amount s >= w (all of them, not only numerals)
    Expr s = F.var(("vs" + std::to_string(w)).c_str(), w);
    Expr phi =
        mk<IMPL>(mk<BUGE>(s, F.num(w, w)), mk<EQ>(mk<BSHL>(x, s), F.num(0, w)));
    valid("shl-saturate", w, 0, phi, F.t);
  }
  // extra = 0: Lean's zeroExtendBy 0 x / signExtendBy 0 x is x (the width-0
  // literal is the unit of concat). Expr's zext / sext to the operand's own
  // width type-checks (Extend rejects only narrowing) but both marshallers
  // assert in < out (ExprToZ.def BZEXT/BSEXT, MarshalYices.cpp), so the
  // solver operator is checked only where that assertion is compiled out.
  {
    Expr x = F.var("vx8", 8);
    Expr b8 = op::bv::bvsort(8, F.efac);
    CHECK(wellTyped(mk<EQ>(mk<BZEXT>(x, b8), x)),
          "zext to own width ill-typed");
#ifdef NDEBUG
    valid("zeroExtendBy", 8, 0, x, mk<BZEXT>(x, b8));
    valid("signExtendBy", 8, 0, x, mk<BSEXT>(x, b8));
#else
    // with assertions on, each marshaller aborts on it
    CHECK(aborts(
              [](Fixture &G) {
                Expr v = G.var("vx8", 8);
                (void)G.validWith<sea::solver::z3_solver_impl>(
                    v, mk<BZEXT>(v, op::bv::bvsort(8, G.efac)));
              },
              F),
          "zext to own width: the z3 marshaller does not abort");
    CHECK(aborts(
              [](Fixture &G) {
                Expr v = G.var("vx8", 8);
                (void)G.validWith<sea::solver::yices_solver_impl>(
                    v, mk<BSEXT>(v, op::bv::bvsort(8, G.efac)));
              },
              F),
          "sext to own width: the yices marshaller does not abort");
#endif
  }
  // the queries are not vacuous: a wrong view is refuted by both solvers
  {
    Expr x = F.var("vx8", 8);
    Expr b16 = op::bv::bvsort(16, F.efac);
    CHECK(!F.validWith<sea::solver::z3_solver_impl>(F.cat({x, F.num(0)}),
                                                    mk<BZEXT>(x, b16)),
          "vacuity: z3 accepts concat(x, 0) as zext");
    CHECK(!F.validWith<sea::solver::yices_solver_impl>(F.cat({x, F.num(0)}),
                                                       mk<BZEXT>(x, b16)),
          "vacuity: yices accepts concat(x, 0) as zext");
    CHECK(!F.validWith<sea::solver::z3_solver_impl>(F.cat({F.num(0), x}),
                                                    mk<BSEXT>(x, b16)),
          "vacuity: z3 accepts the zext view as sext");
    CHECK(!F.validWith<sea::solver::yices_solver_impl>(mk<BMUL>(x, F.num(4)),
                                                       mk<BSHL>(x, F.num(3))),
          "vacuity: yices accepts x * 4 as x << 3");
  }
  llvm::outs() << "views: queries=" << queries << "\n";
}

void testRandom(unsigned count, bool withMemoryRules) {
  unsigned unsound = 0, notNormal = 0, nondet = 0, changed = 0, illTyped = 0;
  for (unsigned i = 0; i < count; ++i) {
    SeaMOptions opts = simpOpts();
    opts.memoryRules = withMemoryRules;
    Fixture F;
    Gen g(F, 1000 + i);
    // a Boolean or a bit-vector term
    Expr in = (i % 3 == 0) ? Expr(g.bv(8 + 8 * (i % 2), 3)) : g.boolean(3);
    RewriteStats st;
    Expr out = F.rewrite(in, st, opts);
    if (out != in)
      ++changed;
    if (getenv("BV_TEST_VERBOSE"))
      llvm::errs() << i << ": " << str(in) << "\n  ~> " << str(out) << "\n";
    std::string why;
    if (!wellTyped(out, &why)) {
      ++illTyped;
      llvm::errs() << "FAIL: random " << i << " ill-typed: " << str(out) << ": "
                   << why << "\n";
    }
    if (!F.equivalent(in, out)) {
      ++unsound;
      llvm::errs() << "FAIL: random " << i << " unsound: " << str(in) << " ~> "
                   << str(out) << "\n";
    }
    RewriteStats st2;
    Expr again = F.rewrite(out, st2, opts);
    if (again != out) {
      ++notNormal;
      llvm::errs() << "FAIL: random " << i << " not a normal form: " << str(out)
                   << " ~> " << str(again) << "\n";
    }
    // the same term built in a fresh factory prints the same result
    Fixture F2;
    Gen g2(F2, 1000 + i);
    Expr in2 = (i % 3 == 0) ? Expr(g2.bv(8 + 8 * (i % 2), 3)) : g2.boolean(3);
    RewriteStats st3;
    Expr out2 = F2.rewrite(in2, st3, opts);
    if (str(out2) != str(out)) {
      ++nondet;
      llvm::errs() << "FAIL: random " << i << " not deterministic: " << str(out)
                   << " vs " << str(out2) << "\n";
    }
  }
  CHECK(unsound == 0, "random: " << unsound << " unsound rewrites");
  CHECK(illTyped == 0, "random: " << illTyped << " ill-typed results");
  CHECK(notNormal == 0,
        "random: " << notNormal << " results are not normal forms");
  CHECK(nondet == 0, "random: " << nondet << " results differ between runs");
  CHECK(changed > count / 4,
        "random: too few terms changed (" << changed << ")");
  llvm::outs() << "random" << (withMemoryRules ? "-mem" : "")
               << ": terms=" << count << " changed=" << changed
               << " unsound=" << unsound << " not-normal=" << notNormal
               << " nondeterministic=" << nondet << " ill-typed=" << illTyped
               << "\n";
}

} // namespace

int main(int argc, char **argv) {
  unsigned randomCount = 300;
  if (argc > 1)
    randomCount = std::atoi(argv[1]);
  struct Test {
    const char *name;
    void (*fn)();
  } tests[] = {{"extract", testExtract}, {"concat", testConcat},
               {"ac", testAc},           {"eq", testEq},
               {"ite", testIte},         {"guards", testGuards},
               {"views", testViews}};
  for (const Test &t : tests) {
    int before = failures;
    t.fn();
    llvm::outs() << "test: " << t.name << " "
                 << (failures == before ? "ok" : "FAILED") << "\n";
  }
  int before = failures;
  testRandom(randomCount, false);
  llvm::outs() << "test: random " << (failures == before ? "ok" : "FAILED")
               << "\n";
  before = failures;
  testRandom(randomCount / 2, true);
  llvm::outs() << "test: random-mem " << (failures == before ? "ok" : "FAILED")
               << "\n";
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
