// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- MemOpsTest.cpp - SeaM memory-operation terms --------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if a
// check fails. Run by test/Expr/memops.test.
//
// What is tested (include/sea/Expr/MemOps.hh, SeaMRules.hh rowMemOp;
// docs/design/SeaM-memops.md "Validation"):
//  - the read equations: the boundary translation (MemOpTranslator) of a read
//    of mcopy / mset / mhavoc against an independent guarded-store expansion
//    of the operation, checked at small widths: at 4-bit addresses
//    every address and length symbolic, 8-bit cells, so n = 0, the maximal
//    n, and ranges that wrap around 2^4 are all covered; overlap (Ms = Md),
//    different memory versions (Ms a store over Md), memory ites, stores and
//    nested terms over a term;
//  - repeated reads of one mhavoc read the one array H;
//  - store-map interaction: a store-map over a term and a term over a
//    store-map, rewritten by the SeaM rules, then the boundary;
//  - the rules (rowMemOp): skip across objects (A-INB, with the premise of
//    the whole destination range), numeral hit / skip, the circular decision
//    (4 bits: d = 14, n = 4, j = 0 is a hit), ARM ranges, n = 0, undecided;
//    every rewrite checked against the expansion under its premises;
//  - the boundary without the rules (expandUnknownRow off, a rewrite stopped
//    at maxRuleCalls) and its refusal of a term that is not read.
//
// Soundness is checked with Yices 2 (QF_ABV): `premises and in != out` unsat.
//
//===----------------------------------------------------------------------===//

// boost hash templates are only forward-declared by Expr.hh; define them here
// (a Release build does not export the instantiations from libSeaExpr).
#include "sea/Expr/MemOps.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/SeaMRules.hh"
#include "sea/Expr/Smt/Yices2SolverImpl.hh"
#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace expr;

namespace {

/// \p e as the solvers see it: nodes and markers erased (S2c / S2d: rule
/// results and conditions may hold them)
Expr erased(const Expr &e) {
  PtrIndexExpander x;
  return x.has(e) ? x.expand(e) : e;
}

int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      llvm::errs() << "FAIL: " << msg << " [" #cond "] (line " << __LINE__     \
                   << ")\n";                                                   \
    }                                                                          \
  } while (0)

constexpr unsigned kW = 4; // address width: 16 cells
constexpr unsigned kV = 8; // cell width

std::string str(const Expr &e) {
  std::ostringstream os;
  os << *e;
  return os.str();
}

template <typename Op>
unsigned countOp(const Expr &root) {
  std::set<ENode *> seen;
  std::vector<ENode *> todo{root.get()};
  unsigned n = 0;
  while (!todo.empty()) {
    ENode *e = todo.back();
    todo.pop_back();
    if (!seen.insert(e).second)
      continue;
    if (isOpX<Op>(Expr(e)))
      ++n;
    for (size_t i = 0; i < e->arity(); ++i)
      todo.push_back(e->arg(i));
  }
  return n;
}

struct Fixture {
  ExprFactory efac;
  BaseRegistry reg;
  Expr memSort;
  Expr Md, Ms, H, M1, M2;
  Expr d, s, n, j, j2, k, v, x, y, c;
  Expr oa, ob; // objects: a at 2, size 4; b at 8, size 6
  Expr on;     // the null object: base 0, size 0 (SeaM-null.md)
  ExprVector layout;

  Fixture()
      : memSort(sort::arrayTy(op::ptr::ptrsort(kW, efac),
                              op::bv::bvsort(kV, efac))) {
    Md = mem("Md");
    Ms = mem("Ms");
    H = mem("H");
    M1 = mem("M1");
    M2 = mem("M2");
    // addresses are pointers (PtrSort S2e); n, x are integers
    d = ptr("d");
    s = ptr("s");
    n = var("n");
    j = ptr("j");
    j2 = ptr("j2");
    k = var("k"); // an integer offset
    x = var("x");
    v = op::bv::bvConst(mkTerm<std::string>("v", efac), kV);
    y = op::bv::bvConst(mkTerm<std::string>("y", efac), kV);
    c = op::bind::boolConst(mkTerm<std::string>("c", efac));
    auto obj = [&](const char *name, unsigned long base, unsigned long size) {
      Expr e = op::ptr::ptrConst(mkTerm<std::string>(name, efac), kW);
      reg.add(e, name, "heap", llvm::APInt(kW, base), llvm::APInt(kW, size), 1);
      reg.addAlias(op::bv::bvConst(mkTerm<std::string>(name, efac), kW), e);
      layout.push_back(mk<EQ>(mk<ADDR>(e), num(base)));
      return e;
    };
    oa = obj("obj.a", 2, 4);
    ob = obj("obj.b", 8, 6);
    on = obj("obj.null", 0, 0);
  }
  Expr y4() { return var("y4"); }
  Expr num(unsigned long k) { return op::bv::bvnum(mpz_class(k), kW, efac); }
  Expr var(const char *name) {
    return op::bv::bvConst(mkTerm<std::string>(name, efac), kW);
  }
  Expr ptr(const char *name) {
    return op::ptr::ptrConst(mkTerm<std::string>(name, efac), kW);
  }
  /// the pointer whose address is the numeral k
  Expr pnum(unsigned long k) { return mk<PTR_FROM_INT>(num(k)); }
  Expr mem(const char *name) {
    return op::bind::mkConst(mkTerm<std::string>(name, efac), memSort);
  }
  /// a + b: over a pointer a node (op::ptr::addressPlus, as OpSem's
  /// addr.add), else a bvadd
  Expr add(const Expr &a, const Expr &b) {
    if (isPtr(a))
      return op::ptr::addressPlus(a, b);
    ExprVector args{a, b};
    return mknary<BADD>(args.begin(), args.end());
  }
  static bool isPtr(Expr e) {
    while (isOpX<ITE>(e))
      e = e->arg(1);
    return op::ptr::isPtrConst(e) || op::ptr::isPtrIndex(e) ||
           isOpX<PTR_FROM_BITS>(e) || isOpX<PTR_FROM_INT>(e);
  }
  /// the strict typing of the pointer sort (PtrSort S2e)
  static bool wellTyped(const Expr &e) {
    TypeChecker tc;
    return !isOpX<ERROR_TY>(tc.typeOf(e));
  }
  Expr at(const Expr &base, unsigned long off) {
    return off == 0 ? base : add(base, num(off));
  }
  Expr sel(const Expr &m, const Expr &i) { return op::array::select(m, i); }

  /// The reference read of memory \p m at \p j: the guarded-store
  /// expansion of every term (independent of MemOps.hh): a term writes cells
  /// d + k for k in 0..2^w - 1 guarded by k <u n (distinct cells: n < 2^w),
  /// so the read is ite(k <u n and d + k = j, <k-th value>, ...) over the
  /// read of the memory below; stores and ites read as in the array theory.
  Expr ref(const Expr &m, const Expr &j) {
    // over the erased terms: bit-vector addresses (the boundary's meaning)
    return refB(erased(m), erased(j));
  }
  Expr refB(const Expr &m, const Expr &j) {
    if (isOpX<STORE>(m))
      return mk<ITE>(mk<EQ>(m->arg(1), j), m->arg(2), refB(m->arg(0), j));
    if (isOpX<ITE>(m))
      return mk<ITE>(m->arg(0), refB(m->arg(1), j), refB(m->arg(2), j));
    if (!isMemOpTerm(m))
      return sel(m, j);
    Expr dd = memOpDst(m), nn = memOpLen(m);
    Expr res = refB(memOpBase(m), j);
    for (unsigned long k = 0; k < (1ul << kW); ++k) {
      Expr kk = num(k), cell = add(dd, kk);
      Expr val = isOpX<MEM_COPY>(m)  ? refB(m->arg(1), add(m->arg(3), kk))
                 : isOpX<MEM_SET>(m) ? m->arg(3)
                                     : sel(m->arg(1), cell);
      res = mk<ITE>(mk<AND>(mk<BULT>(kk, nn), mk<EQ>(cell, j)), val, res);
    }
    return res;
  }

  /// `premises and a != b` is unsat (Yices); the store-maps of the terms are
  /// expanded first (StoreMapExpander), then the terms translated.
  bool equivalent(const Expr &a, const Expr &b, const ExprVector &prem = {}) {
    StoreMapExpander ex;
    WideExpander wx;
    MemOpTranslator tr;
    std::string why;
    Expr ta = tr.translate(wx.expand(ex.expand(a)), why);
    Expr tb = tr.translate(wx.expand(ex.expand(b)), why);
    if (!ta || !tb) {
      llvm::errs() << "translation refused: " << why << "\n";
      return false;
    }
    // rule-made addresses are PTR_INDEX nodes (PtrSort rev 5, S1): the
    // boundary expands them, so does the check
    PtrIndexExpander px;
    ta = px.expand(ta);
    tb = px.expand(tb);
    if (countOp<MEM_COPY>(ta) + countOp<MEM_SET>(ta) + countOp<MEM_HAVOC>(ta) +
            countOp<MEM_COPY>(tb) + countOp<MEM_SET>(tb) +
            countOp<MEM_HAVOC>(tb) !=
        0)
      return false;
    sea::solver::yices_solver_impl sv(efac);
    for (const Expr &p : prem)
      sv.add(erased(p));
    sv.add(erased(mk<NEG>(mk<EQ>(ta, tb))));
    return sv.check() == sea::solver::SolverResult::UNSAT;
  }
  /// is the conjunction satisfiable (a premise that no model meets would make
  /// every check pass)
  bool satisfiable(const ExprVector &prem) {
    sea::solver::yices_solver_impl sv(efac);
    for (const Expr &p : prem)
      sv.add(erased(p));
    return sv.check() == sea::solver::SolverResult::SAT;
  }
};

