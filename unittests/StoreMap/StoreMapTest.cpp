// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- StoreMapTest.cpp - tests of Store-Map (thesis Ch. 6, 7.2) ----------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if a
// check fails. Run by test/Expr/store-map.test.
//
// What is tested (include/sea/Expr/StoreMap.hh, SeaMRules.hh `wow`,
// `rowStoreMap`; ported ideas of units_store_map.cpp of the SeaHorn branch):
//  - the write rules of Fig. 6.4, each with the term it makes, its counter and
//    soundness: SMAP-NEW, SMAP-HIT (and the collapse of an identical address),
//    WOW-COMMUTE and WOSMAP-COMMUTE (by allocation order, A-INB only: with
//    assumeInBounds off no commute and the result holds with no assumption),
//    the barriers (no pointer view, symbolic offset, size-0 object);
//  - the read rules of Fig. 6.5: R-HIT, R-MISS, R-SKIP, R-ABORT (symbolic
//    offset, no view, A-INB off), and expandUnknownRow off;
//  - the ordered map side table: moved on insert (one lookup, no scan), rebuilt
//    on a fork, scanned for a version that lost it, and the same answers
//    with and without the table;
//  - expandStoreMaps (what the solver gets): stores oldest first, the latest
//    value of an offset wins, nested store-maps, deep lists and chains;
//  - a seeded random test (stores over three objects, joins, then reads):
//    the memory built the way SeaOpSem builds it (a rewrite per store, one
//    rewriter), every read sound against the unrewritten chain (Yices), no
//    select over a store-map left, a normal form;
//  - the R-ABORT caching test (the shape of thesis Ch. 7.1's
//    Discussion): store-maps read at a symbolic address through ite joins
//    (Fig. 7.1 with store-maps): rule applications <= c N at N = 40, and
//    exponential with chain-start caching switched off.
//
// Soundness is checked with Yices 2 on `assumptions => in = out`, the terms
// expanded (Yices does not know store-map); the assumptions are the layout and
// the bounds of the symbolic offsets (A-INB as the VC states it).
//
//===----------------------------------------------------------------------===//

// boost hash templates are only forward-declared by Expr.hh; define them here
// (a Release build does not export the instantiations from libSeaExpr).
#include "sea/Expr/StoreMap.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/SeaMRules.hh"
#include "sea/Expr/Smt/Yices2SolverImpl.hh"
#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace expr;

namespace {

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

/// Distinct nodes of a term with operator Op.
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
  Expr bvSort, ptrSort, memSort;
  Expr o1, o2, o3; // allocation order o1 < o2 < o3
  Expr p, q;       // pointers that are no object's base
  Expr o0;         // size 0, o3's base
  Expr x, y, z;
  Expr a, b, c;
  Expr m, m2;
  ExprVector layout; // obj.k = base, x y < size (A-INB, as the VC states it)

