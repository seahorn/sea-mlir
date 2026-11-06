// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaMRulesTest.cpp - tests of the SeaM rewrite rules ----------------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if a
// check fails. Run by test/Expr/seam-rules.test.
//
// One test per rule (include/sea/Expr/SeaMRules.hh). Every case checks
//  - the rewritten term (structurally) and that the named counter fired,
//  - soundness with Yices 2 (z3 is slow on these array queries; the VC tests
//  cover z3): `assumptions => in = out`. The assumptions are the
//    layout (obj.k = base) and the bounds of the offset variables (A-INB) for
//    a rule that names A-INB, none for the others,
//  - for a rule that uses A-INB (cmp.diff-base, which only the compare of a
//    ROW step may use: a program compare of pointers into different objects is
//    never decided, and an object of size 0 never counts as different): with
//    the assumption switched off
//    (SeaMOptions::assumeInBounds = false) cmp.diff-base does not fire and the
//    result is equivalent to the input with no assumption at all.
// A seeded random test rewrites many small memory terms over three objects and
// checks the same, plus that the result is a normal form (rewriting it again
// changes nothing; a RW_n that promises too little breaks this).
//
//===----------------------------------------------------------------------===//

// boost hash templates are only forward-declared by Expr.hh; define them here
// (a Release build does not export the instantiations from libSeaExpr).
#include "sea/Expr/SeaMRules.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/Smt/Yices2SolverImpl.hh"
#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

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

constexpr unsigned kWidth = 16;
constexpr unsigned long kSize = 0x100;

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

/// The rewrite rules: these cases pin the counters of the read-over-write
/// rules on store chains, which Store-Map (on by default) turns into
/// store-maps (its own test is sea-store-map-test).
SeaMOptions P1() {
  SeaMOptions o;
  o.storeMap = false;
  return o;
}

struct Fixture {
  ExprFactory efac;
  BaseRegistry reg;
  Expr bvSort, ptrSort, memSort;
  Expr o1, o2, o3; // object bases
  Expr o4, o0;  // o4 is adjacent to o1 (o1 + kSize); o0 has size 0, o3's base
  Expr o5;      // a fifth object, used by the ARM tests
  Expr nul;     // the null object: base 0, size 0 (SeaM-null.md)
  Expr x, y, z; // symbolic offsets / values
  Expr p, q;    // pointers that are no object's base (ptr(w) constants)
  Expr a, b, c; // Booleans
  Expr m, m2;   // memories
  Expr t, f;
  ExprVector layout; // obj.k = base, x y < size (A-INB, as the VC states it)
  ExprVector layoutOnly; // obj.k = base

  Fixture()
      : bvSort(op::bv::bvsort(kWidth, efac)),
        ptrSort(op::ptr::ptrsort(kWidth, efac)),
        memSort(sort::arrayTy(ptrSort, bvSort)), t(mk<TRUE>(efac)),
        f(mk<FALSE>(efac)) {
    auto obj = [&](const char *name, unsigned long base,
                   unsigned long size = kSize) {
      // a pointer constant, as OpSem's segment (PtrSort S2e), with its
      // erased twin as an alias
      Expr e = op::ptr::ptrConst(mkTerm<std::string>(name, efac), kWidth);
      reg.add(e, name, "heap", llvm::APInt(kWidth, base),
              llvm::APInt(kWidth, size), 4);
      reg.addAlias(op::bv::bvConst(mkTerm<std::string>(name, efac), kWidth), e);
      layout.push_back(mk<EQ>(mk<ADDR>(e), num(base)));
      layoutOnly.push_back(mk<EQ>(mk<ADDR>(e), num(base)));
      return e;
    };
    o1 = obj("obj1", 0x1000);
    o2 = obj("obj2", 0x2000);
    o3 = obj("obj3", 0x3000);
    o4 = obj("obj4", 0x1000 + kSize);
    o0 = obj("obj0", 0x3000, 0);
    o5 = obj("obj5", 0x5000);
    nul = obj("obj.null", 0, 0);
    p = op::ptr::ptrConst(mkTerm<std::string>("p", efac), kWidth);
    q = op::ptr::ptrConst(mkTerm<std::string>("q", efac), kWidth);
    x = var("x");
    y = var("y");
    z = var("z");
    a = op::bind::boolConst(mkTerm<std::string>("a", efac));
    b = op::bind::boolConst(mkTerm<std::string>("b", efac));
    c = op::bind::boolConst(mkTerm<std::string>("c", efac));
    m = mem("m");
    m2 = mem("m2");
    for (const Expr &v : {x, y})
      layout.push_back(mk<BULT>(v, num(kSize)));
  }

  Expr num(unsigned long n) {
    return op::bv::bvnum(mpz_class(n), kWidth, efac);
  }
  Expr var(const char *n) {
    return op::bv::bvConst(mkTerm<std::string>(n, efac), kWidth);
  }
  Expr mem(const char *n) {
    return op::bind::mkConst(mkTerm<std::string>(n, efac), memSort);
  }
  /// A sum; over a pointer (the first argument), the pointer plus each
  /// offset as OpSem's addr.add builds it (op::ptr::addressPlus, a node).
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
  /// the strict typing of the pointer sort (PtrSort S2e)
  bool wellTyped(const Expr &e) {
    TypeChecker tc;
    return !isOpX<ERROR_TY>(tc.typeOf(e));
  }
  /// structural (the type checker recurses: a 30000-deep ite chain)
  bool isPtr(Expr e) {
    while (isOpX<ITE>(e))
      e = e->arg(1);
    return op::ptr::isPtrConst(e) || op::ptr::isPtrIndex(e) ||
           isOpX<PTR_FROM_BITS>(e) || isOpX<PTR_FROM_INT>(e);
  }
  /// x * 1, y * 1: the summand of an addr.add offset as the rules expand it
  /// without simplify (the lowering's term; --simplify makes it x)
  /// the open compare of a ROW step: an alias question (IS_OVERLAP, D13)
  Expr ov(const Expr &a, const Expr &b) { return op::ptr::overlap(a, 1, b, 1); }
  Expr x1() { return mk<BMUL>(x, num(1)); }
  Expr y1() { return mk<BMUL>(y, num(1)); }
  Expr sel(const Expr &mm, const Expr &i) { return op::array::select(mm, i); }
  Expr st(const Expr &mm, const Expr &i, const Expr &v) {
    return op::array::store(mm, i, v);
  }

  Expr rewrite(const Expr &e, SeaMOptions opts, RewriteStats &stats) {
    CHECK(wellTyped(e), "ill-typed input " << str(e));
    SeaMRewriter rw(efac, reg, opts);
    Expr r = rw.rewrite(e);
    stats = rw.stats();
    return r;
  }

  /// Yices: `assumptions => a = b` is valid
  bool equivalent(const Expr &lhs, const Expr &rhs, bool withLayout) {
    sea::solver::yices_solver_impl s(efac);
    if (withLayout)
      for (const Expr &l : layout)
        s.add(erased(l));
    s.add(erased(mk<NEG>(mk<EQ>(erased(lhs), erased(rhs)))));
    if (getenv("SEAM_TEST_DUMP"))
      s.to_smt_lib(llvm::errs());
    return s.check() == sea::solver::SolverResult::UNSAT;
  }

  /// Yices: `layout and premise => a = b` is valid (the layout facts and a
  /// premise built by the rules, e.g. Rules::inBounds of the accesses, as
  /// --seam-check states it; no offset bounds)
  bool equivalentUnder(const Expr &lhs, const Expr &rhs, const Expr &prem) {
    sea::solver::yices_solver_impl s(efac);
    for (const Expr &l : layoutOnly)
      s.add(erased(l));
    s.add(erased(prem));
    s.add(erased(mk<NEG>(mk<EQ>(erased(lhs), erased(rhs)))));
    return s.check() == sea::solver::SolverResult::UNSAT;
  }
  /// Yices: `layout and premise` is satisfiable (a premise nothing meets
  /// would make every check pass)
  bool satisfiable(const Expr &prem) {
    sea::solver::yices_solver_impl s(efac);
    for (const Expr &l : layoutOnly)
      s.add(erased(l));
    s.add(erased(prem));
    return s.check() == sea::solver::SolverResult::SAT;
  }

  /// The case: \p in rewrites to \p expect, the counters fired, the rewriting
  /// is sound (with the layout assumptions iff \p usesInb), and with A-INB off
  /// it is sound with no assumption and cmp.diff-base does not fire.
  void expect(const char *name, const Expr &in, const Expr &expect,
              std::initializer_list<const char *> counters,
              bool usesInb = false, SeaMOptions opts = P1()) {
    CHECK(wellTyped(in), name << ": ill-typed input " << str(in));
    RewriteStats st;
    Expr out = rewrite(in, opts, st);
    CHECK(out == expect,
          name << ": got " << str(out) << ", expected " << str(expect));
    CHECK(wellTyped(out), name << ": ill-typed output " << str(out));
    for (const char *k : counters)
      CHECK(st.get(k) > 0, name << ": counter " << k << " did not fire");
    CHECK(equivalent(in, out, usesInb),
          name << ": unsound (" << str(in) << " vs " << str(out) << ")");
    if (!usesInb)
      return;
    SeaMOptions noInb = P1();
    noInb.assumeInBounds = false;
    RewriteStats st2;
    Expr out2 = rewrite(in, noInb, st2);
    CHECK(st2.get("cmp.diff-base") == 0,
          name << ": cmp.diff-base fired with A-INB off");
    CHECK(equivalent(in, out2, false),
          name << ": with A-INB off, not equivalent with no assumption ("
               << str(in) << " vs " << str(out2) << ")");
  }
};

//===----------------------------------------------------------------------===//
// normalize
//===----------------------------------------------------------------------===//

void testAddNormalize() {
  Fixture F;
  // integer sums (PtrSort S2e: no base is a bvadd addend; an address in
  // one is an observation, PTR_TO_INT)
  // flatten
  F.expect("flatten", F.add({F.add({F.z, F.x}), F.y}), F.add({F.z, F.x, F.y}),
           {"add.normalize"});
  // fold numerals into one, last
  F.expect("fold", F.add({F.z, F.num(4), F.add({F.x, F.num(8)})}),
           F.add({F.z, F.x, F.num(12)}), {"add.normalize"});
  // modulo 2^w: -1 + 2 = 1, and a numeral that sums to 2^w is dropped
  F.expect("fold-mod", F.add({F.x, F.num(0xffffUL), F.num(2)}),
           F.add({F.x, F.num(1)}), {"add.normalize"});
  F.expect("fold-zero", F.add({F.x, F.num(0xfffcUL), F.num(4)}), F.x,
           {"add.normalize"});
  // numerals only
  F.expect("fold-all", F.add({F.num(3), F.num(4)}), F.num(7),
           {"add.normalize"});
  // already normal: untouched, nothing counted
  Expr norm = F.add({F.z, F.x, F.num(4)});
  RewriteStats st;
  CHECK(F.rewrite(norm, P1(), st) == norm, "normal form is a fixpoint");
  CHECK(st.get("add.normalize") == 0, "add.normalize counted on a normal sum");
}