Expr mcopy(const Expr &md, const Expr &ms, const Expr &d, const Expr &s,
           const Expr &n) {
  return op::array::memCopy(md, ms, d, s, n);
}

//===----------------------------------------------------------------------===//
// the read equations vs the guarded-store expansion
//===----------------------------------------------------------------------===//

void testReadEquations() {
  Fixture F;
  struct Case {
    const char *name;
    Expr term;
  };
  Expr st = op::array::store(F.Md, F.x, F.y);
  std::vector<Case> cases = {
      {"mcopy", mcopy(F.Md, F.Ms, F.d, F.s, F.n)},
      {"mcopy-overlap", mcopy(F.Md, F.Md, F.d, F.s, F.n)},
      {"mcopy-version", mcopy(F.Md, st, F.d, F.s, F.n)},
      {"mcopy-over-store", mcopy(st, F.Md, F.d, F.s, F.n)},
      {"mcopy-ite", mcopy(mk<ITE>(F.c, F.M1, F.M2), F.Md, F.d, F.s, F.n)},
      {"mcopy-n0", mcopy(F.Md, F.Ms, F.d, F.s, F.num(0))},
      {"mcopy-nmax", mcopy(F.Md, F.Md, F.d, F.s, F.num(15))},
      {"mset", op::array::memSet(F.Md, F.d, F.n, F.v)},
      {"mset-nmax", op::array::memSet(F.Md, F.d, F.num(15), F.v)},
      {"mhavoc", op::array::memHavoc(F.Md, F.H, F.d, F.n)},
      {"nested",
       mcopy(op::array::memSet(F.Md, F.x, F.n, F.v),
             op::array::memHavoc(F.Md, F.H, F.d, F.n), F.d, F.s, F.n)},
  };
  for (const Case &cs : cases) {
    bool ok = F.equivalent(F.sel(cs.term, F.j), F.ref(cs.term, F.j));
    CHECK(ok, "read equation " << cs.name);
    llvm::outs() << "read-eq: " << cs.name << " " << (ok ? "ok" : "FAILED")
                 << "\n";
  }
  // a store over a term, and a memory ite of terms, read at j
  Expr t = op::array::memSet(F.Md, F.d, F.n, F.v);
  Expr over = op::array::store(t, F.x, F.y);
  CHECK(F.equivalent(F.sel(over, F.j), F.ref(over, F.j)), "store over a term");
  Expr ite = mk<ITE>(F.c, t, mcopy(F.Md, F.Ms, F.d, F.s, F.n));
  CHECK(F.equivalent(F.sel(ite, F.j), F.ref(ite, F.j)), "ite of terms");
  // Wraparound example: d = 14, n = 4: j = 0 is a hit
  Expr w = op::array::memSet(F.Md, F.pnum(14), F.num(4), F.v);
  CHECK(F.equivalent(F.sel(w, F.pnum(0)), F.v), "wrap: 0 in [14, 14 + 4)");
  CHECK(F.equivalent(F.sel(w, F.pnum(2)), F.sel(F.Md, F.pnum(2))),
        "wrap: 2 not in [14, 14 + 4)");
}

void testHavocOneArray() {
  Fixture F;
  Expr t = op::array::memHavoc(F.Md, F.H, F.d, F.n);
  // two reads at equal indices read the same value; inside the range it is
  // H's cell, the same H for every read
  CHECK(F.equivalent(F.sel(t, F.j), F.sel(t, F.j2), {mk<EQ>(F.j, F.j2)}),
        "mhavoc: equal reads");
  CHECK(F.equivalent(F.sel(t, F.j), F.sel(F.H, F.j),
                     {mk<BULT>(mk<PTR_DIFF>(F.j, F.d), F.n)}),
        "mhavoc: one H");
  // translation shares the read: one select of H per index
  MemOpTranslator tr;
  std::string why;
  Expr both = mk<EQ>(F.sel(t, F.j), F.sel(t, F.j));
  Expr out = tr.translate(both, why);
  CHECK(out && countOp<SELECT>(out) == 2, "one H read and one Md read");
}

//===----------------------------------------------------------------------===//
// the rules
//===----------------------------------------------------------------------===//

struct RuleRun {
  Fixture &F;
  SeaMRewriter rw;
  RuleRun(Fixture &F, SeaMOptions o = SeaMOptions(), RewriteOptions ro = {})
      : F(F), rw(F.efac, F.reg, o, ro) {}
  Expr read(const Expr &m, const Expr &j) {
    CHECK(Fixture::wellTyped(F.sel(m, j)),
          "ill-typed read " << str(F.sel(m, j)));
    return rw.rewrite(F.sel(m, rw.rewrite(j)));
  }
  /// the rewriter on a well-typed term
  Expr rewrite(const Expr &t) {
    CHECK(Fixture::wellTyped(t), "ill-typed input " << str(t));
    return rw.rewrite(t);
  }
  uint64_t get(const char *k) const { return rw.stats().get(k); }
};

/// The premises a firing at select(T, j) states for A-INB (as firingPremise):
/// the layout, j in bounds (1 unit), the destination range in bounds.
ExprVector inbPremise(Fixture &F, SeaMRewriter &rw, const Expr &t,
                      const Expr &j) {
  ExprVector p = F.layout;
  bool violated = false;
  p.push_back(rw.rules().inBounds(j, 1, violated));
  p.push_back(rw.rules().rangeInBounds(memOpDst(t), memOpLen(t), violated));
  CHECK(!violated, "premise violated statically");
  return p;
}