  Fixture()
      : bvSort(op::bv::bvsort(kWidth, efac)),
        ptrSort(op::ptr::ptrsort(kWidth, efac)),
        memSort(sort::arrayTy(ptrSort, bvSort)) {
    auto obj = [&](const char *name, unsigned long base,
                   unsigned long size = kSize) {
      // a pointer constant, as OpSem's segment (PtrSort S2e)
      Expr e = op::ptr::ptrConst(mkTerm<std::string>(name, efac), kWidth);
      reg.add(e, name, "heap", llvm::APInt(kWidth, base),
              llvm::APInt(kWidth, size), 4);
      reg.addAlias(op::bv::bvConst(mkTerm<std::string>(name, efac), kWidth), e);
      layout.push_back(mk<EQ>(mk<ADDR>(e), num(base)));
      return e;
    };
    o1 = obj("obj1", 0x1000);
    o2 = obj("obj2", 0x2000);
    o3 = obj("obj3", 0x3000);
    o0 = obj("obj0", 0x3000, 0);
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
  Expr cond(const std::string &n) {
    return op::bind::boolConst(mkTerm<std::string>(n, efac));
  }
  /// base + numeral (the normal form of a pointer), the base itself for 0
  Expr at(const Expr &base, unsigned long off) {
    return off == 0 ? base : add2(base, num(off));
  }
  /// p + q: over a pointer a node (op::ptr::addressPlus, as OpSem's
  /// addr.add), else a bvadd
  Expr add2(const Expr &p, const Expr &q) {
    if (isPtr(p))
      return op::ptr::addressPlus(p, q);
    ExprVector v{p, q};
    return mknary<BADD>(v.begin(), v.end());
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
  Expr sel(const Expr &mm, const Expr &i) { return op::array::select(mm, i); }
  Expr st(const Expr &mm, const Expr &i, const Expr &v) {
    return op::array::store(mm, i, v);
  }

  static SeaMOptions opts(bool inb = true, bool smap = true) {
    SeaMOptions o;
    o.assumeInBounds = inb;
    o.storeMap = smap;
    return o;
  }

  /// The stores of a VC: one rewriter, every store rewritten as it is made
  /// (SeaOpSem::memStore), and the raw chain of the same stores beside it.
  struct Enc {
    Fixture &F;
    SeaMRewriter rw;
    Expr mem, raw;
    Enc(Fixture &F, SeaMOptions o, Expr m0, RewriteOptions ro = {})
        : F(F), rw(F.efac, F.reg, o, ro), mem(m0), raw(m0) {}
    Expr store(const Expr &p, const Expr &v) {
      CHECK(wellTyped(F.st(raw, p, v)), "ill-typed store " << str(p));
      raw = F.st(raw, p, v);
      mem = rw.rewrite(F.st(mem, rw.rewrite(p), v));
      return mem;
    }
    Expr load(const Expr &p) { return rw.rewrite(F.sel(mem, rw.rewrite(p))); }
    Expr rawLoad(const Expr &p) { return F.sel(raw, p); }
  };

  /// `assumptions => lhs = rhs` is valid, store-maps expanded
  bool equivalent(const Expr &lhs, const Expr &rhs, bool withLayout) {
    StoreMapExpander ex;
    sea::solver::yices_solver_impl s(efac);
    PtrIndexExpander px;
    if (withLayout)
      for (const Expr &l : layout)
        s.add(px.expand(l));
    // rule-made addresses are PTR_INDEX nodes (PtrSort rev 5, S1): the
    // boundary expands them, so does the check
    s.add(
        mk<NEG>(mk<EQ>(px.expand(ex.expand(lhs)), px.expand(ex.expand(rhs)))));
    if (getenv("SEAM_TEST_DUMP"))
      s.to_smt_lib(llvm::errs());
    return s.check() == sea::solver::SolverResult::UNSAT;
  }

  /// the memories are equal at every address in \p addrs (arrays expanded)
  bool sameAt(const Expr &m1, const Expr &mm2,
              std::initializer_list<Expr> addrs, bool withLayout) {
    StoreMapExpander ex;
    PtrIndexExpander px;
    Expr e1 = px.expand(ex.expand(m1)), e2 = px.expand(ex.expand(mm2));
    sea::solver::yices_solver_impl s(efac);
    if (withLayout)
      for (const Expr &l : layout)
        s.add(px.expand(l));
    ExprVector diffs;
    for (const Expr &p : addrs)
      diffs.push_back(
          mk<NEG>(mk<EQ>(sel(e1, px.expand(p)), sel(e2, px.expand(p)))));
    s.add(mknary<OR>(diffs.begin(), diffs.end()));
    return s.check() == sea::solver::SolverResult::UNSAT;
  }
};

//===----------------------------------------------------------------------===//
// Fig. 6.4: write over write
//===----------------------------------------------------------------------===//

void testSmapNewAndHit() {
  Fixture F;
  Fixture::Enc e(F, Fixture::opts(), F.m);
  Expr p4 = F.at(F.o1, 4), p8 = F.at(F.o1, 8), p0 = F.o1;
  Expr w = F.var("w"), v = F.var("v"), u = F.var("u");
  // one write: a plain store
  Expr m1 = e.store(p4, w);
  CHECK(isOpX<STORE>(m1), "one write stays a store: " << str(m1));
  // SMAP-NEW: two adjacent writes to one object at numeral offsets
  Expr m2 = e.store(p8, v);
  CHECK(isOpX<STORE_MAP>(m2) && m2->arg(0) == F.m && m2->arg(1) == F.o1,
        "SMAP-NEW: " << str(m2));
  CHECK(e.rw.stats().get("smap.new") == 1, "smap.new");
  CHECK(countOp<STORE>(m2) == 0, "no store left in the store-map");
  // the list is insertion ordered, the latest first (thesis Fig. 7.2)
  Expr ocl = m2->arg(2);
  CHECK(isOpX<SMAP_CONS>(ocl) && ocl->arg(1) == v && ocl->arg(2)->arg(1) == w,
        "OCL order: " << str(ocl));
  // SMAP-HIT: a third write to another offset, then a rewrite of an offset
  Expr m3 = e.store(p0, u);
  CHECK(isOpX<STORE_MAP>(m3) && m3->arg(0) == F.m, "SMAP-HIT: " << str(m3));
  CHECK(e.rw.stats().get("smap.insert") == 1, "smap.insert");
  Expr v2 = F.var("v2");
  Expr m4 = e.store(p4, v2);
  CHECK(isOpX<STORE_MAP>(m4), "SMAP-HIT on an existing offset");
  // the latest value wins, in the map and in the list order
  CHECK(e.load(p4) == v2 && e.load(p8) == v && e.load(p0) == u,
        "reads of a store-map");
  CHECK(e.rw.stats().get("row.smap-hit") == 3, "R-HIT counted");
  // storing the value an offset already holds changes nothing
  CHECK(e.store(p4, v2) == m4, "no-op store");
  CHECK(e.rw.stats().get("smap.noop") == 1, "smap.noop");
  // a miss falls through to the array below (R-MISS), no assumption
  Expr p12 = F.at(F.o1, 12);
  CHECK(e.load(p12) == F.sel(F.m, p12), "R-MISS: " << str(e.load(p12)));
  CHECK(e.rw.stats().get("row.smap-miss") == 1, "row.smap-miss");
  // all sound against the raw chain, with and without A-INB
  for (const Expr &p : {p0, p4, p8, p12})
    CHECK(F.equivalent(e.load(p), e.rawLoad(p), false),
          "read unsound at " << str(p));
  CHECK(F.sameAt(e.mem, e.raw, {p0, p4, p8, p12, F.o2, F.x}, true),
        "memory differs from the raw chain");
  // the ordered map was moved on every insert: lookups never scanned
  CHECK(e.rw.stats().get("smap.scan") == 0, "smap.scan: the map was lost");
  CHECK(e.rw.stats().get("smap.move") >= 2, "smap.move");
}

void testCollapse() {
  Fixture F;
  // the same address twice: only the later write is kept (no assumption)
  {
    Fixture::Enc e(F, Fixture::opts(), F.m);
    Expr w = F.var("w"), v = F.var("v");
    e.store(F.at(F.o1, 4), w);
    Expr m = e.store(F.at(F.o1, 4), v);
    CHECK(m == F.st(F.m, F.at(F.o1, 4), v), "collapse: " << str(m));
    CHECK(e.rw.stats().get("wow.collapse") == 1, "wow.collapse");
    CHECK(F.sameAt(e.mem, e.raw, {F.at(F.o1, 4), F.o1}, false), "unsound");
  }
  // also at a symbolic address, and with Store-Map off nothing changes
  {
    Fixture::Enc e(F, Fixture::opts(), F.m);
    Expr w = F.var("w"), v = F.var("v");
    e.store(F.at(F.o1, 4), F.num(1));
    Expr p = F.add2(F.o1, F.x);
    e.store(p, w);
    Expr m = e.store(p, v);
    CHECK(countOp<STORE>(m) == 2 && F.sameAt(m, e.raw, {p, F.x, F.o1}, false),
          "symbolic collapse: " << str(m));
  }
  {
    Fixture::Enc e(F, Fixture::opts(true, false), F.m);
    Expr w = F.var("w"), v = F.var("v");
    e.store(F.at(F.o1, 4), w);
    Expr m = e.store(F.at(F.o1, 4), v);
    CHECK(countOp<STORE>(m) == 2 && !e.rw.storeMapsMade(),
          "storeMap=0: a plain chain");
  }
}

void testCommute() {
  Fixture F;
  Expr w = F.var("w"), v = F.var("v"), u = F.var("u");
  // WOW-COMMUTE: o1 is allocated before o2, so a write to o1 goes below one to
  // o2 (the head of a normal chain is the latest object)
  {
    Fixture::Enc e(F, Fixture::opts(), F.m);
    e.store(F.o2, w);
    Expr m = e.store(F.o1, v);
    CHECK(isOpX<STORE>(m) && m->arg(1) == F.o2 && isOpX<STORE>(m->arg(0)) &&
              m->arg(0)->arg(1) == F.o1 && m->arg(0)->arg(0) == F.m,
          "WOW-COMMUTE: " << str(m));
    CHECK(e.rw.stats().get("wow.commute") == 1, "wow.commute");
    CHECK(F.sameAt(e.mem, e.raw, {F.o1, F.o2, F.o3}, true),
          "WOW-COMMUTE unsound with A-INB");
    // already in order: no commute
    Expr m2 = e.store(F.o3, u);
    CHECK(m2->arg(0) == m && e.rw.stats().get("wow.commute") == 1,
          "in order: stays");
  }
  // without A-INB nothing is commuted, and the result holds with no assumption
  {
    Fixture::Enc e(F, Fixture::opts(false), F.m);
    e.store(F.o2, w);
    Expr m = e.store(F.o1, v);
    CHECK(m == F.st(F.st(F.m, F.o2, w), F.o1, v) &&
              e.rw.stats().get("wow.commute") == 0,
          "A-INB off: no commute: " << str(m));
  }
  // not for a size-0 object (it shares its base with a neighbour), and not
  // for an object that is not registered or a pointer without a view
  {
    Fixture::Enc e(F, Fixture::opts(), F.m);
    e.store(F.o3, w);
    Expr m = e.store(F.o0, v); // o0 has size 0
    CHECK(e.rw.stats().get("wow.commute") == 0, "size 0: no commute");
    CHECK(F.sameAt(m, e.raw, {F.o3, F.o0}, false), "size 0 sound");
    // the other order: o3 is allocated before o0. A store (positive width)
    // into the size-0 object o0 violates A-INB, so under A-INB the pair is
    // vacuous and the writes commute (cmp.size0-vacuous, SeaM-null.md); the
    // two are one address, so without A-INB they do not
    Fixture::Enc e3(F, Fixture::opts(), F.m);
    e3.store(F.o0, w);
    Expr m3 = e3.store(F.o3, v);
    CHECK(e3.rw.stats().get("wow.commute") == 1 &&
              e3.rw.stats().get("cmp.size0-vacuous") == 1,
          "size 0, other order: vacuous commute: " << str(m3));
    Fixture::Enc e4(F, Fixture::opts(false), F.m);
    e4.store(F.o0, w);
    Expr m4 = e4.store(F.o3, v);
    CHECK(e4.rw.stats().get("wow.commute") == 0 && m4 == e4.raw,
          "size 0, A-INB off: no commute: " << str(m4));
    CHECK(F.sameAt(m4, e4.raw, {F.o3, F.o0}, false), "size 0 (2) sound");
    Fixture::Enc e2(F, Fixture::opts(), F.m);
    Expr ld = mk<PTR_FROM_BITS>(F.sel(F.m2, F.p)); // a loaded pointer
    e2.store(F.o2, w);
    Expr m2 = e2.store(ld, v);
    CHECK(e2.rw.stats().get("wow.commute") == 0 && isOpX<STORE>(m2) &&
              m2->arg(0) == F.st(F.m, F.o2, w),
          "a pointer without a view is a barrier");
  }
  // WOSMAP-COMMUTE: a write to o1 goes below the store-map of o2, and meets the
  // write to o1 beneath it (SMAP-NEW) or a store-map of o1
  {
    Fixture::Enc e(F, Fixture::opts(), F.m);
    e.store(F.at(F.o1, 4), F.num(1)); // o1: one write
    e.store(F.at(F.o2, 0), w);
    Expr sm2 = e.store(F.at(F.o2, 4), v); // store-map of o2 over o1's write
    CHECK(isOpX<STORE_MAP>(sm2) && sm2->arg(1) == F.o2 &&
              isOpX<STORE>(sm2->arg(0)) && sm2->arg(0)->arg(1) == F.at(F.o1, 4),
          "o2 store-map over a write to o1: " << str(sm2));
    Expr m = e.store(F.at(F.o1, 8), u);
    CHECK(isOpX<STORE_MAP>(m) && m->arg(1) == F.o2 &&
              isOpX<STORE_MAP>(m->arg(0)) && m->arg(0)->arg(1) == F.o1,
          "WOSMAP-COMMUTE then SMAP-NEW: " << str(m));
    CHECK(e.rw.stats().get("smap.commute") == 1, "smap.commute");
    // and one more to o1: SMAP-HIT in the inner store-map, found through the
    // commute again
    Expr m3 = e.store(F.at(F.o1, 12), u);
    CHECK(m3->arg(1) == F.o2 && m3->arg(0)->arg(1) == F.o1 &&
              countOp<STORE_MAP>(m3) == 2 && countOp<STORE>(m3) == 0,
          "two store-maps: " << str(m3));
    CHECK(F.sameAt(e.mem, e.raw,
                   {F.at(F.o1, 4), F.at(F.o1, 8), F.at(F.o1, 12), F.at(F.o2, 0),
                    F.at(F.o2, 4), F.o3, F.x},
                   true),
          "WOSMAP-COMMUTE unsound");
  }
  // with A-INB off the write stays on top, and the memory is equal with no
  // assumption at all
  {
    Fixture::Enc e(F, Fixture::opts(false), F.m);
    e.store(F.at(F.o2, 0), w);
    e.store(F.at(F.o2, 4), v);
    Expr m = e.store(F.at(F.o1, 8), u);
    CHECK(isOpX<STORE>(m) && isOpX<STORE_MAP>(m->arg(0)) &&
              e.rw.stats().get("smap.commute") == 0,
          "A-INB off: the write stays above the store-map: " << str(m));
    CHECK(F.sameAt(m, e.raw, {F.at(F.o1, 8), F.at(F.o2, 0), F.at(F.o2, 4), F.x},
                   false),
          "A-INB off: unsound");
  }
}

void testBarriers() {
  Fixture F;
  Expr w = F.var("w"), v = F.var("v"), u = F.var("u");
  // a write at o1 + x (symbolic) is a barrier for the writes of o1 around it
  Fixture::Enc e(F, Fixture::opts(), F.m);
  e.store(F.at(F.o1, 0), F.num(1));
  e.store(F.add2(F.o1, F.x), w);
  Expr m = e.store(F.at(F.o1, 4), v);
  CHECK(countOp<STORE_MAP>(m) == 0 && countOp<STORE>(m) == 3,
        "no store-map across a symbolic offset: " << str(m));
  // a write to another object does commute across it (distinct objects) ...
  Expr m2 = e.store(F.o2, u);
  CHECK(isOpX<STORE>(m2) && m2->arg(1) == F.o2, "later object on top");
  // ... and o1 + 0 below it meets nothing
  CHECK(F.sameAt(e.mem, e.raw,
                 {F.at(F.o1, 0), F.at(F.o1, 4), F.add2(F.o1, F.x), F.o2, F.y},
                 true),
        "barrier chain unsound");
  // reads through it: a hit on the top, the compare unknown below
  Expr r = e.load(F.at(F.o1, 4));
  CHECK(r == v, "read the last write: " << str(r));
  Expr r2 = e.load(F.at(F.o1, 0));
  CHECK(F.equivalent(r2, e.rawLoad(F.at(F.o1, 0)), true),
        "read below the barrier: " << str(r2));
}

//===----------------------------------------------------------------------===//
// Fig. 6.5: read over store-map
//===----------------------------------------------------------------------===//

/// store-map(a, base, {offsets -> values}) built by hand (no table: the lookups
/// scan the list)
Expr smapOf(Fixture &F, const Expr &a, const Expr &base,
            std::initializer_list<std::pair<unsigned long, Expr>> entries) {
  Expr ocl = op::array::smapNil(F.efac);
  for (auto &e : entries)
    ocl = op::array::smapCons(F.num(e.first), e.second, ocl);
  return op::array::storeMap(a, base, ocl);
}

void testRead() {
  Fixture F;
  Expr w = F.var("w"), v = F.var("v"), u = F.var("u");
  Expr sm = smapOf(F, F.m, F.o2, {{0, w}, {8, v}});
  auto rewrite = [&](const Expr &e, SeaMOptions o, RewriteStats &st) {
    SeaMRewriter rw(F.efac, F.reg, o);
    Expr r = rw.rewrite(e);
    st = rw.stats();
    return r;
  };
  RewriteStats st;
  // R-HIT
  CHECK(rewrite(F.sel(sm, F.at(F.o2, 8)), Fixture::opts(), st) == v &&
            st.get("row.smap-hit") == 1,
        "R-HIT");
  // R-MISS: no assumption (another numeral offset)
  {
    SeaMOptions noInb = Fixture::opts(false);
    Expr r = rewrite(F.sel(sm, F.at(F.o2, 4)), noInb, st);
    CHECK(r == F.sel(F.m, F.at(F.o2, 4)) && st.get("row.smap-miss") == 1,
          "R-MISS: " << str(r));
  }
  // R-SKIP: another object (A-INB)
  {
    Expr r = rewrite(F.sel(sm, F.at(F.o1, 8)), Fixture::opts(), st);
    CHECK(r == F.sel(F.m, F.at(F.o1, 8)) && st.get("row.smap-skip") == 1,
          "R-SKIP: " << str(r));
    CHECK(F.equivalent(F.sel(sm, F.at(F.o1, 8)), r, true), "R-SKIP unsound");
    // without A-INB: R-ABORT (a compare against every entry, which cannot be
    // decided), and equal with no assumption
    Expr r2 = rewrite(F.sel(sm, F.at(F.o1, 8)), Fixture::opts(false), st);
    CHECK(st.get("row.smap-skip") == 0 && st.get("row.smap-abort") == 1 &&
              countOp<STORE_MAP>(r2) == 0 && countOp<SELECT>(r2) == 1,
          "A-INB off: R-ABORT: " << str(r2));
    CHECK(F.equivalent(F.sel(sm, F.at(F.o1, 8)), r2, false),
          "A-INB off unsound");
  }
  // R-ABORT: a symbolic offset of the same base: the ite chain of the entries
  {
    Expr idx = F.add2(F.o2, F.x);
    Expr r = rewrite(F.sel(sm, idx), Fixture::opts(), st);
    // x * 1: the offset of an addr.add node, as the rules expand it without
    // simplify
    Expr x1 = mk<BMUL>(F.x, F.num(1));
    Expr want = mk<ITE>(mk<EQ>(F.num(8), x1), v,
                        mk<ITE>(mk<EQ>(F.num(0), x1), w, F.sel(F.m, idx)));
    CHECK(st.get("row.smap-abort") == 1 && countOp<STORE_MAP>(r) == 0 &&
              countOp<STORE>(r) == 0 && r == want,
          "R-ABORT, symbolic offset: " << str(r) << " expected " << str(want));
    CHECK(F.equivalent(F.sel(sm, idx), r, false), "R-ABORT unsound");
  }
  // R-ABORT: no pointer view (a loaded pointer), and an ite of pointers
  {
    Expr loaded = F.sel(F.m2, F.y);
    Expr r = rewrite(F.sel(sm, loaded), Fixture::opts(), st);
    CHECK(st.get("row.smap-abort") == 1 && countOp<STORE_MAP>(r) == 0 &&
              countOp<ITE>(r) == 2,
          "R-ABORT, no view: " << str(r));
    CHECK(F.equivalent(F.sel(sm, loaded), r, false),
          "R-ABORT (loaded) unsound");
    Expr it = mk<ITE>(F.a, F.at(F.o2, 8), F.at(F.o1, 4));
    Expr r2 = rewrite(F.sel(sm, it), Fixture::opts(), st);
    CHECK(countOp<STORE_MAP>(r2) == 0 && F.equivalent(F.sel(sm, it), r2, true),
          "R-ABORT, ite of pointers: " << str(r2));
    // the A-INB part: the o1 leaf cannot be an entry of o2: the compare of
    // the entry (8) is `a`, of the entry (0) false
    CHECK(countOp<EQ>(r2) == 0,
          "R-ABORT: leaves decided by A-INB: " << str(r2));
  }
  // R-ABORT with an entry that the compare decides: HIT ends the read
  {
    Expr idx = F.add2(F.o2, F.x);
    Expr sm2 = smapOf(F, F.m, F.o2, {{0, w}, {8, v}});
    // x + 8 is not a numeral compare: unknown; but o2 + 8 + 0 is
    Expr r = rewrite(F.sel(sm2, F.add2(F.at(F.o2, 8), F.num(0))),
                     Fixture::opts(), st);
    CHECK(r == v, "R-HIT of a normalized sum");
    (void)idx;
  }
  // expandUnknownRow off: the read stays a select over plain stores (the
  // solver's array theory), never a store-map
  {
    SeaMOptions o = Fixture::opts();
    o.expandUnknownRow = false;
    Expr idx = F.add2(F.o2, F.x);
    Expr r = rewrite(F.sel(sm, idx), o, st);
    CHECK(isOpX<SELECT>(r) && countOp<STORE_MAP>(r) == 0 &&
              countOp<STORE>(r) == 2 && r->arg(0)->arg(0)->arg(0) == F.m,
          "expandUnknownRow=0: " << str(r));
    CHECK(F.equivalent(F.sel(sm, idx), r, false), "no-expand unsound");
  }
  // a store-map over an ite memory and a read through an ite join
  {
    Expr j = mk<ITE>(F.a, sm, smapOf(F, F.m2, F.o3, {{4, u}, {12, w}}));
    Expr r = rewrite(F.sel(j, F.at(F.o3, 4)), Fixture::opts(), st);
    CHECK(r == mk<ITE>(F.a, F.sel(F.m, F.at(F.o3, 4)), u),
          "read through a join of store-maps: " << str(r));
  }
}

//===----------------------------------------------------------------------===//
// the ordered map table
//===----------------------------------------------------------------------===//

void testTable() {
  Fixture F;
  Expr w = F.var("w"), v = F.var("v"), u = F.var("u"), t = F.var("t");
  Fixture::Enc e(F, Fixture::opts(), F.m);
  e.store(F.at(F.o1, 0), w);
  Expr base = e.store(F.at(F.o1, 4), v); // store-map with the map
  Expr m1 = e.store(F.at(F.o1, 8), u);   // moves the map to m1's list
  // a fork: the second extension of `base` finds no map and rebuilds it
  Expr m2 = e.store(F.at(F.o1, 12), t);
  (void)m1;
  (void)m2;
  uint64_t rebuilds = e.rw.stats().get("smap.rebuild");
  // extend the old version `base` again (as a join branch would)
  Expr fork = e.rw.rewrite(F.st(base, F.at(F.o1, 16), t));
  CHECK(isOpX<STORE_MAP>(fork), "fork: a store-map");
  CHECK(rebuilds == 0 && e.rw.stats().get("smap.rebuild") == 1,
        "the second extension of one list rebuilds the map once");
  // reads on the old version: no map (moved), so a scan; same answers
  uint64_t scans = e.rw.stats().get("smap.scan");
  Expr oldRead = e.rw.rewrite(F.sel(base, F.at(F.o1, 4)));
  CHECK(oldRead == v, "read of an old version");
  CHECK(e.rw.stats().get("smap.scan") == scans + 1, "an old version scans");
  uint64_t scans2 = e.rw.stats().get("smap.scan");
  CHECK(e.load(F.at(F.o1, 0)) == w && e.load(F.at(F.o1, 12)) == t,
        "the latest version answers");
  CHECK(e.rw.stats().get("smap.scan") == scans2,
        "the latest version has a map");
  // a list with shadowed entries: the first entry met is the latest
  Expr sm = smapOf(F, F.m, F.o1, {{4, w}, {8, v}, {4, u}});
  RewriteStats st;
  SeaMRewriter rw(F.efac, F.reg, Fixture::opts());
  CHECK(rw.rewrite(F.sel(sm, F.at(F.o1, 4))) == u, "latest entry wins (scan)");
  // direct table API
  RewriteStats ts;
  StoreMapTable tab(ts);
  Expr nil = op::array::smapNil(F.efac);
  Expr l = tab.make2(F.num(0), w, F.num(4), v);
  CHECK(tab.find(l, 4) == v && tab.find(l, 0) == w && !tab.find(l, 8),
        "make2 lookups");
  Expr l2 = tab.insert(l, F.num(0), u);
  CHECK(tab.find(l2, 0) == u && tab.find(l2, 4) == v, "insert");
  CHECK(ts.get("smap.scan") == 0, "table: no scan");
  CHECK(tab.find(l, 0) == w && ts.get("smap.scan") == 1,
        "the old list lost its map: scan, same answer");
  CHECK(StoreMapTable::entries(l2).size() == 2 &&
            StoreMapTable::entries(l2).back().value == u,
        "entries: distinct offsets, latest values, latest last");
  (void)nil;
}

//===----------------------------------------------------------------------===//
// expandStoreMaps
//===----------------------------------------------------------------------===//

void testExpand() {
  Fixture F;
  Expr w = F.var("w"), v = F.var("v"), u = F.var("u");
  StoreMapExpander ex;
  // stores oldest first; the latest value of an offset wins; offset 0 is the
  // base itself
  Expr sm = smapOf(F, F.m, F.o1, {{0, w}, {4, v}, {0, u}});
  // entry addresses are PTR_INDEX nodes (PtrSort rev 5, S1.3), expanded
  // at the boundary: compared expanded
  PtrIndexExpander px;
  Expr out = px.expand(ex.expand(sm));
  Expr want = px.expand(F.st(F.st(F.m, F.at(F.o1, 4), v), F.o1, u));
  CHECK(out == want ||
            out == px.expand(F.st(F.st(F.m, F.o1, u), F.at(F.o1, 4), v)),
        "expansion: " << str(out));
  CHECK(countOp<STORE_MAP>(out) == 0 && ex.expanded() == 1, "expanded count");
  // a term with no store-map is returned as it is
  Expr plain = F.st(F.m, F.o1, w);
  CHECK(ex.expand(plain) == plain, "no store-map: unchanged");
  // nested: a store-map in the array and in a value (a read of another one)
  Expr inner = smapOf(F, F.m2, F.o2, {{0, F.num(7)}, {8, F.num(9)}});
  Expr outer =
      smapOf(F, inner, F.o1, {{0, F.sel(inner, F.at(F.o2, 8))}, {4, v}});
  Expr o = ex.expand(F.sel(outer, F.o3));
  CHECK(countOp<STORE_MAP>(o) == 0 && countOp<SMAP_CONS>(o) == 0,
        "nested expansion");
  CHECK(F.equivalent(F.sel(outer, F.o3), o, false), "nested expansion sound");
  CHECK(F.sameAt(outer, ex.expand(outer), {F.o1, F.at(F.o1, 4), F.at(F.o2, 8)},
                 false),
        "expanded memory differs");
  // deep: a 30000-entry list and a 30000-deep chain of store-maps (iterative)
  {
    constexpr unsigned kDepth = 30000;
    Expr ocl = op::array::smapNil(F.efac);
    for (unsigned k = 0; k < kDepth; ++k)
      ocl = op::array::smapCons(F.num(k % 200), F.num(k % 1000), ocl);
    Expr big = op::array::storeMap(F.m, F.o1, ocl);
    StoreMapExpander ex2;
    Expr e2 = ex2.expand(big);
    CHECK(countOp<STORE>(e2) == 200, "deep list: 200 distinct offsets");
    Expr chain = F.m;
    for (unsigned k = 0; k < kDepth; ++k)
      chain = smapOf(F, chain, F.o1, {{k % 7, F.num(k % 100)}, {8, w}});
    Expr e3 = ex2.expand(chain);
    CHECK(countOp<STORE_MAP>(e3) == 0, "deep chain of store-maps");
  }
  // a store-map is typed as its array
  TypeChecker tc;
  CHECK(tc.typeOf(sm) == F.memSort, "typeOf(store-map) = array sort");
}

//===----------------------------------------------------------------------===//
// random: stores over three objects with joins, then reads
//===----------------------------------------------------------------------===//

struct Rng {
  unsigned long s;
  explicit Rng(unsigned long seed) : s(seed) {}
  unsigned next(unsigned n) {
    s = s * 6364136223846793005UL + 1442695040888963407UL;
    return (s >> 33) % n;
  }
};

void testRandom(unsigned count) {
  unsigned unsound = 0, notNormal = 0, left = 0, maps = 0, reads = 0;
  for (unsigned i = 0; i < count; ++i) {
    Fixture F;
    Rng rng(20261101 + i);
    bool inb = i % 4 != 3; // every fourth program without A-INB
    Fixture::Enc e(F, Fixture::opts(inb), F.m);
    auto base = [&] {
      switch (rng.next(3)) {
      case 0:
        return F.o1;
      case 1:
        return F.o2;
      default:
        return F.o3;
      }
    };
    auto ptr = [&]() -> Expr {
      switch (rng.next(8)) {
      case 0:
        return base();
      case 1:
        return F.add2(base(), F.x);
      case 2:
        return F.add2(base(), F.y);
      case 3:
        return mk<ITE>(i % 2 ? F.a : F.b, F.at(base(), 4 * rng.next(3)),
                       F.at(base(), 4 * rng.next(3)));
      default:
        return F.at(base(), 4 * rng.next(6));
      }
    };
    unsigned n = 4 + rng.next(14);
    Expr joined = Expr();
    for (unsigned k = 0; k < n; ++k) {
      Expr p = ptr();
      Expr v = rng.next(4) == 0 ? F.z : F.num(1 + rng.next(4));
      e.store(p, v);
      if (rng.next(8) == 0) { // a join of the memory with an older one
        Expr other = F.st(F.m2, ptr(), F.num(9));
        Expr c = F.cond("j" + std::to_string(k));
        e.mem = mk<ITE>(c, e.mem, e.rw.rewrite(other));
        e.raw = mk<ITE>(c, e.raw, other);
        joined = c;
      }
    }
    (void)joined;
    // the stored memory is a normal form
    {
      Expr again = e.rw.rewrite(e.mem);
      if (again != e.mem) {
        ++notNormal;
        llvm::errs() << "FAIL: random " << i << " memory not normal\n";
      }
    }
    maps += countOp<STORE_MAP>(e.mem) > 0;
    for (unsigned r = 0; r < 4; ++r) {
      Expr p = ptr();
      Expr got = e.load(p), raw = e.rawLoad(p);
      ++reads;
      if (countOp<STORE_MAP>(got) || countOp<SMAP_CONS>(got))
        ++left;
      if (!F.equivalent(got, raw, inb)) {
        ++unsound;
        llvm::errs() << "FAIL: random " << i << " read of " << str(p)
                     << " unsound (A-INB " << inb << "): " << str(got) << "\n";
      }
    }
    // the memory itself, cell by cell (expanded)
    if (!F.sameAt(e.mem, e.raw, {F.o1, F.at(F.o1, 4), F.at(F.o2, 8), F.o3, F.x},
                  inb)) {
      ++unsound;
      llvm::errs() << "FAIL: random " << i << " memory differs\n";
    }
  }
  CHECK(unsound == 0, "random: " << unsound << " unsound");
  CHECK(left == 0, "random: store-map left in a read: " << left);
  CHECK(notNormal == 0, "random: not normal: " << notNormal);
  CHECK(maps > count / 4, "random: too few store-maps made: " << maps);
  llvm::outs() << "random: programs=" << count << " with-store-map=" << maps
               << " reads=" << reads << " unsound=" << unsound
               << " store-map-left=" << left << "\n";
}

//===----------------------------------------------------------------------===//
// R-ABORT, R-MISS and chain-start caching
//===----------------------------------------------------------------------===//
//
// Fig. 7.1 with store-maps:
//   M_{k+1} = ite(c_k, store-map(M_k, b, E_k), store-map(M_k, b, E'_k))
// read at one address, so that both arms of every level end in the same read
// of M_k. What makes the rewriting of a read of M_N linear is that the start
// of a nested rewrite (a ~> b ~> c) is cached (thesis Alg. 4-5). Three shapes:
//   Miss         numeral address, no entry at it (R-MISS: the follow-up is
//                the select over the array below, a hop of a chain), a hit at
//                one level (as Fig. 7.1's midpoint scheme).
//   AbortNarrow  address b + s, s symbolic: R-ABORT, with one entry per
//                store-map (the follow-up ite(b + o = idx, v, select(a0, idx))
//                has the select as a kid: the thesis' trigger).
//   AbortWide    the same with three entries: the follow-up is a chain of ites
//                that are cached by their own frames, which protect the select
//                at the bottom: linear even without chain-start caching
//                (measured; the one-shot revert of this implementation, the
//                SeaHorn branch's way, not the thesis' peel of one entry per
//                step).

enum class Shape { Miss, AbortNarrow, AbortWide };

struct AbortRun {
  RewriteStats stats;
  bool aborted = false;
  bool sound = true;
  bool clean = true; // no store-map and no store left
};

AbortRun runShape(Shape shape, unsigned n, bool chainStartCache,
                  uint64_t maxCalls, bool checkSound) {
  Fixture F;
  Expr M = F.m;
  Expr idx = shape == Shape::Miss ? F.at(F.o1, 100) : F.add2(F.o1, F.x);
  for (unsigned k = 0; k < n; ++k) {
    Expr c = F.cond("c" + std::to_string(k));
    Expr l, r;
    if (shape == Shape::AbortNarrow) {
      l = smapOf(F, M, F.o1, {{4, F.num(10 + k)}});
      r = smapOf(F, M, F.o1, {{12, F.num(30 + k)}});
    } else if (shape == Shape::AbortWide) {
      l = smapOf(F, M, F.o1,
                 {{0, F.num(10 + k)}, {4, F.num(20 + k)}, {8, F.var("p")}});
      r = smapOf(F, M, F.o1, {{12, F.num(30 + k)}, {16, F.num(40 + k)}});
    } else {
      l = smapOf(F, M, F.o1,
                 {{0, F.num(10 + k)}, {k == n / 2 ? 100 : 4, F.num(20 + k)}});
      r = smapOf(F, M, F.o1, {{12, F.num(30 + k)}, {16, F.num(40 + k)}});
    }
    M = mk<ITE>(c, l, r);
  }
  Expr root = F.sel(M, idx);
  AbortRun run;
  RewriteOptions ro;
  ro.chainStartCache = chainStartCache;
  ro.maxRuleCalls = maxCalls;
  SeaMRewriter rw(F.efac, F.reg, Fixture::opts(), ro);
  Expr res = rw.rewrite(root);
  run.stats = rw.stats();
  run.aborted = res == root && maxCalls && run.stats.ruleCalls >= maxCalls;
  run.clean = countOp<STORE_MAP>(res) == 0 && countOp<STORE>(res) == 0;
  if (checkSound && !run.aborted)
    run.sound = F.equivalent(root, res, false);
  return run;
}

const char *nameOf(Shape s) {
  return s == Shape::Miss          ? "miss"
         : s == Shape::AbortNarrow ? "abort-narrow"
                                   : "abort-wide";
}

void testAbortCaching() {
  // measured: 6 N (miss), 3 N (abort) rule applications; the bound leaves room
  // for rule changes that add a few per level and is far below any super-linear
  // behavior
  constexpr unsigned kRulesPerLevel = 12, kRulesConst = 40;
  for (Shape shape : {Shape::Miss, Shape::AbortNarrow, Shape::AbortWide}) {
    const char *nm = nameOf(shape);
    // the reads are sound (small N, Yices)
    {
      AbortRun r = runShape(shape, 3, true, 0, true);
      CHECK(r.sound, nm << ": N=3 unsound");
      CHECK(r.clean, nm << ": N=3 left a store-map or a store");
      CHECK(shape == Shape::Miss ? r.stats.get("row.smap-miss") > 0
                                 : r.stats.get("row.smap-abort") > 0,
            nm << ": the shape does not take its path");
    }
    // linear in N with chain-start caching
    AbortRun r20 = runShape(shape, 20, true, 100 * 20, false);
    AbortRun r40 = runShape(shape, 40, true, 100 * 40, false);
    AbortRun r80 = runShape(shape, 80, true, 100 * 80, false);
    for (auto *r : {&r20, &r40, &r80})
      llvm::outs() << "smap-shape " << nm << " variant=default N="
                   << (r == &r20   ? 20
                       : r == &r40 ? 40
                                   : 80)
                   << " rules-fired=" << r->stats.rulesFired
                   << " rule-calls=" << r->stats.ruleCalls << "\n";
    CHECK(!r40.aborted && r40.clean,
          nm << ": N=40 gave up or left a store-map");
    CHECK(r40.stats.rulesFired <= kRulesPerLevel * 40 + kRulesConst,
          nm << ": N=40: rule applications above c*N: "
             << r40.stats.rulesFired);
    CHECK(r40.stats.rulesFired <= 2 * r20.stats.rulesFired + 64 &&
              r80.stats.rulesFired <= 2 * r40.stats.rulesFired + 64,
          nm << ": super-linear growth");
    CHECK(r40.stats.chainStartCaches > 0, nm << ": no chain start cached");
    // ablation: without caching the start of a chain the same read is walked
    // from every arm again: the count explodes (N = 12 against the cached run)
    const unsigned n = 12;
    AbortRun on = runShape(shape, n, true, 0, false);
    AbortRun off = runShape(shape, n, false, 2000000, false);
    llvm::outs() << "smap-ablation " << nm << " N=" << n
                 << " with-cache=" << on.stats.rulesFired
                 << " without=" << off.stats.rulesFired
                 << (off.aborted ? " (gave up)" : "") << "\n";
    CHECK(off.stats.chainStartCaches == 0, nm << ": cached while off");
    if (shape == Shape::AbortWide) {
      // documented: the ite chains protect the select (see above)
      CHECK(off.stats.rulesFired < 40 * n * n,
            nm << ": expected linear without chain-start caching");
      continue;
    }
    CHECK(off.stats.rulesFired > 40 * n * n,
          nm << ": without chain-start caching the shape is not super-linear: "
                "the test cannot detect a regression");
    CHECK(on.stats.rulesFired * 20 < off.stats.rulesFired,
          nm << ": chain-start caching does not pay");
  }
}

} // namespace

int main(int argc, char **argv) {
  unsigned randomCount = argc > 1 ? std::atoi(argv[1]) : 12;
  struct Test {
    const char *name;
    void (*fn)();
  } tests[] = {{"smap-new-hit", testSmapNewAndHit},
               {"collapse", testCollapse},
               {"commute", testCommute},
               {"barriers", testBarriers},
               {"read", testRead},
               {"table", testTable},
               {"expand", testExpand},
               {"abort-caching", testAbortCaching}};
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
  int before = failures;
  testRandom(randomCount);
  llvm::outs() << "test: random " << (failures == before ? "ok" : "FAILED")
               << "\n";
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