void testAddPullIte() {
  Fixture F;
  // a pointer ite: the node over it is pulled through it (S2c: pointer
  // arithmetic is a node; ptrindex.pull-ite)
  F.expect("pull", F.add({mk<ITE>(F.a, F.o1, F.o2), F.num(8)}),
           mk<ITE>(F.a, F.add({F.o1, F.num(8)}), F.add({F.o2, F.num(8)})),
           {"ptrindex.pull-ite"});
  // the ite in the offset of a pointer (a stride-1 index): pulled by the
  // parity rule when the index is an ite summand, which needs simplify (P1
  // has none: the summand is ite * 1); the lifted pull (pullIteIndex) takes
  // any index ite
  {
    Expr po = F.add({F.o1, mk<ITE>(F.a, F.num(4), F.num(8))});
    RewriteStats sp;
    CHECK(F.rewrite(po, P1(), sp) == po,
          "pull-offset: no pull without simplify");
    SeaMOptions simp = P1();
    simp.simplify = true;
    RewriteStats ss;
    Expr outS = F.rewrite(po, simp, ss);
    CHECK(ss.get("ptrindex.pull-ite") == 1 && isOpX<ITE>(outS),
          "pull-offset: parity pull with simplify: " << str(outS));
    // (with simplify, as in the VC: the pulled nodes' numeral indices fold)
    SeaMOptions lift = P1();
    lift.pullIteIndex = true;
    lift.simplify = true;
    // any stride (the parity rule takes stride 1 only): ite(a, 1, 2) * 24
    Expr i24 =
        op::ptr::ptrIndex(op::PtrOpKind::PTR_INDEX, F.o1,
                          {{mk<ITE>(F.a, F.num(1), F.num(2)), 24}}, 0, 0);
    RewriteStats s24;
    Expr o24 = F.rewrite(i24, lift, s24);
    CHECK(s24.get("ptrindex.pull-ite-index") == 1 && isOpX<ITE>(o24) &&
              F.equivalent(i24, o24, false),
          "pull-index stride 24: " << str(o24));
  }
  // an ite that is not a pointer is left alone
  Expr plain = F.add({mk<ITE>(F.a, F.x, F.y), F.num(8)});
  RewriteStats st;
  CHECK(F.rewrite(plain, P1(), st) == plain, "non-pointer ite stays");
  CHECK(st.get("add.pull-ite") == 0, "pull of a non-pointer ite");
  // bounded: three leaves with a limit of two
  Expr deep = F.add({mk<ITE>(F.a, mk<ITE>(F.b, F.o1, F.o2), F.o3), F.num(8)});
  SeaMOptions two = P1();
  two.maxPullLeaves = 2;
  CHECK(F.rewrite(deep, two, st) == deep, "pull is bounded");
  // nested: both ites are pulled (four leaves)
  Expr nest =
      F.add({mk<ITE>(F.a, F.o1, F.o2), mk<ITE>(F.b, F.num(4), F.num(8))});
  RewriteStats st2;
  Expr out = F.rewrite(nest, P1(), st2);
  CHECK(F.equivalent(nest, out, false), "nested pull is sound");
  CHECK(st2.get("ptrindex.pull-ite") >= 1, "nested pull: the base ite");
  CHECK(!isOpX<BADD>(out), "nested pull leaves an ite");
  // off
  SeaMOptions off = P1();
  off.pullIte = false;
  Expr p = F.add({mk<ITE>(F.a, F.o1, F.o2), F.num(8)});
  CHECK(F.rewrite(p, off, st) == p, "pullIte=false");
}

void testSub() {
  Fixture F;
  F.expect("sub-self", mk<BSUB>(F.x, F.x), F.num(0), {"sub.self"});
  // a bvsub is integer arithmetic: a sum, never a node (S2e)
  F.expect("sub-const", mk<BSUB>(F.add({F.x, F.num(12)}), F.num(4)),
           F.add({F.x, F.num(8)}), {"sub.const"});
  F.expect("sub-const-numerals", mk<BSUB>(F.num(3), F.num(5)), F.num(0xfffeUL),
           {"sub.const"});
  // addresses through observations: to_int(o1 + 12) - to_int(o1 + x)
  F.expect("sub-same-base",
           mk<BSUB>(mk<PTR_TO_INT>(F.add({F.o1, F.num(12)})),
                    mk<PTR_TO_INT>(F.add({F.o1, F.x}))),
           mk<BSUB>(F.num(12), F.x1()), {"sub.same-base"});
  // pointers into different objects: no decision, the difference of the
  // addresses (diff.erase)
  Expr d = mk<PTR_DIFF>(F.o1, F.o2);
  RewriteStats st;
  CHECK(F.rewrite(d, P1(), st) == mk<BSUB>(mk<ADDR>(F.o1), mk<ADDR>(F.o2)) &&
            st.get("diff.same-base") == 0,
        "different bases: no rule");
  // diff.ite-same-base: an ite of pointers into one object minus a pointer
  // into it (either side): pushed through the ite, every leaf cancels the
  // base; exact (no layout, no premise)
  {
    SeaMRewriter rw0(F.efac, F.reg, P1());
    const seam::Rules &R = rw0.rules();
    auto baseFree = [&](const Expr &e) { return !R.mentionsBase(e); };
    Expr it = mk<ITE>(F.a, F.add({F.o1, F.x}), F.add({F.o1, F.x, F.num(8)}));
    Expr rhs = F.add({F.o1, F.y});
    for (bool left : {true, false}) {
      Expr in = left ? mk<PTR_DIFF>(it, rhs) : mk<PTR_DIFF>(rhs, it);
      RewriteStats s2;
      Expr out = F.rewrite(in, P1(), s2);
      CHECK(s2.get("diff.ite-same-base") == 1 &&
                s2.get("diff.same-base") == 2 && baseFree(out),
            "sub.ite-same-base (" << (left ? "left" : "right")
                                  << "): base-free: " << str(out));
      CHECK(F.equivalent(in, out, false),
            "sub.ite-same-base: exact without assumptions");
    }
    // a translated read address, s + (j - d), keeps its object
    Expr tr = F.add({F.o2, mk<PTR_DIFF>(it, rhs)});
    RewriteStats s3;
    Expr out = F.rewrite(tr, P1(), s3);
    // (add.pull-ite may then pull o2 into the ite: every leaf into o2)
    auto inO2 = [&](const Expr &e) {
      std::optional<seam::PtrView> v = R.view(e);
      return v && v->base == F.o2;
    };
    CHECK(inO2(out) ||
              (isOpX<ITE>(out) && inO2(out->arg(1)) && inO2(out->arg(2))),
          "translated address: every leaf a pointer into o2: " << str(out));
    CHECK(F.equivalent(tr, out, false), "translated address: exact");
    // a leaf in another object (or null): no rule
    Expr mixed = mk<ITE>(F.a, F.add({F.o1, F.x}), F.add({F.o2, F.x}));
    RewriteStats s4;
    Expr in4 = mk<PTR_DIFF>(mixed, rhs);
    F.rewrite(in4, P1(), s4);
    CHECK(s4.get("diff.ite-same-base") == 0, "leaf in another object: no rule");
    // more leaves than maxPullLeaves: no rule
    Expr wide = it;
    for (unsigned k = 0; k < 4; ++k)
      wide = mk<ITE>(k % 2 ? F.b : F.c, F.add({F.o1, F.num(16 + k)}), wide);
    RewriteStats s5;
    F.rewrite(mk<PTR_DIFF>(wide, rhs), P1(), s5);
    CHECK(s5.get("sub.ite-same-base") == 0, "over the leaf cap: no rule");
  }
}

//===----------------------------------------------------------------------===//
// compare
//===----------------------------------------------------------------------===//

void testCompare() {
  Fixture F;
  F.expect("refl", mk<PTR_EQ>(F.add({F.o1, F.x}), F.add({F.o1, F.x})), F.t,
           {"cmp.refl"});
  F.expect(
      "bvnum-eq",
      mk<EQ>(F.num(0xffffUL), op::bv::bvnum(mpz_class(-1L), kWidth, F.efac)),
      F.t, {"cmp.bvnum"});
  F.expect("bvnum-ne", mk<EQ>(F.num(3), F.num(4)), F.f, {"cmp.bvnum"});
  F.expect("bool", mk<EQ>(F.t, F.f), F.f, {"cmp.bool"});
  // same base, numeral offsets (no assumption)
  F.expect("same-base-num",
           mk<PTR_EQ>(F.add({F.o1, F.num(4)}), F.add({F.o1, F.num(8)})), F.f,
           {"cmp.same-base"});
  F.expect("same-base-obj-off", mk<PTR_EQ>(F.o1, F.add({F.o1, F.num(4)})), F.f,
           {"cmp.same-base"});
  // same base, symbolic offsets: compare the offsets
  F.expect("same-base-sym", mk<PTR_EQ>(F.add({F.o1, F.x}), F.add({F.o1, F.y})),
           mk<EQ>(F.x1(), F.y1()), {"cmp.same-base"});
  F.expect("same-base-sym-0", mk<PTR_EQ>(F.o1, F.add({F.o1, F.y})),
           mk<EQ>(F.num(0), F.y1()), {"cmp.same-base"});
  // different bases: a program compare is never decided by A-INB (a compare
  // is not an access: one past the end of o1 is o4's base, legal C)
  for (const Expr &e : {mk<PTR_EQ>(F.o1, F.o2),
                        mk<PTR_EQ>(F.add({F.o1, F.x}), F.add({F.o2, F.y})),
                        mk<PTR_EQ>(F.add({F.o1, F.num(kSize)}), F.o4),
                        mk<PTR_EQ>(F.o3, F.o0)}) {
    RewriteStats st;
    CHECK(F.rewrite(e, P1(), st) == e, "program compare unchanged: " << str(e));
    CHECK(st.get("cmp.diff-base") == 0, "diff-base on a program compare");
  }
  // cancellation (no assumption)
  F.expect("cancel", mk<EQ>(F.add({F.x, F.num(4)}), F.add({F.y, F.num(4)})),
           mk<EQ>(F.x, F.y), {"cmp.cancel"});
  F.expect("cancel-common", mk<EQ>(F.add({F.x, F.y}), F.add({F.x, F.z})),
           mk<EQ>(F.y, F.z), {"cmp.cancel"});
  F.expect("cancel-const-moves",
           mk<EQ>(F.add({F.x, F.y, F.num(8)}), F.add({F.x, F.z, F.num(4)})),
           mk<EQ>(F.add({F.y, F.num(4)}), F.z), {"cmp.cancel"});
  F.expect("cancel-numerals",
           mk<EQ>(F.add({F.x, F.num(4)}), F.add({F.x, F.num(8)})), F.f,
           {"cmp.cancel"});
  // a - b = a - c is a + (-b) ... through sub.const only for numerals; the
  // general one is cancellation of the common addend:
  F.expect("cancel-mod",
           mk<EQ>(F.add({F.x, F.num(0xfffcUL)}), F.add({F.y, F.num(0xfffcUL)})),
           mk<EQ>(F.x, F.y), {"cmp.cancel"});
  // neq
  F.expect("neq", mk<NEQ>(F.num(3), F.num(4)), F.t, {"cmp.neq", "cmp.bvnum"});
  F.expect("neq-sym", mk<NEQ>(F.x, F.y), mk<NEG>(mk<EQ>(F.x, F.y)),
           {"cmp.neq"});
  // nothing to do
  Expr e = mk<EQ>(F.x, F.y);
  RewriteStats st;
  CHECK(F.rewrite(e, P1(), st) == e, "x = y unchanged");
}