void testRules() {
  Fixture F;
  // skip across objects: a read of b over a copy into a (symbolic n, d, j)
  {
    RuleRun R(F);
    Expr t = mcopy(F.Md, F.Ms, F.add(F.oa, F.x), F.s, F.n);
    Expr j = F.add(F.ob, F.k);
    Expr r = R.read(t, j);
    CHECK(R.get("row.mcopy-skip") == 1 && R.get("premise.a-inb") == 1,
          "cross-object skip: " << str(r));
    CHECK(countOp<MEM_COPY>(r) == 0, "skip leaves the term");
    ExprVector prem = inbPremise(F, R.rw, t, j);
    CHECK(F.satisfiable(prem), "the skip premise is satisfiable");
    CHECK(F.equivalent(F.ref(t, j), r, prem),
          "cross-object skip under A-INB (range premise)");
    // without the range premise it is not valid (n may run into b)
    ExprVector part = F.layout;
    bool vio = false;
    part.push_back(R.rw.rules().inBounds(j, 1, vio));
    CHECK(!F.equivalent(F.ref(t, j), r, part),
          "the skip needs the destination range");
  }
  // skip across objects by ARM: j = ite(5 pointers into b) + y has no view
  // (the ite is not pulled: maxPullLeaves), its provenance is b only
  {
    RuleRun R(F);
    Expr c2 = op::bind::boolConst(mkTerm<std::string>("c2", F.efac));
    Expr c3 = op::bind::boolConst(mkTerm<std::string>("c3", F.efac));
    Expr c4 = op::bind::boolConst(mkTerm<std::string>("c4", F.efac));
    Expr big = mk<ITE>(
        F.c, F.at(F.ob, 1),
        mk<ITE>(c2, F.at(F.ob, 2),
                mk<ITE>(c3, F.ob, mk<ITE>(c4, F.at(F.ob, 3), F.at(F.ob, 4)))));
    Expr j = F.add(big, F.y4());
    Expr t = mcopy(F.Md, F.Ms, F.add(F.oa, F.x), F.s, F.n);
    Expr r = R.read(t, j);
    CHECK(R.get("row.mcopy-skip") == 1 && R.get("arm.skip-diff-base") == 1 &&
              R.get("premise.a-inb") == 1,
          "ARM cross-object skip: " << str(r));
    ExprVector prem = inbPremise(F, R.rw, t, R.rewrite(j));
    CHECK(F.satisfiable(prem), "ARM skip premise satisfiable");
    CHECK(F.equivalent(F.ref(t, j), r, prem), "ARM cross-object skip exact");
  }
  // no A-INB: no cross-object skip, the read stays exact
  {
    SeaMOptions o;
    o.assumeInBounds = false;
    RuleRun R(F, o);
    Expr t = mcopy(F.Md, F.Ms, F.add(F.oa, F.x), F.s, F.n);
    Expr j = F.add(F.ob, F.k);
    Expr r = R.read(t, j);
    CHECK(R.get("row.mcopy-skip") == 0 && R.get("row.mcopy") == 1,
          "no skip without A-INB");
    CHECK(F.equivalent(F.ref(t, j), r), "undecided read is exact");
  }
  // same object, numeral offsets: hit and skip, no premise
  {
    RuleRun R(F);
    Expr t = op::array::memSet(F.Md, F.at(F.oa, 1), F.num(2), F.v);
    Expr hit = R.read(t, F.at(F.oa, 2));
    Expr skip = R.read(t, F.at(F.oa, 3));
    CHECK(hit == F.v && R.get("row.mset-hit") == 1, "numeral hit");
    CHECK(R.get("row.mset-skip") == 1 && R.get("premise.a-inb") == 0,
          "numeral skip, no premise");
    CHECK(F.equivalent(F.ref(t, F.at(F.oa, 3)), skip), "skip exact");
  }
  // mcopy hit: the read continues in the source at s + k
  {
    RuleRun R(F);
    Expr t = mcopy(F.Md, F.Ms, F.at(F.ob, 1), F.at(F.oa, 0), F.num(3));
    Expr r = R.read(t, F.at(F.ob, 3));
    CHECK(R.get("row.mcopy-hit") == 1, "mcopy hit");
    CHECK(F.equivalent(r, F.sel(F.Ms, F.at(F.oa, 2)), F.layout),
          "mcopy hit reads Ms[s + 2]");
    CHECK(F.equivalent(F.ref(t, F.at(F.ob, 3)), r), "hit exact");
  }
  // circular decision at 4 bits: d = 14, n = 4: j = 0 is a hit (k = 2), j =
  // 2 is a skip (k = 4); a linear interval test would say the opposite of
  // the first
  {
    RuleRun R(F);
    Expr t = op::array::memSet(F.Md, F.pnum(14), F.num(4), F.v);
    Expr r0 = R.read(t, F.pnum(0));
    Expr r2 = R.read(t, F.pnum(2));
    CHECK(r0 == F.v, "wrap hit: " << str(r0));
    CHECK(R.get("row.mset-skip") == 1, "wrap skip");
    CHECK(F.equivalent(F.ref(t, F.pnum(2)), r2), "wrap skip exact");
  }
  // ARM ranges: j = a + ite(c, 1, 2), d = a + 3, n = 2: k in {14, 15}: skip
  // (residues 14..15 >= 2); n = 0: mem.empty
  {
    RuleRun R(F);
    Expr j = F.add(F.oa, mk<ITE>(F.c, F.num(1), F.num(2)));
    Expr t = op::array::memSet(F.Md, F.at(F.oa, 3), F.num(2), F.v);
    Expr r = R.read(t, j);
    CHECK(countOp<MEM_SET>(r) == 0 && R.get("premise.a-inb") == 0,
          "ite index decided: " << str(r));
    CHECK(F.equivalent(F.ref(t, j), r), "ite index exact");
    Expr e = op::array::memSet(F.Md, F.d, F.num(0), F.v);
    Expr re = R.read(e, F.j);
    CHECK(re == F.sel(F.Md, F.j) && R.get("mem.empty") >= 1, "n = 0");
    // ARM: an ite of 5 leaves is not pulled through the sum (maxPullLeaves),
    // so j = a + ite(...) has offsets [0, 2] by ARM; d = a + 3, n = 2:
    // k = [-3, -1] -> residues 13..15 >= 2: skip (arm.range)
    Expr c2 = op::bind::boolConst(mkTerm<std::string>("c2", F.efac));
    Expr c3 = op::bind::boolConst(mkTerm<std::string>("c3", F.efac));
    Expr c4 = op::bind::boolConst(mkTerm<std::string>("c4", F.efac));
    Expr big = mk<ITE>(
        F.c, F.num(1),
        mk<ITE>(c2, F.num(2),
                mk<ITE>(c3, F.num(0), mk<ITE>(c4, F.num(1), F.num(2)))));
    Expr jb = F.add(F.oa, big);
    Expr rb = R.read(t, jb);
    CHECK(R.get("arm.range") >= 1 && countOp<MEM_SET>(rb) == 0,
          "ARM range skip: " << str(rb));
    CHECK(F.equivalent(F.ref(t, jb), rb), "ARM range exact");
    // an absolute ite index
    Expr ja = mk<ITE>(F.c, F.pnum(5), F.pnum(6));
    Expr ta = op::array::memSet(F.Md, F.pnum(3), F.num(2), F.v);
    Expr ra = R.read(ta, ja);
    CHECK(countOp<MEM_SET>(ra) == 0, "absolute ite index decided");
    CHECK(F.equivalent(F.ref(ta, ja), ra), "absolute ite exact");
  }
  // undecided: one ite with one compare, exact
  {
    RuleRun R(F);
    Expr t = mcopy(F.Md, F.Ms, F.d, F.s, F.n);
    Expr r = R.read(t, F.j);
    CHECK(R.get("row.mcopy") == 1 && countOp<BULT>(r) == 1,
          "undecided: one compare: " << str(r));
    CHECK(F.equivalent(F.ref(t, F.j), r), "undecided exact");
  }
}

/// Ranges through the null object (SeaM-null.md, point 6): a range of length
/// 0 accesses nothing, even through null; the range premise reduces to n = 0
/// for a size-0 object. A skip over a null (or mixed) destination is valid
/// because A-INB forces the null part of the range empty; a read through null
/// is vacuous because it has positive width.
void testNull() {
  Fixture F;
  // mset into null + x, symbolic n, read of b: skip (cmp.size0-vacuous);
  // the premise forces n = 0: satisfiable with n = 0, not with n = 1
  {
    RuleRun R(F);
    Expr t = op::array::memSet(F.Md, F.add(F.on, F.x), F.n, F.v);
    Expr j = F.add(F.ob, F.k);
    Expr r = R.read(t, j);
    CHECK(R.get("row.mset-skip") == 1 && R.get("cmp.size0-vacuous") == 1 &&
              R.get("premise.a-inb") == 1,
          "null destination: vacuous skip: " << str(r));
    ExprVector prem = inbPremise(F, R.rw, t, j);
    CHECK(F.satisfiable(prem), "null destination: premise satisfiable");
    ExprVector p0 = prem, p1 = prem;
    p0.push_back(mk<EQ>(F.n, F.num(0)));
    p1.push_back(mk<EQ>(F.n, F.num(1)));
    CHECK(F.satisfiable(p0), "null destination: n = 0 admitted");
    CHECK(!F.satisfiable(p1), "null destination: n = 1 violates the range");
    CHECK(F.equivalent(F.ref(t, j), r, prem),
          "null destination: exact under A-INB");
    // without the range premise not valid (x + k may be b's cells)
    ExprVector part = F.layout;
    bool vio = false;
    part.push_back(R.rw.rules().inBounds(j, 1, vio));
    CHECK(!F.equivalent(F.ref(t, j), r, part),
          "null destination: the skip needs the range premise");
  }
  // mcopy into ite(c, null + x, a + x), read of b: both leaf pairs decided
  // (size0-vacuous, diff-base); the premise is guarded, not a violation
  {
    // the read's compare itself: addr.prune-size0, which drops the null leaf
    // of the destination first, is tested in testPruneSize0
    SeaMOptions noPrune;
    noPrune.pruneSize0 = false;
    RuleRun R(F, noPrune);
    Expr dst = mk<ITE>(F.c, F.add(F.on, F.x), F.add(F.oa, F.x));
    Expr t = mcopy(F.Md, F.Ms, dst, F.s, F.n);
    Expr j = F.add(F.ob, F.k);
    Expr r = R.read(t, j);
    CHECK(R.get("row.mcopy-skip") == 1 && R.get("cmp.size0-vacuous") == 1 &&
              R.get("cmp.diff-base") == 1,
          "mixed destination: skip: " << str(r));
    ExprVector prem = inbPremise(F, R.rw, t, j);
    CHECK(F.satisfiable(prem), "mixed destination: premise satisfiable");
    ExprVector pc = prem;
    pc.push_back(F.c);
    pc.push_back(mk<EQ>(F.n, F.num(1)));
    CHECK(!F.satisfiable(pc), "mixed destination: the null part forces n = 0");
    CHECK(F.equivalent(F.ref(t, j), r, prem),
          "mixed destination: exact under A-INB");
  }
  // numeral n = 0 through null: mem.empty, no premise
  {
    RuleRun R(F);
    Expr t = op::array::memSet(F.Md, F.add(F.on, F.x), F.num(0), F.v);
    Expr r = R.read(t, F.j);
    CHECK(r == F.sel(F.Md, F.j) && R.get("mem.empty") == 1 &&
              R.get("premise.a-inb") == 0,
          "n = 0 through null: mem.empty");
    bool vio = false;
    CHECK(isOpX<TRUE>(
              R.rw.rules().rangeInBounds(F.add(F.on, F.x), F.num(0), vio)) &&
              !vio,
          "n = 0 through null: in bounds");
  }
  // a read through null (positive width) of a range into a: vacuous, and a
  // definite violation of its access (sea-vc falls back)
  {
    RuleRun R(F);
    Expr t = op::array::memSet(F.Md, F.add(F.oa, F.x), F.n, F.v);
    Expr j = F.add(F.on, F.y4());
    Expr r = R.read(t, j);
    CHECK(R.get("row.mset-skip") == 1 && R.get("cmp.size0-vacuous") == 1,
          "read through null: vacuous: " << str(r));
    bool vio = false;
    CHECK(isOpX<FALSE>(R.rw.rules().inBounds(j, 1, vio)) && vio,
          "read through null: a definite violation");
  }
  // without A-INB: nothing is vacuous, the read stays exact
  {
    SeaMOptions o;
    o.assumeInBounds = false;
    RuleRun R(F, o);
    Expr t = op::array::memSet(F.Md, F.add(F.on, F.x), F.n, F.v);
    Expr j = F.add(F.ob, F.k);
    Expr r = R.read(t, j);
    CHECK(R.get("cmp.size0-vacuous") == 0 && R.get("row.mset") == 1,
          "no A-INB: undecided");
    CHECK(F.equivalent(F.ref(t, j), r), "no A-INB: exact");
  }
}