void testCompareIte() {
  Fixture F;
  Expr p = mk<ITE>(F.a, F.o2, F.add({F.o2, F.num(8)})); // ite(a, o2, o2+8)
  // a program compare against an ite of pointers into another object is not
  // decided either (no A-INB): only ROW's compare is (testRow)
  {
    Expr e = mk<PTR_EQ>(F.o1, p);
    RewriteStats st;
    CHECK(F.rewrite(e, P1(), st) == e, "program ite compare stays");
    CHECK(st.get("cmp.diff-base") == 0, "diff-base on a program ite compare");
  }
  // Fig. 3.4: (o2 + 8) = ite(a, o2, o2 + 8) is !a
  F.expect("ite-eq-fig34", mk<PTR_EQ>(F.add({F.o2, F.num(8)}), p), mk<NEG>(F.a),
           {"cmp.ite-push", "cmp.same-base"});
  // one leaf resolves: the other compare stays
  Expr q = mk<ITE>(F.a, F.x, F.num(5));
  F.expect("ite-eq-partial", mk<EQ>(q, F.num(5)),
           mk<ITE>(F.a, mk<EQ>(F.x, F.num(5)), F.t), {"cmp.ite-push"});
  // two ites with the same condition: branch by branch (ITE-EQ-2)
  F.expect("ite-eq-2",
           mk<EQ>(mk<ITE>(F.a, F.num(1), F.num(2)),
                  mk<ITE>(F.a, F.num(1), F.num(3))),
           F.a, {"cmp.ite-push", "ite.bool"});
  // bv1 round trip, as the VC writes a Boolean (fromBool then toBool)
  Expr one = op::bv::bvnum(mpz_class(1UL), 1, F.efac);
  Expr zero = op::bv::bvnum(mpz_class(0UL), 1, F.efac);
  F.expect("bool-roundtrip", mk<EQ>(mk<ITE>(F.a, one, zero), one), F.a,
           {"cmp.ite-push", "ite.bool"});
  // nothing resolves: not pushed (it would only grow)
  Expr e = mk<EQ>(mk<ITE>(F.a, F.x, F.y), F.z);
  RewriteStats st;
  CHECK(F.rewrite(e, P1(), st) == e, "unresolved ite compare stays");
  CHECK(st.get("cmp.ite-push") == 0, "ite-push without a resolved leaf");
  // bounded
  Expr big = mk<PTR_EQ>(mk<ITE>(F.a, F.o1, F.o2), mk<ITE>(F.b, F.o1, F.o3));
  SeaMOptions one2 = P1();
  one2.maxEqLeaves = 2;
  one2.maxDecideLeaves = 2;
  RewriteStats st2;
  Expr out = F.rewrite(big, one2, st2);
  CHECK(st2.get("cmp.ite-push") == 0, "ite-push is bounded");
  CHECK(F.equivalent(big, out, true), "bounded compare is sound");
}

//===----------------------------------------------------------------------===//
// ite and Boolean
//===----------------------------------------------------------------------===//

void testIteBool() {
  Fixture F;
  Expr c = F.a;
  F.expect("ite-true", mk<ITE>(F.t, F.x, F.y), F.x, {"ite.true"});
  F.expect("ite-false", mk<ITE>(F.f, F.x, F.y), F.y, {"ite.false"});
  F.expect("ite-same", mk<ITE>(c, F.x, F.x), F.x, {"ite.same"});
  F.expect("ite-id", mk<ITE>(c, F.t, F.f), c, {"ite.bool"});
  F.expect("ite-neg", mk<ITE>(c, F.f, F.t), mk<NEG>(c), {"ite.bool"});
  F.expect("ite-neg-cond", mk<ITE>(c, F.x, mk<ITE>(mk<NEG>(c), F.y, F.z)),
           mk<ITE>(c, F.x, F.y), {"ite.neg-cond"});
  F.expect("ite-neg-cond-2", mk<ITE>(mk<NEG>(c), F.x, mk<ITE>(c, F.y, F.z)),
           mk<ITE>(mk<NEG>(c), F.x, F.y), {"ite.neg-cond"});
  F.expect("neg-neg", mk<NEG>(mk<NEG>(c)), c, {"bool.neg"});
  F.expect("neg-true", mk<NEG>(F.t), F.f, {"bool.neg"});
  F.expect("neg-false", mk<NEG>(F.f), F.t, {"bool.neg"});
  // NEG-PUSH when a branch is simple
  F.expect("neg-push", mk<NEG>(mk<ITE>(c, F.b, F.t)),
           mk<ITE>(c, mk<NEG>(F.b), F.f), {"bool.neg-ite"});
  // !ite(c, false, true) is c (the ite rule then the NEG rule)
  F.expect("neg-ite-bool", mk<NEG>(mk<ITE>(c, F.f, F.t)), c, {"ite.bool"});
  // not pushed when nothing simplifies
  Expr np = mk<NEG>(mk<ITE>(c, F.b, F.a));
  RewriteStats st;
  CHECK(F.rewrite(np, P1(), st) == np, "neg-push only when it helps");
  // and / or
  ExprVector at{F.t, F.a, F.a};
  F.expect("and-true", mknary<AND>(at.begin(), at.end()), F.a, {"bool.and"});
  ExprVector af{F.a, F.f};
  F.expect("and-false", mknary<AND>(af.begin(), af.end()), F.f, {"bool.and"});
  ExprVector ot{F.a, F.t};
  F.expect("or-true", mknary<OR>(ot.begin(), ot.end()), F.t, {"bool.or"});
  ExprVector of{F.f, F.a, F.b, F.a};
  F.expect("or-false", mknary<OR>(of.begin(), of.end()), mk<OR>(F.a, F.b),
           {"bool.or"});
}

//===----------------------------------------------------------------------===//
// read over write
//===----------------------------------------------------------------------===//

void testRow() {
  Fixture F;
  Expr v = F.var("v"), w = F.var("w");
  // HIT: same pointer
  F.expect(
      "hit",
      F.sel(F.st(F.m, F.add({F.o1, F.num(4)}), v), F.add({F.o1, F.num(4)})), v,
      {"row.store-hit"});
  // HIT through the normal form: the read pointer is written unnormalized
  F.expect("hit-norm",
           F.sel(F.st(F.m, F.add({F.o1, F.num(4)}), v),
                 F.add({F.add({F.o1, F.num(3)}), F.num(1)})),
           v, {"row.store-hit"});
  // SKIP: same base, different offsets (no assumption)
  F.expect("skip-same-base", F.sel(F.st(F.m, F.o1, v), F.add({F.o1, F.num(4)})),
           F.sel(F.m, F.add({F.o1, F.num(4)})), {"row.store-skip"});
  // SKIP: different base (A-INB), symbolic offsets
  F.expect("skip-diff-base",
           F.sel(F.st(F.m, F.add({F.o1, F.x}), v), F.add({F.o2, F.y})),
           F.sel(F.m, F.add({F.o2, F.y})), {"row.store-skip", "cmp.diff-base"},
           true);
  {
    // with A-INB off: ROW with the compare kept
    SeaMOptions off = P1();
    off.assumeInBounds = false;
    Expr e = F.sel(F.st(F.m, F.add({F.o1, F.x}), v), F.add({F.o2, F.y}));
    RewriteStats st;
    Expr out = F.rewrite(e, off, st);
    CHECK(out == mk<ITE>(F.ov(F.add({F.o1, F.x}), F.add({F.o2, F.y})), v,
                         F.sel(F.m, F.add({F.o2, F.y}))),
          "ROW without A-INB: " << str(out));
  }
  // the compare of a ROW step is the only one that uses A-INB: here a store
  // address that is an ite, a read of one of its objects
  F.expect(
      "ite-index-diff",
      F.sel(F.st(F.m, mk<ITE>(F.a, F.o2, F.add({F.o2, F.num(8)})), v), F.o1),
      F.sel(F.m, F.o1), {"row.store-skip", "cmp.diff-base"}, true);
  F.expect(
      "ite-index-diff-r",
      F.sel(F.st(F.m, F.o1, v), mk<ITE>(F.a, F.o2, F.add({F.o2, F.num(8)}))),
      F.sel(F.m, mk<ITE>(F.a, F.o2, F.add({F.o2, F.num(8)}))),
      {"row.store-skip", "cmp.diff-base"}, true);
  // one leaf equal, the other in another object: the compare is `a`
  F.expect(
      "ite-index-partial", F.sel(F.st(F.m, mk<ITE>(F.a, F.o1, F.o2), v), F.o1),
      mk<ITE>(F.a, v, F.sel(F.m, F.o1)), {"row.store", "cmp.diff-base"}, true);
  // size-0 object o0 has o3's base: not different by value, but a read (of
  // positive width) in o0 violates A-INB: vacuous under it (SeaM-null.md),
  // a definite violation (sea-vc falls back), and no skip without A-INB
  {
    Expr in = F.sel(F.st(F.m, F.o3, v), F.o0);
    RewriteStats st;
    Expr out = F.rewrite(in, P1(), st);
    CHECK(out == F.sel(F.m, F.o0) && st.get("cmp.size0-vacuous") > 0,
          "size-0: vacuous skip: " << str(out));
    SeaMRewriter rw(F.efac, F.reg, P1());
    bool violated = false;
    CHECK(isOpX<FALSE>(rw.rules().inBounds(F.o0, 1, violated)) && violated,
          "size-0: the read is a definite violation");
    SeaMOptions noInb = P1();
    noInb.assumeInBounds = false;
    RewriteStats st2;
    Expr out2 = F.rewrite(in, noInb, st2);
    CHECK(out2 == mk<ITE>(F.ov(F.o3, F.o0), v, F.sel(F.m, F.o0)) &&
              st2.get("cmp.size0-vacuous") == 0,
          "size-0 without A-INB: " << str(out2));
    CHECK(F.equivalent(in, out2, false), "size-0 without A-INB: unsound");
  }
  // integer casts: o1 + (o2 - o1) is the address o2, not a pointer into o1
  // (the view needs an offset that mentions no base): no skip
  {
    Expr idx =
        F.add({F.o1, mk<BSUB>(mk<PTR_TO_INT>(F.o2), mk<PTR_TO_INT>(F.o1))});
    F.expect("int-cast", F.sel(F.st(F.m, F.o2, v), idx),
             mk<ITE>(F.ov(F.o2, idx), v, F.sel(F.m, idx)), {"row.store"});
  }
  // a program compare inside an address survives; the one-past-the-end
  // pointer of o1 is o4's base (their layout is adjacent), so the select
  // below really reads v through the program's own compare
  {
    Expr c = mk<PTR_EQ>(F.add({F.o1, F.num(kSize)}), F.o4);
    Expr in = F.sel(F.st(F.m, F.o4, v), mk<ITE>(c, F.o4, F.o2));
    RewriteStats st;
    Expr out = F.rewrite(in, P1(), st);
    CHECK(F.equivalent(in, out, true), "adjacent: unsound " << str(out));
    std::function<bool(const Expr &)> has = [&](const Expr &e) {
      if (e == c)
        return true;
      for (size_t i = 0; i < e->arity(); ++i)
        if (has(e->arg(i)))
          return true;
      return false;
    };
    CHECK(has(out), "the program compare was dropped: " << str(out));
  }
  // unknown: ROW (Fig. 4.6 first rule), the compare simplified by the offsets
  F.expect("row-sym", F.sel(F.st(F.m, F.p, v), F.q),
           mk<ITE>(F.ov(F.p, F.q), v, F.sel(F.m, F.q)), {"row.store"});
  F.expect("row-same-base-sym",
           F.sel(F.st(F.m, F.add({F.o1, F.x}), v), F.add({F.o1, F.y})),
           mk<ITE>(mk<EQ>(F.x1(), F.y1()), v, F.sel(F.m, F.add({F.o1, F.y}))),
           {"row.store", "cmp.same-base"});
  // a chain: skip, skip, hit; nothing below the hit is looked at
  {
    Expr mem =
        F.st(F.st(F.st(F.m, F.o1, F.num(1)), F.o2, F.num(2)), F.o3, F.num(3));
    F.expect("chain", F.sel(mem, F.o1), F.num(1),
             {"row.store-skip", "row.store-hit"}, true);
    RewriteStats st;
    F.rewrite(F.sel(mem, F.o3), P1(), st);
    CHECK(st.get("row.store-hit") == 1 && st.get("row.store-skip") == 0,
          "hit at the top ends the chain");
    RewriteStats st2;
    Expr deep = F.sel(
        F.st(F.st(F.st(F.m, F.o3, w), F.o2, F.num(2)), F.o1, F.num(1)), F.o3);
    F.rewrite(deep, P1(), st2);
    CHECK(st2.get("row.store-skip") == 2 && st2.get("row.store-hit") == 1,
          "two skips then a hit: " << st2.get("row.store-skip"));
  }
  // a chain that ends in the memory
  F.expect("chain-end", F.sel(F.st(F.m, F.o1, v), F.o2), F.sel(F.m, F.o2),
           {"row.store-skip"}, true);
  // ROW over ite (Fig. 2.6 / 2.1)
  F.expect("row-ite", F.sel(mk<ITE>(F.a, F.m, F.m2), F.q),
           mk<ITE>(F.a, F.sel(F.m, F.q), F.sel(F.m2, F.q)), {"row.ite"});
  // ... with stores below that resolve
  F.expect("row-ite-stores",
           F.sel(mk<ITE>(F.a, F.st(F.m, F.o1, v), F.st(F.m2, F.o2, w)), F.o1),
           mk<ITE>(F.a, v, F.sel(F.m2, F.o1)), {"row.ite", "row.store-hit"},
           true);
  // equal arms
  F.expect("row-ite-same", F.sel(mk<ITE>(F.a, F.m, F.m), F.q), F.sel(F.m, F.q),
           {"ite.same"});
}

// Thesis Fig. 3.1 (P1) and Fig. 3.5 (P2) with word cells: obj8 = o1, obj16 =
// o2.
void testThesisExamples() {
  Fixture F;
  Expr obj8 = F.o1, obj16 = F.o2, p8 = F.add({F.o2, F.num(8)});
  Expr mem0 = F.m;
  // P1: *num = 4; st->a = 21, st->b = 42; p = a ? &st->a : &st->b; *p = 4
  Expr mem1 = F.st(
      F.st(F.st(F.st(mem0, obj8, F.num(4)), obj16, F.num(21)), p8, F.num(42)),
      mk<ITE>(F.a, obj16, p8), F.num(4));
  // Fig. 3.3b: (select mem1 obj8) = 4
  F.expect("fig3.3b", F.sel(mem1, obj8), F.num(4), {"row.store-hit"}, true);
  // Fig. 3.4b: (select mem1 (+ obj16 8)) = ite(!a, 4, 42)
  F.expect("fig3.4b", F.sel(mem1, p8),
           mk<ITE>(mk<NEG>(F.a), F.num(4), F.num(42)),
           {"row.store-hit", "row.store"}, true);
  // P2 (Fig. 3.5): six writes to three cells; reads see the last ones
  Expr n = F.st(mem0, obj16, F.num(21));
  n = F.st(n, obj8, F.num(4));
  n = F.st(n, p8, F.num(42));
  n = F.st(n, obj16, F.num(42));
  n = F.st(n, obj8, F.num(8));
  n = F.st(n, p8, F.num(21));
  F.expect("fig3.5-num", F.sel(n, obj8), F.num(8), {"row.store-hit"}, true);
  F.expect("fig3.5-st-b", F.sel(n, p8), F.num(21), {"row.store-hit"}, true);
  F.expect("fig3.5-st-a", F.sel(n, obj16), F.num(42), {"row.store-hit"}, true);
}

//===----------------------------------------------------------------------===//
// ARM (AddrRangeMap.hh): the compare of a ROW step falls back to disjoint
// address range maps
//===----------------------------------------------------------------------===//