/// addr.prune-size0 over ranges (docs/design/SeaM-provenance.md):
/// an operation-level equivalence under the A-INB of
/// the original ranges (rangeInBounds of the destination, and of an mcopy
/// source), checked as the equality of the two memories at a fresh cell j,
/// not as an address equality: with a size-0 leaf, A-INB admits n = 0 with
/// the null guard true, where both operations are no-ops although their
/// addresses differ.
void testPruneSize0() {
  Fixture F;
  Expr nx = F.add(F.on, F.x), ax = F.add(F.oa, F.x), bd = F.add(F.ob, F.k);
  Expr mixed = mk<ITE>(F.c, nx, ax);
  // the premise of a range firing: the layout and every original range
  auto premise = [&](SeaMRewriter &rw, const Expr &t) {
    ExprVector p = F.layout;
    llvm::SmallVector<std::pair<Expr, Expr>, 2> rs;
    rw.rules().accessRanges(t, rs);
    for (const auto &[d, n] : rs) {
      bool vio = false;
      p.push_back(rw.rules().rangeInBounds(d, n, vio));
      CHECK(!vio, "range premise violated statically");
    }
    return p;
  };
  // a mixed destination, symbolic n: the live leaf; equal memories under
  // the range premise, not without it
  {
    RuleRun R(F);
    Expr t = op::array::memSet(F.Md, mixed, F.n, F.v);
    Expr r = R.rewrite(t);
    CHECK(r == op::array::memSet(F.Md, ax, F.n, F.v) &&
              R.get("addr.prune-size0-range") == 1 &&
              R.get("premise.a-inb") == 1,
          "mixed destination: pruned: " << str(r));
    ExprVector prem = premise(R.rw, t);
    CHECK(prem.size() == F.layout.size() + 1, "mixed destination: one range");
    CHECK(F.satisfiable(prem), "mixed destination: premise satisfiable");
    CHECK(F.equivalent(F.ref(t, F.j), F.ref(r, F.j), prem),
          "mixed destination: equal memories under A-INB");
    CHECK(!F.equivalent(F.ref(t, F.j), F.ref(r, F.j), F.layout),
          "mixed destination: not without the range premise");
    // n = 0 with the null guard true is admitted, and there the addresses
    // differ: the check is the operations', not the addresses'
    ExprVector p0 = prem;
    p0.push_back(F.c);
    p0.push_back(mk<EQ>(F.n, F.num(0)));
    p0.push_back(mk<NEG>(mk<EQ>(mixed, ax)));
    CHECK(F.satisfiable(p0),
          "mixed destination: n = 0, null guard true, addresses differ");
    CHECK(F.equivalent(F.ref(t, F.j), F.ref(r, F.j), p0),
          "mixed destination: n = 0: both no-ops");
  }
  // every destination leaf size 0, symbolic n: A-INB gives n = 0, the term
  // is its incoming memory (a no-op, not a violation)
  {
    RuleRun R(F);
    Expr allNull = mk<ITE>(F.c, nx, F.at(F.on, 1));
    Expr t = op::array::memSet(F.Md, allNull, F.n, F.v);
    Expr r = R.rewrite(t);
    CHECK(r == F.Md && R.get("addr.prune-size0-noop") == 1,
          "all-null destination: no-op: " << str(r));
    bool vio = false;
    Expr inb = R.rw.rules().rangeInBounds(allNull, F.n, vio);
    CHECK(!vio && inb == mk<EQ>(F.n, F.num(0)),
          "all-null destination: no static violation, n = 0");
    ExprVector prem = premise(R.rw, t);
    CHECK(F.satisfiable(prem), "all-null destination: premise satisfiable");
    CHECK(F.equivalent(F.ref(t, F.j), F.ref(r, F.j), prem),
          "all-null destination: equal memories under A-INB");
    CHECK(!F.equivalent(F.ref(t, F.j), F.ref(r, F.j), F.layout),
          "all-null destination: not without it");
  }
  // all null with n = 0 (numeral): a no-op
  {
    RuleRun R(F);
    Expr allNull = mk<ITE>(F.c, nx, F.at(F.on, 1));
    Expr t = mcopy(F.Md, F.Ms, allNull, F.s, F.num(0));
    Expr r = R.rewrite(t);
    CHECK(r == F.Md, "all-null destination, n = 0: no-op: " << str(r));
  }
  // all null with a numeral n > 0: a definite violation, not rewritten (the
  // static fallback)
  {
    RuleRun R(F);
    Expr allNull = mk<ITE>(F.c, nx, F.at(F.on, 1));
    Expr t = op::array::memSet(F.Md, allNull, F.num(2), F.v);
    Expr r = R.rewrite(t);
    CHECK(r == t && R.get("addr.prune-size0-noop") == 0 &&
              R.get("addr.prune-size0-range") == 0,
          "all-null destination, n = 2: kept: " << str(r));
  }
  // an mcopy source: mixed -> its live leaf; all null -> no-op
  {
    RuleRun R(F);
    Expr src = mk<ITE>(F.c, nx, bd);
    Expr t = mcopy(F.Md, F.Ms, ax, src, F.n);
    Expr r = R.rewrite(t);
    CHECK(r == mcopy(F.Md, F.Ms, ax, bd, F.n) &&
              R.get("addr.prune-size0-range") == 1,
          "mixed source: pruned: " << str(r));
    ExprVector prem = premise(R.rw, t);
    CHECK(prem.size() == F.layout.size() + 2, "mcopy: two ranges");
    CHECK(F.satisfiable(prem), "mixed source: premise satisfiable");
    CHECK(F.equivalent(F.ref(t, F.j), F.ref(r, F.j), prem),
          "mixed source: equal memories under A-INB");
    CHECK(!F.equivalent(F.ref(t, F.j), F.ref(r, F.j), F.layout),
          "mixed source: not without it");
  }
  {
    RuleRun R(F);
    Expr src = mk<ITE>(F.c, nx, F.at(F.on, 2));
    Expr t = mcopy(F.Md, F.Ms, ax, src, F.n);
    Expr r = R.rewrite(t);
    CHECK(r == F.Md && R.get("addr.prune-size0-noop") == 1,
          "all-null source: no-op: " << str(r));
    ExprVector prem = premise(R.rw, t);
    CHECK(F.satisfiable(prem) &&
              F.equivalent(F.ref(t, F.j), F.ref(r, F.j), prem),
          "all-null source: equal memories under A-INB");
  }
  // a read over a pruned destination keeps its provenance: one leaf pair,
  // decided by cmp.diff-base (no size-0 pair left to look at)
  {
    RuleRun R(F);
    Expr t = mcopy(F.Md, F.Ms, mixed, F.s, F.n);
    Expr r = R.read(t, bd);
    CHECK(R.get("row.mcopy-skip") == 1 && R.get("cmp.diff-base") == 1 &&
              R.get("cmp.size0-vacuous") == 0,
          "read after pruning: skip by diff-base: " << str(r));
    CHECK(F.equivalent(F.ref(t, bd), r, inbPremise(F, R.rw, t, bd)),
          "read after pruning: exact under A-INB");
  }
  // without A-INB, or with the option off: nothing pruned
  {
    SeaMOptions o;
    o.assumeInBounds = false;
    RuleRun R(F, o);
    Expr t = op::array::memSet(F.Md, mixed, F.n, F.v);
    CHECK(R.rewrite(t) == t, "no A-INB: range kept");
    SeaMOptions off;
    off.pruneSize0 = false;
    RuleRun R2(F, off);
    CHECK(R2.rewrite(t) == t, "option off: range kept");
  }
}