void testArm() {
  Fixture F;
  Expr v = F.var("v");
  // five leaves: over maxPullLeaves (the sum below is not pulled through the
  // ite) and not decided leaf by leaf by the pointer view of a sum over an ite
  Expr big = mk<ITE>(F.a, mk<ITE>(F.b, F.o1, F.o2),
                     mk<ITE>(F.c, F.o3, mk<ITE>(F.b, F.o1, F.o4)));
  Expr store = F.add({big, F.num(8)}); // a pointer into one of o1..o4, +8
  auto run = [&](const Expr &in, SeaMOptions opts, RewriteStats &st) {
    return F.rewrite(in, opts, st);
  };
  // cross bases (A-INB): the ARM of the store address has o1..o4, the read
  // is a pointer into o5: skip, no expansion
  {
    Expr in = F.sel(F.st(F.m, store, v), F.add({F.o5, F.y}));
    F.expect("arm-cross", in, F.sel(F.m, F.add({F.o5, F.y})),
             {"row.store-skip", "arm.skip"}, true);
    // ARM off: the compare stays unknown, ROW expands
    SeaMOptions off = P1();
    off.arm = false;
    RewriteStats st;
    Expr out = run(in, off, st);
    CHECK(st.get("arm.skip") == 0 && st.get("row.store") == 1 &&
              isOpX<ITE>(out),
          "arm off: ROW expands " << str(out));
    CHECK(F.equivalent(in, out, true), "arm off: unsound");
    // A-INB off: different bases are not separated; ARM never uses A-INB then
    SeaMOptions noInb = P1();
    noInb.assumeInBounds = false;
    RewriteStats st2;
    Expr out2 = run(in, noInb, st2);
    CHECK(st2.get("arm.skip") == 0 && st2.get("cmp.diff-base") == 0 &&
              isOpX<ITE>(out2),
          "A-INB off: no cross-base ARM " << str(out2));
    CHECK(F.equivalent(in, out2, false),
          "A-INB off: not sound with no assumption");
  }
  // too many leaf pairs for decideEq (3 x 3 = 9 > 8): ARM of the whole terms
  {
    Expr l = mk<ITE>(F.a, mk<ITE>(F.b, F.o1, F.o2), F.o3);
    Expr r = mk<ITE>(F.a, mk<ITE>(F.c, F.o5, F.add({F.o5, F.num(8)})),
                     F.add({F.o5, F.num(16)}));
    Expr in = F.sel(F.st(F.m, F.add({l, F.x}), v), F.add({r, F.y}));
    SeaMOptions many = P1();
    many.maxDecideLeaves = 4; // 3 x 3 pairs are over the bound
    RewriteStats st;
    Expr out = run(in, many, st);
    // the store is skipped: a read of m at r + y (its shape: the node pulled
    // through r's ites, ptrindex.pull-ite)
    CHECK(isOpX<SELECT>(out) && out->arg(0) == F.m,
          "whole-term ARM: " << str(out));
    CHECK(st.get("arm.skip") > 0 && st.get("row.store-skip") > 0,
          "whole-term ARM counters");
    CHECK(F.equivalent(in, out, true), "whole-term ARM unsound");
  }
  // same base, intervals that do not meet: no assumption (also with A-INB off)
  {
    // o1 + five-leaf ite of numerals: not decided by the view (offsets are not
    // numerals); [4, 20] against [100, 100]
    Expr offs =
        mk<ITE>(F.a, mk<ITE>(F.b, F.num(4), F.num(8)),
                mk<ITE>(F.c, F.num(12), mk<ITE>(F.b, F.num(16), F.num(20))));
    Expr s1 = F.add({F.o1, offs});
    Expr in = F.sel(F.st(F.m, s1, v), F.add({F.o1, F.num(100)}));
    for (bool inb : {true, false}) {
      SeaMOptions o = P1();
      o.assumeInBounds = inb;
      RewriteStats st;
      Expr out = run(in, o, st);
      CHECK(out == F.sel(F.m, F.add({F.o1, F.num(100)})),
            "same-base ARM (A-INB " << inb << "): " << str(out));
      CHECK(st.get("arm.skip") > 0, "same-base ARM counter");
      CHECK(st.get("cmp.diff-base") == 0, "same-base ARM: no diff-base");
      CHECK(F.equivalent(in, out, false),
            "same-base ARM: not sound with no assumption");
    }
    // the intervals meet: no decision
    Expr in2 = F.sel(F.st(F.m, s1, v), F.add({F.o1, F.num(12)}));
    RewriteStats st2;
    Expr out2 = run(in2, P1(), st2);
    CHECK(st2.get("arm.skip") == 0 && isOpX<ITE>(out2),
          "meeting intervals: " << str(out2));
    CHECK(F.equivalent(in2, out2, true), "meeting intervals unsound");
  }
  // never a program compare
  {
    Expr offs =
        mk<ITE>(F.a, mk<ITE>(F.b, F.num(4), F.num(8)),
                mk<ITE>(F.c, F.num(12), mk<ITE>(F.b, F.num(16), F.num(20))));
    for (const Expr &e :
         {mk<PTR_EQ>(F.add({F.o1, offs}), F.add({F.o1, F.num(100)})),
          mk<PTR_EQ>(F.add({big, F.num(8)}), F.add({F.o5, F.y}))}) {
      RewriteStats st;
      Expr out = F.rewrite(e, P1(), st);
      CHECK(st.get("arm.skip") == 0 && st.get("cmp.diff-base") == 0,
            "ARM on a program compare: " << str(out));
      CHECK(F.equivalent(e, out, false), "program compare: unsound");
    }
  }
  // not claimed: an absolute part (a number may lie in any object), top (a
  // base mentioned through an integer cast), a size-0 object
  {
    Expr num = mk<ITE>(
        F.a, F.o1, mk<PTR_FROM_INT>(F.num(0x2004))); // a pointer or a number
    Expr cast =
        F.add({F.o1, mk<BSUB>(mk<PTR_TO_INT>(F.o2), mk<PTR_TO_INT>(F.o1))});
    for (const Expr &addr :
         {F.add({num, mk<ITE>(F.b, F.num(4), F.num(8))}),
          F.add({big, mk<BSUB>(mk<PTR_TO_INT>(F.o2), mk<PTR_TO_INT>(F.o1))}),
          cast}) {
      Expr in = F.sel(F.st(F.m, F.add({addr, F.x}), v), F.add({F.o5, F.y}));
      RewriteStats st;
      Expr out = run(in, P1(), st);
      CHECK(st.get("arm.skip") == 0,
            "ARM claimed something it must not: " << str(addr));
      CHECK(F.equivalent(in, out, true), "unsound: " << str(out));
    }
    // size-0 object in the map: its provenance is vacuous under A-INB (an
    // access of positive width), the rest is apart from o3: skip, with
    // cmp.size0-vacuous; sound under the premise (layout and both access
    // conditions), which is satisfiable; without A-INB no skip
    Expr z = F.add({mk<ITE>(F.a, mk<ITE>(F.b, F.o1, F.o2),
                            mk<ITE>(F.c, F.o0, mk<ITE>(F.b, F.o1, F.o2))),
                    F.num(8)});
    Expr in = F.sel(F.st(F.m, z, v), F.add({F.o3, F.y}));
    RewriteStats st;
    Expr out = run(in, P1(), st);
    CHECK(st.get("arm.skip") > 0 && st.get("cmp.size0-vacuous") > 0,
          "size-0 object: vacuous provenance, ARM skip: " << str(out));
    {
      SeaMRewriter rw(F.efac, F.reg, P1());
      bool violated = false;
      Expr prem = mk<AND>(rw.rules().inBounds(rw.rewrite(z), 1, violated),
                          rw.rules().inBounds(F.add({F.o3, F.y}), 1, violated));
      CHECK(!violated, "size-0 provenance: not a definite violation");
      CHECK(F.satisfiable(prem), "size-0 provenance: premise satisfiable");
      CHECK(F.equivalentUnder(in, out, prem),
            "size-0: unsound under the premise");
    }
    SeaMOptions noInb = P1();
    noInb.assumeInBounds = false;
    RewriteStats st2;
    Expr out2 = run(in, noInb, st2);
    CHECK(st2.get("arm.skip") == 0, "size-0 object: no ARM skip without A-INB");
    CHECK(F.equivalent(in, out2, false), "size-0 without A-INB: unsound");
  }
  // A-INB condition of an access address with no view: per provenance
  {
    bool violated = false;
    RewriteStats st;
    SeaMRewriter rw(F.efac, F.reg);
    Expr addr = rw.rewrite(F.add({big, F.x}));
    Expr cond = rw.rules().inBounds(addr, 4, violated);
    CHECK(!isOpX<TRUE>(cond) && !violated,
          "access condition of a sum over an ite: " << str(cond));
    // the condition is false where the provenance base is out of reach: o1's
    // leaf at offset x with x >= size - 4
    sea::solver::yices_solver_impl s(F.efac);
    for (const Expr &l : F.layout)
      s.add(erased(l));
    s.add(erased(F.a));
    s.add(erased(F.b));
    s.add(erased(mk<EQ>(F.x, F.num(kSize - 2))));
    s.add(erased(cond));
    CHECK(s.check() == sea::solver::SolverResult::UNSAT,
          "the condition holds out of bounds");
    sea::solver::yices_solver_impl s2(F.efac);
    for (const Expr &l : F.layout)
      s2.add(erased(l));
    s2.add(F.a);
    s2.add(F.b);
    s2.add(mk<EQ>(F.x, F.num(8)));
    s2.add(erased(cond));
    CHECK(s2.check() == sea::solver::SolverResult::SAT,
          "the condition fails in bounds");
    // every entry inside its object: nothing to assume
    Expr inside = rw.rewrite(F.add({big, F.num(8)}));
    bool v3 = false;
    CHECK(isOpX<TRUE>(rw.rules().inBounds(inside, 4, v3)) && !v3,
          "in-bounds interval: no condition");
    // wholly outside its object (kSize = 0x100): a definite violation
    Expr outside = rw.rewrite(F.add({big, F.num(0x300)}));
    bool v4 = false;
    Expr c4 = rw.rules().inBounds(outside, 4, v4);
    CHECK(v4 && !isOpX<TRUE>(c4),
          "out-of-bounds interval: violation, condition built");
  }
  // a deep ite chain under a sum: the condition is built iteratively
  {
    constexpr unsigned kDepth = 30000;
    Expr chain = F.o5;
    for (unsigned k = 0; k < kDepth; ++k) {
      Expr cnd = op::bind::boolConst(
          mkTerm<std::string>("deep" + std::to_string(k), F.efac));
      chain = mk<ITE>(cnd, k % 2 ? F.o1 : F.o2, chain);
    }
    SeaMRewriter rw(F.efac, F.reg, P1());
    bool v = false;
    Expr cond = rw.rules().inBounds(F.add({chain, F.x}), 4, v);
    CHECK(!isOpX<TRUE>(cond), "deep chain: condition");
    Expr plain = rw.rules().inBounds(chain, 4, v);
    CHECK(isOpX<TRUE>(plain) && !v, "deep chain of bases: nothing to assume");
  }
}

//===----------------------------------------------------------------------===//
// the null object and objects of size 0 (docs/design/SeaM-null.md)
//===----------------------------------------------------------------------===//