/// Width terms (docs/design/SeaM-mixed-width.md rev 3): each rewrite of a
/// wselect / select over a wstore (or a memop term) checked against the byte
/// expansion (WideExpander) at 4-bit addresses, under the A-INB premises of
/// the spans where the rule uses A-INB; residual cases stay terms.
void testWide() {
  Fixture F;
  using op::array::wselect;
  using op::array::wstore;
  Expr bv16 = op::bv::bvsort(16, F.efac), bv32 = op::bv::bvsort(32, F.efac);
  Expr v32 = op::bv::bvConst(mkTerm<std::string>("v32", F.efac), 32);
  // non-uniform bytes, so that byte order matters
  Expr k32 = op::bv::bvnum(mpz_class(0x44332211UL), 32, F.efac);
  auto run = [&](const Expr &t, SeaMOptions o = SeaMOptions()) {
    RuleRun R(F, o);
    return std::make_pair(R.rewrite(t), R.rw.stats());
  };
  auto spanPrem = [&](SeaMRewriter &rw, const Expr &a, uint64_t w) {
    bool vio = false;
    return rw.rules().inBounds(a, w, vio);
  };
  // W-HIT: same address, same width
  {
    Expr t = wselect(wstore(F.Md, F.at(F.oa, 1), v32), F.at(F.oa, 1), bv32);
    auto [r, st] = run(t);
    CHECK(r == v32 && st.get("wide.hit") == 1, "w-hit: " << str(r));
    CHECK(F.equivalent(t, r), "w-hit exact");
  }
  // W-SUB: a 2-byte read at offset 1 of a 4-byte write; a byte read at 2
  {
    Expr t = wselect(wstore(F.Md, F.oa, k32), F.at(F.oa, 1), bv16);
    auto [r, st] = run(t);
    CHECK(st.get("wide.sub") == 1 && F.equivalent(t, r),
          "w-sub 2 bytes at 1: " << str(r));
    Expr t2 = op::array::select(wstore(F.Md, F.oa, k32), F.at(F.oa, 2));
    auto [r2, st2] = run(t2);
    CHECK(st2.get("wide.sub") == 1 && F.equivalent(t2, r2),
          "byte read inside a wide write: " << str(r2));
    // the value is 0x33 (little endian)
    CHECK(F.equivalent(t2, op::bv::bvnum(mpz_class(0x33UL), 8, F.efac)),
          "little endian byte 2 = 0x33");
  }
  // circular: a 2-byte write at offset 15 covers offsets 15 and 0 (mod 16);
  // a byte read at offset 0 is its second byte
  {
    Expr t = op::array::select(
        wstore(F.Md, F.at(F.oa, 15),
               op::bv::bvnum(mpz_class(0x2211UL), 16, F.efac)),
        F.oa);
    auto [r, st] = run(t);
    CHECK(st.get("wide.sub") == 1 && F.equivalent(t, r),
          "wrap: offset 0 inside a write at 15: " << str(r));
    CHECK(F.equivalent(t, op::bv::bvnum(mpz_class(0x22UL), 8, F.efac)),
          "wrap: the second byte");
  }
  // W-SKIP, one base, numerals: disjoint spans [0, 4) and [4, 6)
  {
    Expr t = wselect(wstore(F.Md, F.oa, v32), F.at(F.oa, 4), bv16);
    auto [r, st] = run(t);
    CHECK(r == wselect(F.Md, F.at(F.oa, 4), bv16) && st.get("wide.skip") == 1 &&
              st.get("premise.a-inb") == 0,
          "w-skip numerals, no premise: " << str(r));
    CHECK(F.equivalent(t, r), "w-skip numerals exact");
  }
  // partial overlap: [0, 4) and [2, 6): the byte fallback (the read split
  // into byte reads, each decided by the byte rules), exact
  {
    Expr t = wselect(wstore(F.Md, F.oa, v32), F.at(F.oa, 2), bv32);
    auto [r, st] = run(t);
    CHECK(st.get("wide.split-read") == 1 && countOp<W_SELECT>(r) == 0 &&
              F.equivalent(t, r),
          "partial overlap: byte fallback: " << str(r));
  }
  // across objects: skip under A-INB (whole spans), valid with the span
  // premises, not without them; not without A-INB
  {
    Expr wa = F.add(F.oa, F.x), rb = F.add(F.ob, F.k);
    Expr t = wselect(wstore(F.Md, wa, v32), rb, bv16);
    RuleRun R(F);
    Expr r = R.rewrite(t);
    CHECK(r == wselect(F.Md, rb, bv16) && R.get("wide.skip") == 1 &&
              R.get("premise.a-inb") == 1,
          "w-skip across objects: " << str(r));
    ExprVector prem = F.layout;
    prem.push_back(spanPrem(R.rw, wa, 4));
    prem.push_back(spanPrem(R.rw, rb, 2));
    CHECK(F.satisfiable(prem), "span premises satisfiable");
    CHECK(F.equivalent(t, r, prem), "w-skip across objects under A-INB");
    // the writer's span premise is needed: the reader's alone is not enough
    // (the fixture's objects are too far apart for a narrowed writer span to
    // reach b: the reader's-width mutant is for a fixture with adjacent
    // objects)
    ExprVector weak = F.layout;
    weak.push_back(spanPrem(R.rw, rb, 2));
    CHECK(!F.equivalent(t, r, weak), "the writer's span premise is needed");
    SeaMOptions o;
    o.assumeInBounds = false;
    auto [r2, st2] = run(t, o);
    CHECK(st2.get("wide.skip") == 0 && st2.get("wide.split-read") == 1 &&
              F.equivalent(t, r2),
          "no A-INB: no skip, the byte fallback, exact");
  }
  // over a memop term: a 2-byte read inside an mcopy range is the source
  // version at s + (j - d); a read of another object skips (A-INB)
  {
    Expr t =
        wselect(mcopy(F.Md, F.Ms, F.oa, F.ob, F.num(4)), F.at(F.oa, 1), bv16);
    auto [r, st] = run(t);
    CHECK(st.get("wide.memop-hit") == 1 && F.equivalent(t, r),
          "w-read inside an mcopy: " << str(r));
    Expr t0 = wselect(op::array::memSet(F.Md, F.oa, F.num(0), F.v), F.oa, bv16);
    auto [r0, st0] = run(t0);
    CHECK(r0 == wselect(F.Md, F.oa, bv16) && F.equivalent(t0, r0),
          "zero-length range: no change: " << str(r0));
    Expr ts =
        wselect(op::array::memSet(F.Md, F.oa, F.n, F.v), F.at(F.oa, 1), bv16);
    auto [rs, sts] = run(ts);
    CHECK(sts.get("wide.memop-hit") == 0 && sts.get("wide.split-read") == 1 &&
              F.equivalent(ts, rs),
          "symbolic length, same object: the byte fallback, exact");
  }
}

/// Width terms: cases for negative-cache lifetime of WideExpander, an
/// adjacent-object skip whose writer premise must be the writer's own width, a
/// full-circle write (width 2^n) and a two-piece circular overlap (both fall
/// back, exact).
void testWideReview() {
  // WideExpander pins its cache keys: a released non-wide node whose address
  // is reused by a wselect must not answer "no width term"
  {
    ExprFactory ef;
    Expr index = op::bv::bvsort(kW, ef), byte = op::bv::bvsort(8, ef);
    Expr sort16 = op::bv::bvsort(16, ef);
    Expr m = op::bind::mkConst(mkTerm<std::string>("M", ef),
                               sort::arrayTy(index, byte));
    Expr a = op::bv::bvnum(mpz_class(0UL), kW, ef);
    Expr v = op::bv::bvnum(mpz_class(1UL), 16, ef);
    Expr c = op::bind::boolConst(mkTerm<std::string>("c", ef));
    WideExpander wx;
    {
      Expr temp = mk<ITE>(c, v, mk<BADD>(v, v));
      CHECK(wx.expand(temp) == temp, "a term without width terms unchanged");
    }
    Expr wide = op::array::wselect(m, a, sort16);
    CHECK(wx.hasWide(wide), "a wselect is seen after a node was released");
    CHECK(!isOpX<W_SELECT>(wx.expand(wide)), "and expanded");
  }
  Fixture F;
  using op::array::wselect;
  using op::array::wstore;
  // an object c adjacent to a (a: [2, 6), c: [6, 8))
  Expr oc = F.ptr("obj.c");
  F.reg.add(oc, "c", "heap", llvm::APInt(kW, 6), llvm::APInt(kW, 2), 1);
  F.reg.addAlias(F.var("obj.c"), oc);
  F.layout.push_back(mk<EQ>(mk<ADDR>(oc), F.num(6)));
  Expr bv32 = op::bv::bvsort(32, F.efac), bv16 = op::bv::bvsort(16, F.efac);
  Expr v32 = op::bv::bvConst(mkTerm<std::string>("w32", F.efac), 32);
  {
    // a 4-byte write at a + x, a 1-byte read at c + d: skip under A-INB
    Expr wa = F.add(F.oa, F.x), rc = F.add(oc, F.k);
    Expr t = op::array::select(wstore(F.Md, wa, v32), rc);
    RuleRun R(F);
    Expr r = R.rewrite(t);
    CHECK(r == op::array::select(F.Md, rc) && R.get("wide.skip") == 1,
          "adjacent objects: skip: " << str(r));
    bool vio = false;
    ExprVector prem = F.layout;
    prem.push_back(R.rw.rules().inBounds(wa, 4, vio));
    prem.push_back(R.rw.rules().inBounds(rc, 1, vio));
    CHECK(F.satisfiable(prem) && F.equivalent(t, r, prem),
          "adjacent objects: valid with the writer's own 4-byte span");
    ExprVector mutant = F.layout; // the reader's width for the writer
    mutant.push_back(R.rw.rules().inBounds(wa, 1, vio));
    mutant.push_back(R.rw.rules().inBounds(rc, 1, vio));
    CHECK(!F.equivalent(t, r, mutant),
          "adjacent objects: the reader's width for the writer is wrong");
  }
  {
    // full circle: a 16-byte write at a covers every address; a 4-byte read
    // at a + 14 wraps (bytes 14, 15, 0, 1 of the value): not W-SUB (k + r >
    // w), the fallback; exact
    Expr v128 = op::bv::bvConst(mkTerm<std::string>("w128", F.efac), 128);
    Expr t = wselect(wstore(F.Md, F.oa, v128), F.at(F.oa, 14), bv32);
    RuleRun R(F);
    Expr r = R.rewrite(t);
    // the wide read is split (not one W-SUB); its byte reads are W-SUBs
    CHECK(R.get("wide.split-read") == 1 && F.equivalent(t, r),
          "full circle: fallback, exact: " << str(r));
    // two pieces: a 12-byte write at 0, a 12-byte read at 8 meets it in
    // [8, 12) and in [0, 4) (wrapping): fallback, exact
    Expr v96 = op::bv::bvConst(mkTerm<std::string>("w96", F.efac), 96);
    Expr bv96 = op::bv::bvsort(96, F.efac);
    Expr t2 = wselect(wstore(F.Md, F.oa, v96), F.at(F.oa, 8), bv96);
    RuleRun R2(F);
    Expr r2 = R2.rewrite(t2);
    CHECK(R2.get("wide.split-read") == 1 && R2.get("wide.hit") == 0 &&
              F.equivalent(t2, r2),
          "two-piece circular overlap: fallback, exact");
  }
}

/// W-ROW-CONGRUENT (SeaM-mixed-width.md rev 5): equal power-of-two widths,
/// starts congruent mod w: ite(i = j, v, below), exact without premises.
void testWideCongruent() {
  Fixture F;
  using op::array::wselect;
  using op::array::wstore;
  Expr bv32 = op::bv::bvsort(32, F.efac);
  Expr v32 = op::bv::bvConst(mkTerm<std::string>("c32", F.efac), 32);
  Expr four = F.num(4);
  Expr i = F.add(F.oa, mk<BMUL>(four, F.x)),
       j = F.add(F.oa, mk<BMUL>(four, F.k));
  {
    Expr t = wselect(wstore(F.Md, i, v32), j, bv32);
    SeaMOptions o;
    o.assumeInBounds = false; // exact: no assumption needed
    RuleRun R(F, o);
    Expr r = R.rewrite(t);
    CHECK(R.get("wide.row-congruent") == 1 && R.get("wide.split-read") == 0 &&
              R.get("premise.a-inb") == 0,
          "congruent starts: one word compare: " << str(r));
    CHECK(F.equivalent(t, r), "congruent: exact without premises");
  }
  {
    // starts 2 apart with width 4: not congruent mod 4: the fallback
    Expr j2 = F.add(F.add(F.oa, mk<BMUL>(four, F.k)), F.num(2));
    Expr t = wselect(wstore(F.Md, i, v32), j2, bv32);
    RuleRun R(F);
    Expr r = R.rewrite(t);
    CHECK(R.get("wide.row-congruent") == 0 && R.get("wide.split-read") == 1 &&
              F.equivalent(t, r),
          "not congruent: fallback, exact");
  }
  {
    // two loads through a chain of congruent stores stay words
    Expr k = F.add(F.oa, mk<BMUL>(four, F.y4()));
    Expr m2 = wstore(wstore(F.Md, i, v32), k, v32);
    Expr t = wselect(m2, j, bv32);
    RuleRun R(F);
    Expr r = R.rewrite(t);
    CHECK(R.get("wide.row-congruent") == 2 && R.get("wide.split-read") == 0 &&
              F.equivalent(t, r),
          "a chain of congruent stores: words all the way: " << str(r));
  }
}

/// W-ROW-ALIGNED (SeaM-mixed-width.md rev 5): equal power-of-two widths,
/// both accesses annotated w-aligned (`align`: trusted front-end facts):
/// ite(i = j, v, below); valid given the alignment facts, not without; no
/// rule without both annotations.
void testWideAligned() {
  Fixture F;
  using op::array::wselect;
  using op::array::wstore;
  Expr bv32 = op::bv::bvsort(32, F.efac);
  Expr v32 = op::bv::bvConst(mkTerm<std::string>("a32", F.efac), 32);
  Expr i = F.add(F.oa, F.x), j = F.add(F.ob, F.k); // no linear relation
  // (different objects would skip under A-INB: turn A-INB off to isolate)
  SeaMOptions o;
  o.assumeInBounds = false;
  {
    Expr t = wselect(wstore(F.Md, i, v32, 4), j, bv32, 4);
    RuleRun R(F, o);
    Expr r = R.rewrite(t);
    CHECK(R.get("wide.row-aligned") == 1 && R.get("premise.align") == 1 &&
              R.get("wide.split-read") == 0,
          "aligned: one word compare: " << str(r));
    auto aligned = [&](const Expr &a) {
      return mk<EQ>(mk<BAND>(a, F.num(3)), F.num(0));
    };
    ExprVector facts{aligned(i), aligned(j)};
    CHECK(F.satisfiable(facts) && F.equivalent(t, r, facts),
          "aligned: valid given the alignment facts");
    CHECK(!F.equivalent(t, r), "aligned: not valid without them");
  }
  {
    // one access without the annotation: no aligned rule (the fallback)
    Expr t = wselect(wstore(F.Md, i, v32, 4), j, bv32, 1);
    RuleRun R(F, o);
    Expr r = R.rewrite(t);
    CHECK(R.get("wide.row-aligned") == 0 && R.get("wide.split-read") == 1 &&
              F.equivalent(t, r),
          "one annotation missing: fallback, exact");
  }
  {
    // the annotation does not transfer to a changed address: an mcopy
    // source read (rev 5's counterexample shape) has alignment 1
    Expr t = wselect(mcopy(F.Md, F.Ms, F.at(F.oa, 0), F.at(F.ob, 1), F.num(4)),
                     F.at(F.oa, 0), bv32, 4);
    RuleRun R(F);
    Expr r = R.rewrite(t);
    CHECK(R.get("wide.memop-hit") == 1 && F.equivalent(t, r),
          "mcopy hit: exact");
    std::ostringstream os;
    os << *r;
    CHECK(os.str().find("wselect") == std::string::npos ||
              R.get("wide.row-aligned") == 0,
          "the translated source read carries no promise");
  }
}

/// W-BYTE-IN: a byte read at a symbolic j over a w-byte write at a symbolic
/// i (no relation, no premise): ite((j - i) <u w, byte of v, below): exact
/// over 4-bit addresses (the write may wrap), and the 8-byte case.
void testWideByteIn() {
  Fixture F;
  using op::array::wstore;
  Expr v32 = op::bv::bvConst(mkTerm<std::string>("b32", F.efac), 32);
  Expr v64 = op::bv::bvConst(mkTerm<std::string>("b64", F.efac), 64);
  SeaMOptions o;
  o.assumeInBounds = false;
  // one object at symbolic offsets (where the byte rules decide nothing)
  for (Expr v : {v32, v64}) {
    Expr t =
        op::array::select(wstore(F.Md, F.add(F.oa, F.x), v), F.add(F.oa, F.k));
    RuleRun R(F, o);
    Expr r = R.rewrite(t);
    CHECK(R.get("wide.byte-in") == 1 && R.get("wide.expand-write") == 0,
          "byte-in fires: " << str(r));
    CHECK(F.equivalent(t, r), "byte-in exact (wrap included)");
  }
  SeaMOptions off = o;
  off.wideByteIn = false;
  Expr t =
      op::array::select(wstore(F.Md, F.add(F.oa, F.x), v32), F.add(F.oa, F.k));
  RuleRun R(F, off);
  Expr r = R.rewrite(t);
  CHECK(R.get("wide.expand-write") == 1 && F.equivalent(t, r),
        "off: the write is expanded, exact");
}