void testNull() {
  Fixture F;
  SeaMRewriter rw(F.efac, F.reg, P1());
  auto &R = rw.rules();
  Expr nz = F.add({F.nul, F.z}); // null + z, z unbounded
  Expr ox = F.add({F.o1, F.x});
  // guarded aggregation: an impossible leaf is false in the condition, the
  // access is a definite violation only if every alternative is impossible
  {
    bool vio = false;
    Expr mixed = mk<ITE>(F.a, nz, ox);
    Expr cond = R.inBounds(mixed, 4, vio);
    CHECK(!vio, "mixed null / object: no definite violation");
    CHECK(cond == mk<ITE>(F.a, F.f, mk<BULE>(F.x1(), F.num(kSize - 4))),
          "mixed: guarded condition: " << str(cond));
    CHECK(F.satisfiable(cond), "mixed: condition satisfiable");
    CHECK(!F.satisfiable(mk<AND>(cond, F.a)), "mixed: the null leaf is false");
    bool vio2 = false;
    Expr all = mk<ITE>(F.a, nz, F.add({F.o1, F.num(0x300)}));
    CHECK(isOpX<FALSE>(R.inBounds(all, 4, vio2)) && vio2,
          "every alternative impossible: a definite violation");
    // not only null: an ordinary out-of-bounds leaf beside a live one
    bool vio3 = false;
    Expr oob = mk<ITE>(F.a, F.add({F.o1, F.num(0x300)}), F.add({F.o2, F.x}));
    Expr c3 = R.inBounds(oob, 4, vio3);
    CHECK(!vio3 && F.satisfiable(c3), "mixed out-of-bounds leaf: guarded");
    // ARM leaves (no view): a sum over an ite with a null provenance
    Expr deep = F.add({mk<ITE>(F.a, mk<ITE>(F.b, F.o1, F.nul),
                               mk<ITE>(F.c, F.o2, mk<ITE>(F.b, F.nul, F.o5))),
                       F.num(8)});
    Expr dn = rw.rewrite(deep);
    bool vio4 = false;
    Expr c4 = R.inBounds(dn, 4, vio4);
    CHECK(!vio4 && F.satisfiable(c4),
          "ARM: mixed provenance guarded: " << str(c4));
    CHECK(!F.satisfiable(mk<AND>(c4, mk<AND>(F.a, mk<NEG>(F.b)))),
          "ARM: the null provenance is false");
    Expr dz = rw.rewrite(
        F.add({mk<ITE>(F.a, F.nul, mk<ITE>(F.b, F.nul, F.o0)), F.x}));
    bool vio5 = false;
    CHECK(isOpX<FALSE>(R.inBounds(dz, 1, vio5)) && vio5,
          "ARM: only size-0 provenances: a definite violation");
    // ranges: length 0 through null is admitted, positive length is not
    Expr n = F.var("n");
    bool rv = false;
    CHECK(R.rangeInBounds(nz, n, rv) == mk<EQ>(n, F.num(0)) && !rv,
          "range through null: n = 0");
    CHECK(isOpX<TRUE>(R.rangeInBounds(nz, F.num(0), rv)) && !rv,
          "range through null: numeral 0");
    CHECK(isOpX<FALSE>(R.rangeInBounds(nz, F.num(1), rv)) && rv,
          "range through null: numeral 1, a definite violation");
    bool rv2 = false;
    Expr rc = R.rangeInBounds(mixed, F.num(4), rv2);
    CHECK(!rv2 && F.satisfiable(rc), "range through a mixed pointer: guarded");
  }
  // an access compare: read through ite(a, null + z, o1 + x) over a store to
  // o2: every leaf pair decided (cmp.size0-vacuous, cmp.diff-base), skip,
  // valid under the full A-INB premise (satisfiable), not without it
  {
    Expr idx = mk<ITE>(F.a, nz, ox), sa = F.add({F.o2, F.y});
    Expr v = F.var("v");
    Expr in = F.sel(F.st(F.m, sa, v), idx);
    RewriteStats st;
    // the compare itself (addr.prune-size0, which drops the null leaf first,
    // is tested in testPruneSize0)
    SeaMOptions noPrune = P1();
    noPrune.pruneSize0 = false;
    Expr out = F.rewrite(in, noPrune, st);
    CHECK(out == F.sel(F.m, idx), "mixed null read: skip: " << str(out));
    CHECK(st.get("cmp.size0-vacuous") > 0 && st.get("cmp.diff-base") > 0 &&
              st.get("premise.a-inb") > 0,
          "mixed null read: reasons");
    seam::Decision d;
    CHECK(R.decideEq(sa, idx, d, true) == seam::Tri::No &&
              d.uses(seam::PremAInb),
          "decision: no, A-INB");
    bool recorded = false;
    for (const auto &r : d.reasons)
      recorded = recorded || (std::string(r.rule) == "cmp.size0-vacuous" &&
                              (r.premises & seam::PremAInb));
    CHECK(recorded, "cmp.size0-vacuous recorded with A-INB in the Decision");
    bool vio = false;
    Expr prem = mk<AND>(R.inBounds(idx, 1, vio), R.inBounds(sa, 1, vio));
    CHECK(!vio && F.satisfiable(prem), "premise satisfiable");
    CHECK(F.equivalentUnder(in, out, prem), "valid under the A-INB premise");
    CHECK(!F.equivalentUnder(in, out, F.t), "not valid without the premise");
    // the leaf pair alone: null + z against o2 + y
    seam::Decision d1;
    CHECK(R.decidePair(sa, nz, d1, true) == seam::Tri::No &&
              d1.reasons.size() == 1 &&
              std::string(d1.reasons[0].rule) == "cmp.size0-vacuous",
          "leaf pair: cmp.size0-vacuous");
    // without A-INB: not decided, sound with no assumption
    SeaMOptions noInb = P1();
    noInb.assumeInBounds = false;
    RewriteStats st2;
    Expr out2 = F.rewrite(in, noInb, st2);
    CHECK(st2.get("cmp.size0-vacuous") == 0 &&
              st2.get("addr.prune-size0") == 0 && out2 != F.sel(F.m, idx),
          "without A-INB: no skip: " << str(out2));
    CHECK(F.equivalent(in, out2, false), "without A-INB: unsound");
    // same base, numeral offsets: premise-free (cmp.same-base), not vacuous
    seam::Decision d2;
    CHECK(R.decidePair(F.add({F.nul, F.num(4)}), F.add({F.nul, F.num(8)}), d2,
                       true) == seam::Tri::No &&
              !d2.uses(seam::PremAInb),
          "null + 4 vs null + 8: same base, no premise");
  }
  // program compares: never by layout or by size 0
  {
    RewriteStats st;
    CHECK(F.rewrite(mk<PTR_EQ>(F.nul, F.nul), P1(), st) == F.t, "null == null");
    for (const Expr &e : {mk<PTR_EQ>(F.o1, F.nul), mk<PTR_EQ>(nz, ox),
                          mk<PTR_EQ>(F.add({F.nul, F.num(0x1004)}), ox),
                          mk<PTR_EQ>(F.o0, F.o3)}) {
      RewriteStats st1;
      Expr out = F.rewrite(e, P1(), st1);
      CHECK(out == e && st1.get("cmp.size0-vacuous") == 0 &&
                st1.get("cmp.diff-base") == 0,
            "program compare not decided: " << str(e) << " -> " << str(out));
      // o1 == null: false by the layout facts, which the solver has (the
      // rewriter's constants are independent without them); the others can
      // hold (null + k may equal obj + j numerically; o0 shares o3's base)
      CHECK(F.satisfiable(e) == (e != mk<PTR_EQ>(F.o1, F.nul)),
            "program compare left to the solver: " << str(e));
    }
  }
  // null + k against obj + j (symbolic): no base decision in a program
  // compare, and none for ROW without A-INB
  {
    Expr in = F.sel(F.st(F.m, ox, F.var("v")), nz);
    SeaMOptions noInb = P1();
    noInb.assumeInBounds = false;
    RewriteStats st;
    Expr out = F.rewrite(in, noInb, st);
    CHECK(isOpX<ITE>(out), "null + z vs o1 + x without A-INB: undecided");
    seam::Decision d;
    CHECK(R.decidePair(ox, nz, d, false) == seam::Tri::Unknown,
          "null + z vs o1 + x: program compare unknown");
  }
}

//===----------------------------------------------------------------------===//
// shadow memories: object identity, not A-INB (SeaM-null.md)
//===----------------------------------------------------------------------===//

/// addr.prune-size0 at loads and stores (docs/design/SeaM-provenance.md):
/// the ite leaves of an access address in a size-0 object are
/// dropped, valid under that access's own A-INB (inBounds of the original
/// address), not without it; only the access argument changes.
void testPruneSize0() {
  Fixture F;
  SeaMRewriter rw0(F.efac, F.reg, P1());
  const seam::Rules &R = rw0.rules();
  Expr nz = F.add({F.nul, F.z}), ox = F.add({F.o1, F.x});
  Expr idx = mk<ITE>(F.a, nz, ox);
  // a load: the null leaf dropped
  {
    Expr in = F.sel(F.m, idx);
    RewriteStats st;
    Expr out = F.rewrite(in, P1(), st);
    CHECK(out == F.sel(F.m, ox) && st.get("addr.prune-size0") == 1 &&
              st.get("premise.a-inb") == 1,
          "load: null leaf pruned: " << str(out));
    bool vio = false;
    Expr prem = R.inBounds(idx, 1, vio);
    CHECK(!vio && F.satisfiable(prem), "load: premise satisfiable");
    CHECK(F.equivalentUnder(in, out, prem), "load: valid under A-INB");
    CHECK(!F.equivalentUnder(in, out, F.t), "load: not valid without it");
  }
  // a store: the address of the store pruned, the value untouched (it is the
  // same pointer, stored: never pruned)
  {
    // a stored pointer is its bits (PTR_TO_BITS, as OpSem stores it)
    Expr in = F.st(F.m, idx, mk<PTR_TO_BITS>(idx));
    RewriteStats st;
    Expr out = F.rewrite(in, P1(), st);
    CHECK(out == F.st(F.m, ox, mk<PTR_TO_BITS>(idx)) &&
              st.get("addr.prune-size0") == 1,
          "store: address pruned, value kept: " << str(out));
    bool vio = false;
    Expr prem = R.inBounds(idx, 1, vio);
    Expr k = F.p;
    CHECK(F.equivalentUnder(F.sel(in, k), F.sel(out, k), prem),
          "store: valid under A-INB");
    CHECK(!F.equivalentUnder(F.sel(in, k), F.sel(out, k), F.t),
          "store: not valid without it");
  }
  // nested: only the size-0 leaves go, the other guards stay
  {
    Expr oy = F.add({F.o2, F.y});
    Expr in = F.sel(F.m, mk<ITE>(F.a, ox, mk<ITE>(F.b, nz, oy)));
    RewriteStats st;
    Expr out = F.rewrite(in, P1(), st);
    CHECK(out == F.sel(F.m, mk<ITE>(F.a, ox, oy)),
          "nested: inner null leaf pruned: " << str(out));
  }
  // a numeral leaf is kept (it may be a valid address); a zero-sized
  // allocation (o0) is pruned as null is
  {
    Expr numIdx = mk<ITE>(F.a, mk<PTR_FROM_INT>(F.num(0x40)), ox);
    RewriteStats st;
    Expr out = F.rewrite(F.sel(F.m, numIdx), P1(), st);
    CHECK(out == F.sel(F.m, numIdx) && st.get("addr.prune-size0") == 0,
          "numeral leaf kept: " << str(out));
    RewriteStats st2;
    Expr out2 =
        F.rewrite(F.sel(F.m, mk<ITE>(F.a, F.add({F.o0, F.z}), ox)), P1(), st2);
    CHECK(out2 == F.sel(F.m, ox), "size-0 allocation pruned: " << str(out2));
  }
  // every leaf size 0: a definite violation, not rewritten (the static
  // fallback runs without A-INB)
  {
    Expr allNull = mk<ITE>(F.a, nz, F.add({F.nul, F.num(4)}));
    RewriteStats st;
    Expr out = F.rewrite(F.sel(F.m, allNull), P1(), st);
    CHECK(st.get("addr.prune-size0") == 0,
          "all null: not pruned: " << str(out));
    bool vio = false;
    (void)R.inBounds(allNull, 1, vio);
    CHECK(vio, "all null: a definite violation");
  }
  // a program compare of the same pointer is not an access: not pruned
  {
    Expr cmp = mk<PTR_EQ>(idx, F.p);
    RewriteStats st;
    Expr out = F.rewrite(cmp, P1(), st);
    CHECK(st.get("addr.prune-size0") == 0, "compare: not pruned: " << str(out));
  }
  // off without A-INB, and with the option off
  {
    SeaMOptions noInb = P1();
    noInb.assumeInBounds = false;
    RewriteStats st;
    Expr out = F.rewrite(F.sel(F.m, idx), noInb, st);
    CHECK(out == F.sel(F.m, idx) && st.get("addr.prune-size0") == 0,
          "no A-INB: not pruned");
    SeaMOptions off = P1();
    off.pruneSize0 = false;
    RewriteStats st2;
    Expr out2 = F.rewrite(F.sel(F.m, idx), off, st2);
    CHECK(out2 == F.sel(F.m, idx), "option off: not pruned");
  }
  // a shadow memory: identity contract, no A-INB: not pruned
  {
    Expr S = F.mem("shadow-prune");
    F.reg.addShadowRoot(S);
    RewriteStats st;
    Expr out = F.rewrite(F.sel(S, idx), P1(), st);
    CHECK(out == F.sel(S, idx) && st.get("addr.prune-size0") == 0,
          "shadow: not pruned: " << str(out));
  }
}