/// Store-map interaction: stores at numeral offsets of one object around a
/// term (store-map over a term, a term over a store-map), as SeaOpSem builds
/// them (a rewrite per store), then reads; each read checked against the
/// expansion under A-INB.
void testStoreMap() {
  Fixture F;
  RuleRun R(F);
  auto store = [&](const Expr &m, const Expr &p, const Expr &v) {
    return R.rewrite(op::array::store(m, R.rewrite(p), v));
  };
  Expr y2 = op::bv::bvConst(mkTerm<std::string>("y2", F.efac), kV);
  Expr m1 = store(store(F.Md, F.at(F.ob, 0), F.y), F.at(F.ob, 1), y2);
  CHECK(countOp<STORE_MAP>(m1) == 1, "a store-map below");
  Expr t = mcopy(m1, m1, F.at(F.ob, 2), F.at(F.ob, 0), F.n);
  Expr m2 = store(store(t, F.at(F.ob, 4), F.v), F.at(F.ob, 5), F.v);
  CHECK(countOp<STORE_MAP>(m2) == 2, "a store-map above the term");
  Expr refm = op::array::store(
      op::array::store(
          mcopy(op::array::store(op::array::store(F.Md, F.at(F.ob, 0), F.y),
                                 F.at(F.ob, 1), y2),
                op::array::store(op::array::store(F.Md, F.at(F.ob, 0), F.y),
                                 F.at(F.ob, 1), y2),
                F.at(F.ob, 2), F.at(F.ob, 0), F.n),
          F.at(F.ob, 4), F.v),
      F.at(F.ob, 5), F.v);
  for (unsigned long k = 0; k < 6; ++k) {
    Expr p = F.at(F.ob, k);
    Expr r = R.read(m2, p);
    ExprVector prem = F.layout;
    bool vio = false;
    prem.push_back(R.rw.rules().rangeInBounds(F.at(F.ob, 2), F.n, vio));
    CHECK(F.equivalent(F.ref(refm, p), r, prem),
          "store-map read at b + " << k << ": " << str(r));
  }
  // a read of another object skips everything (store-map R-SKIP, mcopy skip)
  Expr ra = R.read(m2, F.add(F.oa, F.x));
  CHECK(ra == F.sel(F.Md, F.add(F.oa, F.x)), "read of a skips: " << str(ra));
}

/// The boundary without the rules, and its refusal.
void testBoundary() {
  Fixture F;
  Expr t = mcopy(F.Md, F.Ms, F.at(F.oa, 0), F.at(F.ob, 0), F.n);
  // expandUnknownRow off: an undecided read stays a select over the term
  {
    SeaMOptions o;
    o.expandUnknownRow = false;
    RuleRun R(F, o);
    Expr r = R.read(t, F.add(F.oa, F.x));
    CHECK(isOpX<SELECT>(r) && isMemOpTerm(r->arg(0)), "read kept: " << str(r));
    std::string why;
    R.rw.noteMemOps();
    Expr b = R.rw.solverTerm(r, why);
    CHECK(b && countOp<MEM_COPY>(b) == 0, "boundary translates the read");
    CHECK(F.equivalent(F.ref(t, F.add(F.oa, F.x)), b), "row-expand=0 exact");
  }
  // a rewrite stopped by its budget: the boundary still translates
  {
    RewriteOptions ro;
    ro.maxRuleCalls = 1;
    RuleRun R(F, SeaMOptions(), ro);
    Expr m = op::array::memSet(t, F.d, F.n, F.v);
    Expr r = R.read(m, F.add(F.oa, F.x));
    std::string why;
    R.rw.noteMemOps();
    Expr b = R.rw.solverTerm(r, why);
    CHECK(b && countOp<MEM_COPY>(b) + countOp<MEM_SET>(b) == 0,
          "boundary after an abort");
    CHECK(F.equivalent(F.ref(m, F.add(F.oa, F.x)), b), "aborted rewrite exact");
  }
  // a term not under a read: refused, with a reason
  {
    MemOpTranslator tr;
    std::string why;
    Expr eqm = mk<EQ>(t, F.Md);
    CHECK(!tr.translate(eqm, why) && !why.empty(),
          "memory equality refused: " << why);
    why.clear();
    CHECK(!tr.translate(op::array::store(t, F.x, F.y), why),
          "memory root refused");
    // a term-free memory equality is left alone
    CHECK(tr.translate(mk<EQ>(F.Ms, F.Md), why) == mk<EQ>(F.Ms, F.Md),
          "no term: unchanged");
  }
}

/// PTR_INDEX (docs/design/PtrIndex.md rev 5): construction, identity,
/// typing, and the expansion into bit-vector arithmetic (proved equal by
/// Yices for every value of the base and the indices, with negative
/// strides, constant and K).
void testPtrIndex() {
  using namespace op;
  ExprFactory efac;
  Expr s64 = bv::bvsort(64, efac);
  Expr b = bind::mkConst(mkTerm<std::string>("pi.b", efac), s64);
  Expr x = bind::mkConst(mkTerm<std::string>("pi.x", efac), s64);
  Expr y = bind::mkConst(mkTerm<std::string>("pi.y", efac), s64);
  auto num = [&](long v) {
    mpz_class m(v), mod;
    mpz_ui_pow_ui(mod.get_mpz_t(), 2, 64);
    mpz_fdiv_r(m.get_mpz_t(), m.get_mpz_t(), mod.get_mpz_t());
    return bv::bvnum(m, 64, efac);
  };
  std::vector<std::pair<Expr, mpz_class>> I{{x, mpz_class(24L)},
                                            {y, mpz_class(-4L)}};
  Expr nusw = ptr::ptrIndex(PtrOpKind::PTR_INDEX_NUSW, b, I, mpz_class(-16L),
                            mpz_class(0L));
  Expr plain =
      ptr::ptrIndex(PtrOpKind::PTR_INDEX, b, I, mpz_class(-16L), mpz_class(0L));
  Expr k8 = ptr::ptrIndex(PtrOpKind::PTR_INDEX_NUSW, b, I, mpz_class(-16L),
                          mpz_class(8L));
  CHECK(nusw != plain, "flags are part of the identity");
  CHECK(nusw == ptr::ptrIndex(PtrOpKind::PTR_INDEX_NUSW, b, I, mpz_class(-16L),
                              mpz_class(0L)),
        "hash-consed");
  CHECK(k8 != nusw && k8 != mk<BADD>(nusw, num(8)), "K is in the node");
  CHECK(ptr::isFlagged(nusw.get()) && !ptr::isFlagged(plain.get()),
        "isFlagged");
  CHECK(ptr::numIndices(nusw) == 2 && ptr::index(nusw, 1) == y &&
            ptr::stride(nusw, 1) == mpz_class(-4L) &&
            ptr::constant(nusw) == mpz_class(-16L) &&
            ptr::byteOffset(k8) == mpz_class(8L),
        "accessors");
  TypeChecker tc;
  // S2e: the base must be ptr(w); a node over bv(w) is ill-typed, one over
  // a pointer constant has its sort
  CHECK(tc.typeOf(nusw) == sort::errorTy(efac), "a bv base is ill-typed");
  {
    Expr pb = ptr::ptrConst(mkTerm<std::string>("pb", efac), 64);
    Expr pn = ptr::ptrIndex(PtrOpKind::PTR_INDEX_NUSW, pb, I, mpz_class(-16L),
                            mpz_class(0L));
    CHECK(tc.typeOf(pn) == ptr::ptrsort(64, efac), "the sort of the base");
    CHECK(tc.typeOf(mk<ADDR>(pn)) == s64, "addr: ptr(w) -> bv(w)");
    CHECK(tc.typeOf(mk<PTR_FROM_INT>(x)) == ptr::ptrsort(64, efac),
          "from_int: bv(w) -> ptr(w)");
    CHECK(tc.typeOf(mk<PTR_DIFF>(pn, pb)) == s64, "diff: ptr x ptr -> bv");
    CHECK(tc.typeOf(mk<PTR_EQ>(pn, pb)) == sort::boolTy(efac), "ptr-eq");
    CHECK(tc.typeOf(mk<EQ>(pn, pb)) == sort::errorTy(efac), "= refuses ptr(w)");
    CHECK(tc.typeOf(mk<BADD>(pb, x)) == sort::errorTy(efac),
          "bvadd refuses ptr(w)");
    PtrIndexExpander x0;
    Expr pe = x0.expand(mk<PTR_EQ>(pn, pb));
    CHECK(!x0.has(pe) && tc.typeOf(pe) == sort::boolTy(efac),
          "erased: no ptr sort left, well typed");
  }
  PtrIndexExpander ex;
  Expr wrapped = mk<EQ>(k8, b);
  CHECK(ex.has(wrapped) && !ex.has(mk<EQ>(x, b)), "has()");
  Expr e = ex.expand(wrapped);
  CHECK(!ex.has(e), "no node left after the expansion");
  Expr ref = mk<BADD>(
      mk<BADD>(mk<BADD>(b, mk<BMUL>(x, num(24))), mk<BMUL>(y, num(-4))),
      num(-16 + 8));
  sea::solver::yices_solver_impl sv(efac);
  sv.add(erased(mk<NEG>(mk<EQ>(e, mk<EQ>(ref, b)))));
  CHECK(sv.check() == sea::solver::SolverResult::UNSAT,
        "expansion = b + 24x - 4y - 16 + 8 for every b, x, y");
  // a constant-free, K-free node expands without the constant
  Expr one = ptr::ptrIndex(PtrOpKind::PTR_INDEX, b, {{x, mpz_class(8L)}},
                           mpz_class(0L), mpz_class(0L));
  CHECK(ex.expand(one) == mk<BADD>(b, mk<BMUL>(x, num(8))),
        "b + 8x exactly"); // a constant-only node (rev 6: m = 0 legal): typed,
                           // expanded to b + c + K
  Expr c0 =
      ptr::ptrIndex(PtrOpKind::PTR_INDEX, b, {}, mpz_class(-8L), mpz_class(2L));
  CHECK(ptr::numIndices(c0) == 0, "constant-only node");
  CHECK(ex.expand(c0) == mk<BADD>(b, num(-6)), "b + (-8 + 2)");
}

/// op::ptr::byteAddress (N70): byte k of an access at an ite address with k
/// pushed into the leaves, ite(c, A + 24, A) + k ~> ite(c, A + 24 + k, A + k),
/// exact, shared subterms rebuilt once, refused over the leaf cap; the
/// boundary's byte expansion (WideExpander) uses it when set.
void testIteOffset() {
  using namespace op;
  ExprFactory efac;
  const unsigned n = 64;
  Expr A = ptr::ptrConst(mkTerm<std::string>("A", efac), n);
  Expr B = ptr::ptrConst(mkTerm<std::string>("B", efac), n);
  Expr c = bind::boolConst(mkTerm<std::string>("c", efac));
  Expr c2 = bind::boolConst(mkTerm<std::string>("c2", efac));
  auto num = [&](unsigned long v) { return bv::bvnum(mpz_class(v), n, efac); };
  auto valid = [&](const Expr &a, const Expr &b) {
    TypeChecker tc;
    PtrIndexExpander px;
    Expr ea = mk<ADDR>(a), eb = mk<ADDR>(b);
    sea::solver::yices_solver_impl sv(efac);
    sv.add(px.expand(mk<NEG>(mk<EQ>(ea, eb))));
    return sv.check() == sea::solver::SolverResult::UNSAT;
  };
  // the leaves of the result are not ites, and no node has an ite base
  std::function<bool(const Expr &)> pushedForm = [&](const Expr &e) {
    if (isOpX<ITE>(e))
      return pushedForm(e->arg(1)) && pushedForm(e->arg(2));
    return !(ptr::isPtrIndex(e) && isOpX<ITE>(ptr::base(e)));
  };
  Expr A24 = ptr::addressPlus(A, num(24));
  Expr q = mk<ITE>(c, A24, A);
  ptr::OffsetPush how;
  {
    Expr r = ptr::byteAddress(q, num(3), 8, how);
    CHECK(how == ptr::OffsetPush::Pushed && isOpX<ITE>(r) && r->arg(0) == c &&
              r->arg(1) == ptr::addressPlus(A, num(27)) &&
              r->arg(2) == ptr::addressPlus(A, num(3)),
          "ite(c, A + 24, A) + 3 = ite(c, A + 27, A + 3): " << *r);
    CHECK(valid(r, ptr::addressPlus(q, num(3))), "pushed: exact");
    // the old form: a node over the ite
    Expr old = ptr::byteAddress(q, num(3), 0, how);
    CHECK(how == ptr::OffsetPush::None && old == ptr::addressPlus(q, num(3)) &&
              !pushedForm(old),
          "cap 0: a + k over the ite");
  }
  {
    // wrap: k pushed mod 2^n into a leaf at the top of the space
    Expr top = ptr::addressPlus(A, num(~0UL));
    Expr r = ptr::byteAddress(mk<ITE>(c, top, B), num(2), 8, how);
    CHECK(how == ptr::OffsetPush::Pushed && pushedForm(r) &&
              valid(r, ptr::addressPlus(mk<ITE>(c, top, B), num(2))),
          "wrap: exact mod 2^n");
  }
  {
    // a shared subterm: ite(c, X, ite(c2, X, B)), X = ite(c2, A24, A): X is
    // rebuilt once (hash-consed: the same node at both places)
    Expr X = mk<ITE>(c2, A24, A);
    Expr d = mk<ITE>(c, X, mk<ITE>(c2, X, B));
    Expr r = ptr::byteAddress(d, num(5), 8, how);
    CHECK(how == ptr::OffsetPush::Pushed && pushedForm(r) &&
              r->arg(1) == r->arg(2)->arg(1) &&
              valid(r, ptr::addressPlus(d, num(5))),
          "shared subterm pushed once, exact");
  }
  {
    // over the cap: 9 distinct leaves -> refused, a + k over the ite
    Expr big = B;
    for (unsigned long i = 0; i < 8; ++i)
      big = mk<ITE>(
          bind::boolConst(mkTerm<std::string>("b" + std::to_string(i), efac)),
          ptr::addressPlus(A, num(8 * i)), big);
    Expr r = ptr::byteAddress(big, num(1), 8, how);
    CHECK(how == ptr::OffsetPush::Refused && r == ptr::addressPlus(big, num(1)),
          "9 leaves over a cap of 8: refused");
    ptr::byteAddress(big, num(1), 9, how);
    CHECK(how == ptr::OffsetPush::Pushed, "9 leaves within a cap of 9");
  }
  {
    // an ite of promised nodes: addressPlus's own push (K + k), not counted
    Expr x = bv::bvConst(mkTerm<std::string>("x", efac), n);
    Expr p1 = ptr::ptrIndex(PtrOpKind::PTR_INDEX_NUSW, A, {{x, mpz_class(8L)}},
                            mpz_class(0L), mpz_class(0L));
    Expr p2 = ptr::ptrIndex(PtrOpKind::PTR_INDEX_NUSW, B, {{x, mpz_class(8L)}},
                            mpz_class(0L), mpz_class(0L));
    Expr r = ptr::byteAddress(mk<ITE>(c, p1, p2), num(4), 8, how);
    CHECK(how == ptr::OffsetPush::None &&
              r == ptr::addressPlus(mk<ITE>(c, p1, p2), num(4)),
          "promised leaves: addressPlus's K push");
  }
  {
    // the boundary: a 4-byte read at ite(c, A + 24, A), bytes in the leaves
    Expr byte = bv::bvsort(8, efac);
    Expr M = bind::mkConst(mkTerm<std::string>("M", efac),
                           sort::arrayTy(ptr::ptrsort(n, efac), byte));
    Expr w = array::wselect(M, q, bv::bvsort(32, efac));
    WideExpander on, off;
    on.maxOffsetLeaves(8);
    Expr ron = on.expand(w), roff = off.expand(w);
    bool leaves = true;
    std::function<void(const Expr &)> walk = [&](const Expr &e) {
      if (isOpX<SELECT>(e))
        leaves = leaves && pushedForm(e->arg(1));
      for (size_t i = 0; i < e->arity(); ++i)
        walk(Expr(e->arg(i)));
    };
    walk(ron);
    CHECK(leaves && on.offsetPushed() == 3 && off.offsetPushed() == 0,
          "boundary: bytes 1..3 at the leaves: " << *ron);
    PtrIndexExpander px;
    sea::solver::yices_solver_impl sv(efac);
    sv.add(px.expand(mk<NEG>(mk<EQ>(ron, roff))));
    CHECK(sv.check() == sea::solver::SolverResult::UNSAT,
          "boundary: the same bytes as a + k");
  }
}

} // namespace

int main(int argc, char **argv) {
  struct Test {
    const char *name;
    void (*fn)();
  } tests[] = {{"read-equations", testReadEquations},
               {"havoc-one-array", testHavocOneArray},
               {"rules", testRules},
               {"null", testNull},
               {"prune-size0", testPruneSize0},
               {"wide", testWide},
               {"wide-review", testWideReview},
               {"wide-congruent", testWideCongruent},
               {"wide-aligned", testWideAligned},
               {"wide-byte-in", testWideByteIn},
               {"store-map", testStoreMap},
               {"boundary", testBoundary},
               {"ptr-index", testPtrIndex},
               {"ite-offset", testIteOffset}};
  for (const Test &t : tests) {
    int before = failures;
    auto t0 = std::chrono::steady_clock::now();
    t.fn();
    llvm::outs() << "test: " << t.name << " "
                 << (failures == before ? "ok" : "FAILED") << " ("
                 << std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count()
                 << " ms)\n";
  }
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