void testShadow() {
  Fixture F;
  // a shadow memory: its start is registered (in the VC the const-array of
  // `sea.mem.const ... shadow`; a named array here: Yices has no lambdas)
  Expr S = F.mem("shadow");
  F.reg.addShadowRoot(S);
  Expr v = F.var("v"), w = F.var("w");
  auto noAInb = [&](const RewriteStats &st, const char *name) {
    CHECK(st.get("premise.a-inb") == 0 && st.get("cmp.diff-base") == 0 &&
              st.get("cmp.size0-vacuous") == 0 && st.get("arm.skip") == 0,
          name << ": an A-INB rule fired in a shadow memory");
  };
  // distinct bases: skip by identity, valid given the layout only
  for (auto [i, j] : {std::pair<Expr, Expr>{F.o1, F.o2},
                      {F.nul, F.o1}, // null has its own shadow cell
                      {F.o1, F.nul},
                      {F.o0, F.o1}}) { // size 0, its own base vs o1
    Expr in = F.sel(F.st(S, i, v), j);
    RewriteStats st;
    Expr out = F.rewrite(in, P1(), st);
    CHECK(out == F.sel(S, j), "shadow skip " << str(in) << " -> " << str(out));
    CHECK(st.get("shadow.distinct-base") > 0 && st.get("premise.layout") > 0,
          "shadow skip: counters");
    noAInb(st, "shadow skip");
    CHECK(F.equivalentUnder(in, out, F.t), "shadow skip: valid under layout");
    CHECK(!F.equivalent(in, out, false), "shadow skip: needs the layout");
    // with A-INB off: the same (LAYOUT is not A-INB)
    SeaMOptions noInb = P1();
    noInb.assumeInBounds = false;
    RewriteStats st2;
    CHECK(F.rewrite(in, noInb, st2) == out, "shadow skip with A-INB off");
  }
  // a mixed key ite(a, null, o2) against o1: both leaves by identity
  {
    Expr in = F.sel(F.st(S, mk<ITE>(F.a, F.nul, F.o2), v), F.o1);
    RewriteStats st;
    Expr out = F.rewrite(in, P1(), st);
    CHECK(out == F.sel(S, F.o1), "shadow mixed key: " << str(out));
    noAInb(st, "shadow mixed key");
    CHECK(F.equivalentUnder(in, out, F.t), "shadow mixed key: valid");
  }
  // the same key: a hit (premise-free)
  {
    Expr in = F.sel(F.st(S, F.nul, v), F.nul);
    RewriteStats st;
    CHECK(F.rewrite(in, P1(), st) == v, "shadow hit on null");
  }
  // not decided by identity: equal numerals (o0 is placed at o3's base),
  // a nonzero offset (not a key): kept or expanded, valid with no premise
  for (auto [i, j] : {std::pair<Expr, Expr>{F.o3, F.o0},
                      {F.add({F.o1, F.num(4)}), F.o2},
                      {F.add({F.o1, F.x}), F.o2}}) {
    Expr in = F.sel(F.st(S, i, v), j);
    RewriteStats st;
    Expr out = F.rewrite(in, P1(), st);
    CHECK(out != F.sel(S, j) && st.get("shadow.distinct-base") == 0,
          "shadow undecided: " << str(in) << " -> " << str(out));
    noAInb(st, "shadow undecided");
    CHECK(F.equivalent(in, out, false), "shadow undecided: premise-free");
  }
  // control: the same read in a data memory uses A-INB
  {
    RewriteStats st;
    Expr in = F.sel(F.st(F.m, F.o1, v), F.nul);
    CHECK(F.rewrite(in, P1(), st) == F.sel(F.m, F.nul) &&
              st.get("cmp.size0-vacuous") > 0,
          "data memory: cmp.size0-vacuous");
  }
  // through an ite of memories (a join)
  {
    Expr in = F.sel(mk<ITE>(F.a, F.st(S, F.o1, v), S), F.o2);
    RewriteStats st;
    Expr out = F.rewrite(in, P1(), st);
    CHECK(out == F.sel(S, F.o2), "shadow join: " << str(out));
    noAInb(st, "shadow join");
  }
  // WOW-COMMUTE by identity (LAYOUT), into allocation order
  {
    SeaMOptions sm; // Store-Map on (the commutes are its rules)
    Expr in = F.st(F.st(S, F.o2, v), F.o1, w);
    RewriteStats st;
    Expr out = F.rewrite(in, sm, st);
    CHECK(out == F.st(F.st(S, F.o1, w), F.o2, v),
          "shadow commute: " << str(out));
    CHECK(st.get("wow.commute") > 0 && st.get("premise.layout") > 0,
          "shadow commute: counters");
    noAInb(st, "shadow commute");
    CHECK(F.equivalentUnder(in, out, F.t), "shadow commute: valid");
    // equal numerals: no commute
    RewriteStats st2;
    Expr in2 = F.st(F.st(S, F.o3, v), F.o0, w);
    CHECK(F.rewrite(in2, sm, st2) == in2, "shadow: no commute o0 / o3");
  }
  // the Decision: rule and premise recorded
  {
    SeaMRewriter rw(F.efac, F.reg, P1());
    seam::Decision d;
    CHECK(rw.rules().decidePair(F.o1, F.nul, d, false, true) == seam::Tri::No &&
              d.uses(seam::PremLayout) && !d.uses(seam::PremAInb) &&
              d.reasons.size() == 1 &&
              std::string(d.reasons[0].rule) == "shadow.distinct-base",
          "decision: shadow.distinct-base, LAYOUT");
    seam::Decision d2;
    CHECK(rw.rules().decidePair(F.o1, F.nul, d2, false, false) ==
              seam::Tri::Unknown,
          "decision: a program compare is not decided by identity");
    CHECK(rw.rules().shadowMemory(F.st(S, F.o1, v)) &&
              !rw.rules().shadowMemory(F.st(F.m, F.o1, v)),
          "shadowMemory");
  }
}

//===----------------------------------------------------------------------===//
// random terms: sound, and a normal form
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
  /// also pointers that are sums over ites with more than maxPullLeaves
  /// leaves (the ARM case), and ites of numeral offsets
  bool big = false;
  Gen(Fixture &F, unsigned long seed, bool big = false)
      : F(F), rng(seed), big(big) {}

  Expr bigBase(unsigned depth) {
    if (depth == 0 || rng.next(4) == 0) {
      Expr b = rng.next(5) == 0 ? F.o5 : base();
      return rng.next(3) == 0 ? F.add({b, F.num(4 * rng.next(4))}) : b;
    }
    return mk<ITE>(cond(), bigBase(depth - 1), bigBase(depth - 1));
  }
  Expr bigOffset(unsigned depth) {
    if (depth == 0 || rng.next(3) == 0)
      return rng.next(5) == 0 ? F.x : F.num(4 * rng.next(8));
    return mk<ITE>(cond(), bigOffset(depth - 1), bigOffset(depth - 1));
  }

  Expr offset() {
    switch (rng.next(7)) {
    case 0:
      return F.num(0);
    case 1:
      return F.num(4);
    case 2:
      return F.num(8);
    case 3:
      return F.x;
    case 4:
      return F.y;
    case 5:
      return F.add({F.x, F.num(4)});
    default:
      return F.num(12);
    }
  }
  Expr base() {
    switch (rng.next(3)) {
    case 0:
      return F.o1;
    case 1:
      return F.o2;
    default:
      return F.o3;
    }
  }
  Expr cond() {
    switch (rng.next(3)) {
    case 0:
      return F.a;
    case 1:
      return F.b;
    default:
      return F.c;
    }
  }
  Expr ptr(unsigned depth) {
    if (big) {
      switch (rng.next(6)) {
      case 0:
        return F.add({bigBase(3), bigOffset(rng.next(3))});
      case 1:
        return F.add({base(), bigOffset(3)});
      default:
        break;
      }
    }
    unsigned k = rng.next(depth ? 6 : 4);
    switch (k) {
    case 0:
      return base();
    case 1:
    case 2:
      return F.add({base(), offset()});
    case 3:
      return F.add({F.add({base(), offset()}), F.num(4 * rng.next(3))});
    case 4:
      return mk<ITE>(cond(), ptr(depth - 1), ptr(depth - 1));
    default:
      return F.add({mk<ITE>(cond(), base(), base()), offset()});
    }
  }
  Expr value() { return rng.next(3) == 0 ? F.z : F.num(1 + rng.next(5)); }
  Expr mem(unsigned depth) {
    if (depth == 0)
      return rng.next(2) ? F.m : F.m2;
    switch (rng.next(5)) {
    case 0:
      return mk<ITE>(cond(), mem(depth - 1), mem(depth - 1));
    default:
      return F.st(mem(depth - 1), ptr(1), value());
    }
  }
  Expr read() { return F.sel(mem(4), ptr(1)); }
};

void testRandom(unsigned count, bool big = false) {
  Fixture F;
  Gen g(F, big ? 20261101 : 20260501, big);
  unsigned unsound = 0, notNormal = 0, noInbUnsound = 0, resolved = 0;
  uint64_t armSkips = 0;
  for (unsigned i = 0; i < count; ++i) {
    Expr in = g.read();
    if (getenv("SEAM_TEST_VERBOSE"))
      llvm::errs() << "random " << i << ": " << str(in) << "\n";
    RewriteStats st;
    Expr out = F.rewrite(in, SeaMOptions(), st);
    armSkips += st.get("arm.skip");
    if (getenv("SEAM_TEST_VERBOSE"))
      llvm::errs() << "  rewritten: " << str(out) << "\n";
    if (!isOpX<SELECT>(out) && !isOpX<ITE>(out))
      ++resolved;
    if (getenv("SEAM_TEST_VERBOSE"))
      llvm::errs() << "  checking\n";
    if (!F.equivalent(in, out, true)) {
      ++unsound;
      llvm::errs() << "FAIL: random " << i << " unsound: " << str(in) << " ~> "
                   << str(out) << "\n";
    }
    if (getenv("SEAM_TEST_VERBOSE"))
      llvm::errs() << "  checked\n";
    RewriteStats st2;
    Expr again = F.rewrite(out, SeaMOptions(), st2);
    if (again != out) {
      ++notNormal;
      if (notNormal <= 3)
        llvm::errs() << "FAIL: random " << i
                     << " not a normal form: " << str(out) << " ~> "
                     << str(again) << "\n";
    }
    if (getenv("SEAM_TEST_VERBOSE"))
      llvm::errs() << "  idempotent\n";
    // a program compare of two pointers: valid over plain bit-vectors (no
    // layout), A-INB never used
    {
      Expr cmp = mk<PTR_EQ>(g.ptr(1), g.ptr(1));
      RewriteStats stc;
      Expr oc = F.rewrite(cmp, SeaMOptions(), stc);
      if (stc.get("cmp.diff-base") != 0 || !F.equivalent(cmp, oc, false)) {
        ++noInbUnsound;
        llvm::errs() << "FAIL: random " << i << " program compare: " << str(cmp)
                     << " ~> " << str(oc) << "\n";
      }
    }
    SeaMOptions noInb;
    noInb.assumeInBounds = false;
    RewriteStats st3;
    Expr o3 = F.rewrite(in, noInb, st3);
    if (getenv("SEAM_TEST_VERBOSE"))
      llvm::errs() << "  noinb rewritten: " << str(o3) << "\n";
    if (st3.get("cmp.diff-base") != 0 || !F.equivalent(in, o3, false)) {
      ++noInbUnsound;
      llvm::errs() << "FAIL: random " << i << " with A-INB off: " << str(in)
                   << " ~> " << str(o3) << "\n";
    }
  }
  CHECK(!big || armSkips > 0, "random-arm: ARM never fired");
  CHECK(unsound == 0, "random: " << unsound << " unsound rewrites");
  CHECK(noInbUnsound == 0, "random: A-INB off: " << noInbUnsound);
  CHECK(notNormal == 0,
        "random: " << notNormal << " results are not normal forms");
  llvm::outs() << (big ? "random-arm" : "random") << ": terms=" << count
               << " arm-skips=" << armSkips << " resolved-to-value=" << resolved
               << " unsound=" << unsound << " not-normal=" << notNormal << "\n";
}

} // namespace

/// Provenance: a decision over several leaf pairs
/// reports every rule and premise it used, not the first one's. The first
/// pair below is decided by the same base (no assumption), the second needs
/// A-INB (two objects): the Decision must carry both, and the counters of the
/// rewrite that uses it must show the premise.
/// ptr.load-resolve (PtrSort S1.6b): a loaded pointer whose bits are a
/// stored pointer is that pointer; through ites of loaded values within the
/// leaf bound, or over the DAG with resolveIteNodes (S4)
void testLoadResolve() {
  Fixture F;
  auto bits = [&](const Expr &p) { return mk<PTR_TO_BITS>(p); };
  Expr ld = F.sel(F.m, F.q); // a load nothing resolves
  F.expect("load-resolve", mk<PTR_FROM_BITS>(bits(F.add({F.o1, F.num(4)}))),
           F.add({F.o1, F.num(4)}), {"ptr.load-resolve"});
  F.expect("load-resolve-ite", mk<PTR_FROM_BITS>(mk<ITE>(F.a, bits(F.o1), ld)),
           mk<ITE>(F.a, F.o1, mk<PTR_FROM_BITS>(ld)), {"ptr.load-resolve-ite"});
  // six leaves: over the leaf bound (4); a DAG of 5 ites
  Expr conds[] = {F.a, F.b, F.c, F.a, F.b};
  Expr ptrs[] = {F.o1, F.o2, F.o3, F.o5, F.add({F.o1, F.num(8)}), F.o4};
  Expr it = bits(ptrs[5]), want = ptrs[5];
  for (int k = 4; k >= 0; --k) {
    it = mk<ITE>(conds[k], bits(ptrs[k]), it);
    want = mk<ITE>(conds[k], ptrs[k], want);
  }
  Expr in = mk<PTR_FROM_BITS>(it);
  RewriteStats st;
  CHECK(F.rewrite(in, P1(), st) == in, "six leaves: over the leaf bound");
  SeaMOptions dag = P1();
  dag.resolveIteNodes = 8;
  RewriteStats sd;
  Expr out = F.rewrite(in, dag, sd);
  CHECK(sd.get("ptr.load-resolve-ite-dag") == 1 && out == want &&
            F.wellTyped(out) && F.equivalent(in, out, false),
        "dag resolve: " << str(out));
  // over the node bound: unchanged
  dag.resolveIteNodes = 4;
  RewriteStats s4;
  CHECK(F.rewrite(in, dag, s4) == in, "dag resolve: node bound");
}

void testProvenance() {
  Fixture F;
  Expr v = F.var("v");
  Expr near = F.add({F.o1, F.num(4)});
  Expr far = F.add({F.o1, F.num(8)});
  Expr other = F.add({F.o2, F.num(4)});
  auto hasReason = [](const seam::Decision &d, const char *rule,
                      unsigned premises) {
    for (const seam::Decision::Reason &r : d.reasons)
      if (std::string(r.rule) == rule && r.premises == premises)
        return true;
    return false;
  };
  {
    RewriteStats stats;
    seam::Rules rules(F.efac, F.reg, stats, P1());
    // ite(a, o1 + 4, o2 + 4) = o1 + 8: (same base, different numerals), then
    // (different objects)
    Expr l = mk<ITE>(F.a, near, other);
    seam::Decision d;
    CHECK(rules.decideEq(l, far, d, /*diffBase=*/true) == seam::Tri::No,
          "provenance: decided No");
    CHECK(hasReason(d, "cmp.same-base", seam::PremNone) &&
              hasReason(d, "cmp.diff-base", seam::PremAInb) &&
              d.reasons.size() == 2,
          "provenance: both leaf pairs reported, " << d.reasons.size());
    CHECK(d.premises == seam::PremAInb && d.uses(seam::PremAInb),
          "provenance: the premise of the second pair is not lost");
    // the order of the pairs does not matter
    seam::Decision d2;
    CHECK(rules.decideEq(mk<ITE>(F.a, other, near), far, d2, true) ==
                  seam::Tri::No &&
              d2.premises == seam::PremAInb && d2.reasons.size() == 2,
          "provenance: pair order");
    // a program compare (diffBase false) does not decide the cross pair
    seam::Decision d3;
    CHECK(rules.decideEq(l, far, d3, /*diffBase=*/false) ==
                  seam::Tri::Unknown &&
              d3.reasons.empty(),
          "provenance: no decision, no reason");
    // one base only: no premise at all
    seam::Decision d4;
    CHECK(rules.decideEq(mk<ITE>(F.a, near, F.add({F.o1, F.num(12)})), far, d4,
                         true) == seam::Tri::No &&
              d4.premises == seam::PremNone && d4.reasons.size() == 1,
          "provenance: same base only needs nothing");
    // a decision without a premise stays separate from one with it: a Yes
    seam::Decision d5;
    CHECK(rules.decideEq(far, far, d5, true) == seam::Tri::Yes &&
              hasReason(d5, "cmp.refl", seam::PremNone) &&
              d5.premises == seam::PremNone,
          "provenance: refl");
  }
  // the counters of the rewrite: every reason, and premise.a-inb once
  {
    Expr l = mk<ITE>(F.a, near, other);
    Expr in = F.sel(F.st(F.m, l, v), far);
    RewriteStats st;
    Expr out = F.rewrite(in, P1(), st);
    CHECK(out == F.sel(F.m, far), "provenance rewrite: " << str(out));
    CHECK(st.get("row.store-skip") == 1 && st.get("cmp.same-base") >= 1 &&
              st.get("cmp.diff-base") >= 1,
          "provenance: every reason counted");
    CHECK(st.get("premise.a-inb") == 1, "provenance: premise counted once");
    CHECK(F.equivalent(in, out, true), "provenance rewrite: unsound");
    // same base only: no premise counted
    Expr in2 = F.sel(F.st(F.m, near, v), far);
    RewriteStats st2;
    Expr out2 = F.rewrite(in2, P1(), st2);
    CHECK(out2 == F.sel(F.m, far) && st2.get("premise.a-inb") == 0,
          "provenance: same-base skip uses no premise");
    // A-INB off: the cross pair is not decided, no premise
    SeaMOptions off = P1();
    off.assumeInBounds = false;
    RewriteStats st3;
    Expr out3 = F.rewrite(in, off, st3);
    CHECK(st3.get("premise.a-inb") == 0 && st3.get("cmp.diff-base") == 0 &&
              F.equivalent(in, out3, false),
          "provenance: A-INB off, no premise");
  }
  // ARM across bases carries the premise; ARM on one base does not
  {
    Expr big = mk<ITE>(F.a, mk<ITE>(F.b, F.o1, F.o2),
                       mk<ITE>(F.c, F.o3, mk<ITE>(F.b, F.o1, F.o4)));
    Expr sum = F.add({big, F.num(8)});
    RewriteStats stats;
    seam::Rules rules(F.efac, F.reg, stats, P1());
    seam::Decision d;
    CHECK(rules.decideEq(sum, F.add({F.o5, F.y}), d, true) == seam::Tri::No &&
              hasReason(d, "arm.skip", seam::PremAInb) &&
              d.premises == seam::PremAInb,
          "provenance: ARM across bases uses A-INB");
    Expr offs =
        mk<ITE>(F.a, mk<ITE>(F.b, F.num(4), F.num(8)),
                mk<ITE>(F.c, F.num(12), mk<ITE>(F.b, F.num(16), F.num(20))));
    seam::Decision d2;
    CHECK(rules.decideEq(F.add({F.o1, offs}), F.add({F.o1, F.num(100)}), d2,
                         true) == seam::Tri::No &&
              hasReason(d2, "arm.skip", seam::PremNone) &&
              d2.premises == seam::PremNone,
          "provenance: ARM on one base needs no assumption");
    RewriteStats st;
    Expr in = F.sel(F.st(F.m, sum, v), F.add({F.o5, F.y}));
    F.rewrite(in, P1(), st);
    CHECK(st.get("arm.skip-diff-base") == 1 && st.get("premise.a-inb") == 1,
          "provenance: cross-base ARM skip counted");
    RewriteStats st2;
    F.rewrite(
        F.sel(F.st(F.m, F.add({F.o1, offs}), v), F.add({F.o1, F.num(100)})),
        P1(), st2);
    CHECK(st2.get("arm.skip") >= 1 && st2.get("arm.skip-diff-base") == 0 &&
              st2.get("premise.a-inb") == 0,
          "provenance: same-base ARM skip uses no premise");
  }
  // the commutes of Store-Map name the premise
  {
    RewriteStats st;
    Expr chain = F.st(F.st(F.m, F.add({F.o2, F.num(4)}), v),
                      F.add({F.o1, F.num(4)}), F.var("w"));
    F.rewrite(chain, SeaMOptions(), st);
    CHECK(st.get("wow.commute") >= 1 &&
              st.get("wow.commute") + st.get("smap.commute") ==
                  st.get("premise.a-inb"),
          "provenance: commute counted as a premise use");
  }
}

int main(int argc, char **argv) {
  unsigned randomCount = 300;
  if (argc > 1)
    randomCount = std::atoi(argv[1]);
  struct Test {
    const char *name;
    void (*fn)();
  } tests[] = {{"add-normalize", testAddNormalize},
               {"add-pull-ite", testAddPullIte},
               {"sub", testSub},
               {"compare", testCompare},
               {"compare-ite", testCompareIte},
               {"ite-bool", testIteBool},
               {"row", testRow},
               {"thesis-examples", testThesisExamples},
               {"arm", testArm},
               {"provenance", testProvenance},
               {"load-resolve", testLoadResolve},
               {"null", testNull},
               {"prune-size0", testPruneSize0},
               {"shadow", testShadow}};
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
  before = failures;
  testRandom(randomCount / 2, /*big=*/true);
  llvm::outs() << "test: random-arm " << (failures == before ? "ok" : "FAILED")
               << "\n";
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
