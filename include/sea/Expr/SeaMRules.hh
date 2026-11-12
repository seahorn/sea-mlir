// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// SeaM rules (docs/design/SeaM.md, "Rules"): object-
// aware simplification of memory terms, in one rewrite config separate from
// the minimal test config (ExprRewriteRules.hh).
//
// Terms. Addresses are bit-vectors. A term is a *pointer into object k* when
// it normalizes to bvadd(obj.k, off) (or is obj.k), obj.k in the BaseRegistry
// (BaseRegistry.hh); `off` is any bit-vector term. The thesis' ptr(b, o) is
// this view; =p is `=` on the normalized terms (Fig. 4.2, 4.3).
//
// Soundness. Conditional (docs/design/SeaM.md, "Soundness stance"). A rule
// that uses an assumption names it in its comment as `uses: ...`; a rule with
// no `uses` line is valid over plain bit-vectors and arrays (the unit tests
// check every case with Yices: `assumptions => lhs = rhs`).
//   A-INB: the addresses of memory accesses lie in their objects. Stated on
//          the syntactic view: an access address that normalizes to
//          bvadd(obj.k, off) (a "pointer into object k", see view(): `off`
//          mentions no registered base) satisfies obj.k <= addr and addr +
//          unit <= obj.k + size_k, for every access unit. So two such
//          addresses into different objects (both of nonzero size) are
//          different. It is a fact about the program, not about the layout:
//          sea-vc checks what it can (SeaOpSem::inBounds*: static violations
//          fall back to no assumption; the rest is assumed) and
//          says so in its output.
//          Objects of size 0 (the null object of sea.ptr.null, zero-sized
//          allocations; docs/design/SeaM-null.md) hold no access of positive
//          width: A-INB is false for such an access, so a ROW leaf pair
//          with one side in such an object is vacuous (`cmp.size0-vacuous`,
//          decided different). Its pointers are never separated by base in
//          a program compare (they may share an address with a neighbour).
//          A range [d, d + n) in such an object is in bounds iff n = 0.
// Used only by `cmp.diff-base`, `cmp.size0-vacuous` and by `arm.skip` across
// two bases (or dropping a size-0 provenance), and only on
// the compares ROW makes between a store address and a read address
// (`diffBase` of decideEq / decidePair, set by row() alone): a compare is not
// an access, so the equalities of the program (a one-past-the-end pointer
// against the next object's base is legal C) never use it, whether they stand
// in an address, a stored value or a condition.
//
// Shadow memories (docs/design/SeaM-null.md, "Shadow memories"). A memory
// term built over a start the registry knows as a shadow root
// (shadowMemory: `sea.mem.const ... shadow`, made by sea-lower-tracking)
// holds metadata keyed by object identity: its accesses are not program
// accesses, so none of the A-INB rules above applies to them (the compares
// of ROW, R-ABORT and WOW-COMMUTE there run with diffBase = false). Instead
// `shadow.distinct-base`: two different registered bases at offset 0 whose
// placed numerals differ are different cells. uses: LAYOUT (the placement
// equations obj.k = <numeral>, a fact of the VC; sea-place-static gives
// every object its own base, size 0 included).
//
// Provenance. A decision (decideEq / decidePair, ARM) is
// a `Decision`: the rule of every leaf pair it rests on, each with the
// premises (`Premise` bits) it needed, and their union; nothing is reduced to
// the first reason. The counters are derived from it: every reason is counted
// (`countReasons`), `arm.skip-diff-base` counts an ARM decision that separated
// two bases, and `premise.a-inb` counts every rewrite that depends on A-INB
// (ROW hit / skip, R-ABORT entries, R-SKIP, the commutes, a compare tree with
// a leaf decided by it). A run whose `premise.a-inb` is 0 does not use A-INB.
//
// RW_n. Every rule that returns RW_n says why n levels suffice: the rules
// apply at the result and n levels below; the terms the rule builds out of the
// (normal) kids of its input must have no redex below level n. A rule that
// returns a term built only of normal terms returns RW_DONE.
//
// Store-Map (thesis Ch. 6): `wow` at a store (Fig. 6.4) and the
// read over a store-map (Fig. 6.5) in `row`. A-INB is used by WOW-COMMUTE,
// WOSMAP-COMMUTE (the two addresses are in different objects, so the writes
// commute) and R-SKIP (the read is in another object than the store-map),
// through Rules::distinctObjects: both objects registered, assumeInBounds
// (one of size 0: cmp.size0-vacuous). R-ABORT decides each entry against the
// read address with the compare of ROW (decideEq / buildEq, diffBase), so an
// entry that is decided by a different object (cmp.diff-base, arm.skip across
// bases) uses A-INB as ROW does (with --seam-inb=0 it does not). Same-base
// merging (SMAP-NEW, SMAP-HIT), R-HIT and R-MISS use no assumption: offsets are
// numerals, different numerals are different addresses.
//
// Reference: thesis Fig. 2.1 (ite), 2.2 (compare), 2.3 (Boolean), 2.4 (add),
// 2.6 / 4.6 (read over write), 4.2 / 4.3 (pointer compare, arithmetic); the
// SeaHorn branch seam-dev14 (lib/seahorn/Smt/ExprRewriterDefs.cc,
// include/seahorn/Expr/ExprRewriteRule.hh) for the shape. Differences:
//  - ROW decides HIT / SKIP at once (decideEq) instead of building
//    ite(i = j, v, select(a, j)) and letting the compare and the ite rule
//    resolve it after the inner select has been rewritten as well: a HIT then
//    ends the chain (the thesis states that the chain stops early; with
//    bottom-up rewriting of the new ite it would not).
//  - the equality over an ite is pushed (ITE-EQ, Fig. 2.1) only when it is
//    bounded and resolves at least one leaf; the branch resolved
//    ite-pointers by ARM or not at all.
//  - n-ary bvadd is normalized with the bases first and the folded numeral
//    last, so that equal pointers are equal terms.
#include "sea/Expr/Ackermann.hh"
#include "sea/Expr/AddrRangeMap.hh"
#include "sea/Expr/BaseRegistry.hh"
#include "sea/Expr/BvRules.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprRewriteRules.hh"
#include "sea/Expr/ExprRewriter.hh"
#include "sea/Expr/InitialWords.hh"
#include "sea/Expr/MemOps.hh"
#include "sea/Expr/StoreMap.hh"
#include "sea/Expr/TypeChecker.hh"

#include "llvm/ADT/SmallVector.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace expr {

/// Switches and bounds of the SeaM rules.
struct SeaMOptions {
  /// Use A-INB: pointers into different objects are different. Off: the
  /// rules are valid over plain bit-vectors (nothing is assumed).
  bool assumeInBounds = true;
  /// Expand a read over a store whose address compare is unknown into
  /// ite(i = j, v, select(a, j)) (the thesis' ROW, which relies on ARM to
  /// decide most compares). Off: such a select is kept as it is (the solver's
  /// array theory handles it), so only the stores the compare decides are
  /// skipped or hit; the query stays near the size of the unrewritten one.
  bool expandUnknownRow = true;
  /// An expanded read over consecutive undecided writes of one value v
  /// (a plain store chain, or a store-map's open entries) is one
  /// ite(c_1 | ... | c_k, v, below), built at once (row.store-group /
  /// row.smap-group): linear in the chain, where one ite per write merged
  /// afterwards (bv.ite-merge) rebuilt a growing disjunction per level,
  /// quadratic. Exact. sea-vc --seam-row-group.
  bool groupSameValueRow = true;
  /// checkInbPremise (cmp.diff-base's A-INB premise names recorded
  /// accesses); sea-vc --seam-check-inb-premise. Off by default: on
  /// vcc-pi2: 9,475 of 11,117 checked addresses are not matched,
  /// mostly the matcher's gaps (keyed companions recorded through their
  /// mirror, other normal forms of one address), to be refined.
  bool checkInbPremise = false;
  /// without A-INB, decide the access pairs / memop ranges of two placed
  /// objects at numeral offsets by their placed numerals (cmp.layout,
  /// mem.in-layout; uses: LAYOUT). Off by default: valid only where the
  /// layout equations obj = n are part of the formula (sea-vc turns it on,
  /// --seam-layout); the unit drivers' bases are free constants
  bool layoutDecide = false;
  /// ARM (AddrRangeMap.hh, thesis Ch. 5): when the syntactic checks of a ROW
  /// compare leave it unknown, decide `!=` if the address range maps of the
  /// two sides are disjoint (counter arm.skip). Never for a program compare.
  bool arm = true;
  /// Store-Map (thesis Ch. 6, Fig. 6.4; StoreMap.hh): compress writes to one
  /// object at numeral offsets into store-map terms, commute writes to
  /// different objects into allocation order (uses A-INB, so only with
  /// assumeInBounds). Reads over store-maps are always rewritten (R-HIT,
  /// R-MISS, R-SKIP, R-ABORT), whatever this says: a store-map cannot go to a
  /// solver.
  bool storeMap = true;
  /// Pull bvadd through an ite of pointers (normalization).
  bool pullIte = true;
  /// PtrSort S4: a node is pulled through its first ite,
  /// base or index of any stride, when the parity rule (the base ite, or the
  /// first ite summand when it is a stride-1 index) does not fire; the same
  /// leaf bound (counter ptrindex.pull-ite-index). sea-vc --seam-pull-index.
  bool pullIteIndex = false;
  /// PtrSort S4: ptr.load-resolve through an ite of loaded values bounded by
  /// its distinct ite nodes (a memoized map over the DAG, linear) instead of
  /// its tree leaves (maxPullLeaves). 0: the leaf bound (counter
  /// ptr.load-resolve-ite-dag). sea-vc --seam-resolve-ite-nodes.
  unsigned resolveIteNodes = 0;
  /// PtrSort S6 probe: ptr.load-resolve through a split load whose bytes
  /// share an ite condition: concat(ite(c, a_k, b_k)) ~> ite(c, concat(a),
  /// concat(b)) (a byte that is not ite(c, ...) goes to both sides), at most
  /// this many pulls per load, kept only if a side reassembles a stored
  /// pointer's bits. 0: off (counter ptr.load-resolve-concat).
  /// sea-vc --seam-pull-concat.
  unsigned pullConcatIte = 0;
  /// cmp.congruence: decide two addresses apart when their difference is
  /// sum c_k x_k + d with 2^t dividing every c_k and not d (exact, no
  /// premise). Off by default until measured. sea-vc --seam-congruence.
  bool congruence = false;
  /// Push an equality through ite when that resolves a leaf (ITE-EQ).
  bool pushEqIte = true;
  /// A bvadd is pulled through ites only if the product of their leaf counts
  /// is at most this (the pull is exponential).
  unsigned maxPullLeaves = 4;
  /// A program compare (eq(), ITE-EQ) is pushed through ites only for at
  /// most this many leaf pairs.
  unsigned maxEqLeaves = 4;
  /// The decisions of the accesses look at most at this many leaf pairs:
  /// decideEq / decideIn (no term is built), and the compare trees of ROW
  /// (rowCompareEq) and of a memory-operation read (rowMemOp) are built over
  /// the same pairs, so every pair a decision looked at is resolved per leaf.
  unsigned maxDecideLeaves = 8;
  /// The boundary's byte expansion of a width term at an ite address
  /// (WideExpander) pushes the byte offset into the ite leaves, ite(c, x, y)
  /// + k ~> ite(c, x + k, y + k) (op::ptr::byteAddress), when the ite has at
  /// most this many distinct leaves (and ite nodes); 0: a + k over the ite.
  /// The rewriter does the same for the addresses it sees by
  /// ptrindex.pull-ite (maxPullLeaves); no rule runs after the boundary.
  /// Untuned: the cap of the decisions (sea-vc --seam-offset-leaves).
  unsigned maxOffsetLeaves = 8;
  /// BvRules.hh: the bit-vector and Boolean normalization rules
  /// (extract / concat, linear equality, AC order, Boolean ite), valid over
  /// plain bit-vectors, for every term the rewriter visits. The addends of a
  /// bvadd are sorted, and every result is cached (the rewriter is run over
  /// the whole query, not only memory reads). Off: the rules of phases 1-3
  /// only, as before.
  bool simplify = false;
  /// The rules of memory terms (ROW, WOW, Store-Map): select and store are
  /// left alone when off (`sea-vc --simplify` without --seam).
  bool memoryRules = true;
  /// Off: no rule at all; every term stays as OpSem built it (the raw
  /// encoding). Only sea-seam-export sets it (its raw lanes,
  /// docs/SeamExport.md); sea-vc never does.
  bool rules = true;
  /// addr.prune-size0 (docs/design/SeaM-provenance.md; uses A-INB,
  /// so only with assumeInBounds): drop the syntactic ite leaves of an access
  /// address (select / store index, memory-operation range) that are in an
  /// object of size 0.
  bool pruneSize0 = true;
  /// Width terms (docs/design/SeaM-mixed-width.md): a load / store of a byte
  /// memory wider than a byte is one wselect / wstore (not split into bytes
  /// by sea-split-wide-access); the boundary expands what is left. Off: the
  /// lowering splits wide accesses into bytes (as before).
  bool wideAccess = false;
  /// Rules may use the accesses' `align` annotations (trusted front-end
  /// facts); off: alignment only from the terms (congruence)
  bool assumeAligned = true;
  /// W-BYTE-IN: a byte read over an undecided wide write as one membership
  /// ite with a shift (off: expand the write into byte stores)
  bool wideByteIn = true;
  /// W-ITE (rowWideLeaves) with an undecided leaf pair: the read at that
  /// leaf address under its ite condition (on: a word value with one open
  /// read per open leaf; off: no W-ITE unless every pair is decided, the
  /// read goes to the byte fallback). Open question, untuned
  /// (sea-vc --seam-wide-ite-open).
  bool wideIteOpenLeaves = false;
  /// R-LEAVES (Rules::leafSplit): a read (select, wselect) at an ite address
  /// is split into the reads at its leaves when every leaf read is decided
  /// down the whole memory and some leaf is given by a write.
  /// Off: the read keeps its ite address (sea-vc --seam-leaf-split=0).
  /// The address is an ite or a node over an ite base; the leaves are
  /// counted distinct (the ite is a DAG), at most maxDecideLeaves.
  bool leafSplit = true;
  /// R-LEAVES-PARTIAL: R-LEAVES also when some leaf reads are undecided:
  /// the decided leaves (each given by a write) split, the undecided ones
  /// stay one read at the ite of them (the residual address). A tradeoff:
  /// the residual read is a new address next to the reads at the whole
  /// address other reads keep. Off by default, untuned
  /// (sea-vc --seam-leaf-split-partial).
  bool leafSplitPartial = false;
  /// R-STRIDE (PtrIndex.md section 0c; sea-vc --seam-stride): in R-ABORT, an
  /// entry b + o is skipped for a read whose every leaf is a promised node
  /// over b whose offsets never reach o. Uses the promises (PremPtrIndex)
  /// and, for K != 0, A-INB.
  bool strideRule = false;
  /// Test hook (sea-vc --seam-plant-bug, hidden): make one rule wrong, so a
  /// test shows that the per-firing check (--seam-check) catches
  /// it. Never set outside those tests.
  enum class PlantBug { None, SubConst, RowStoreHit };
  PlantBug plantBug = PlantBug::None;
  /// Test hook (sea-vc --seam-rule-limit, hidden): RewriteOptions::
  /// maxRuleCalls of the rewriter SeaOpSem makes (0: no limit).
  uint64_t maxRuleCalls = 0;
};

namespace seam {

/// Answer of the compare.
enum class Tri { No, Yes, Unknown };

/// The assumptions a decision or a rewrite depends on (docs/design/SeaM.md,
/// "Soundness stance"), as a bit set. A-INB is an assumption about the
/// program (discharged by a separate bounds check). LAYOUT is a fact of the
/// VC, not an assumption: the placement equations obj.k = <numeral> (side
/// conditions on every path); a rule that names it is valid given them
/// (--seam-check adds them), and it is never reported as assumed. A new
/// named assumption adds a bit, and every place that combines
/// premises already carries it.
enum Premise : unsigned {
  PremNone = 0,
  PremAInb = 1,
  PremLayout = 2,
  /// ALIGN: the `align` annotations of the accesses (front-end facts,
  /// trusted like LAYOUT; not an assumption, never
  /// reported as assumed; sea-vc --check-align is an optional sanity check
  /// of the front end). A rule that names it is valid given them
  /// (--seam-check adds them as `address & (a - 1) = 0`).
  PremAlign = 4,
  /// PTR_INDEX: the no-wrap promise (P1-P3) of a sea.ptr.index node (a GEP's
  /// inbounds / nusw / nuw; front-end facts, trusted like ALIGN). Counted
  /// `premise.ptr-index`; --seam-check adds op::ptr::promise of the nodes
  /// (PtrIndex.md section 5).
  PremPtrIndex = 8
};

/// What a compare decided, and everything it relied on: the rules that
/// decided each leaf pair it looked at (not only the first) with the
/// premises of each, and the union of the premises. Provenance for the rule
/// trail; the rule counters are derived from it (Rules::count).
/// Filled by a decision that is Yes or No; left alone when Unknown.
struct Decision {
  struct Reason {
    const char *rule;  ///< the counter name of the rule (a literal)
    unsigned premises; ///< Premise bits this use of the rule needed
  };
  /// distinct (rule, premises), in order of first use
  llvm::SmallVector<Reason, 4> reasons;
  /// union of the premises of the reasons
  unsigned premises = PremNone;

  void add(const char *rule, unsigned prem = PremNone) {
    premises |= prem;
    for (const Reason &r : reasons)
      if (r.premises == prem && std::strcmp(r.rule, rule) == 0)
        return;
    reasons.push_back({rule, prem});
  }
  void add(const Decision &o) {
    for (const Reason &r : o.reasons)
      add(r.rule, r.premises);
  }
  bool uses(Premise p) const { return (premises & p) != 0; }
};

/// A pointer into an object: base + offset.
struct PtrView {
  Expr base;
  Expr off;
};

class Rules {
public:
  Rules(ExprFactory &efac, const BaseRegistry &bases, RewriteStats &stats,
        SeaMOptions opts)
      : m_efac(efac), m_bases(bases), m_stats(stats), m_smaps(stats),
        m_opts(opts), m_arm(bases), m_bv(efac, bases, stats) {
    m_old.setArith([this](const Expr &e) { return flat(e); });
    m_cmpOld.setArith([this](const Expr &e) { return flat(e); });
  }
  Rules(const Rules &) = delete;
  Rules &operator=(const Rules &) = delete;

  const SeaMOptions &options() const { return m_opts; }
  RewriteStats &stats() { return m_stats; }
  const AddrRangeAnalysis &arm() const { return m_arm; }
  /// The normalization rules of BvRules.hh (used when options().simplify).
  bvnorm::Rules &bv() { return m_bv; }

  //===------------------------------------------------------------------===//
  // provenance: counters derived from what a decision relied on
  //===------------------------------------------------------------------===//

  /// Counts every rule that decided a leaf pair of \p d (not only the first),
  /// and `arm.skip-diff-base` for an ARM decision that separated two bases
  /// (A-INB), so a run reports how many of its skips are cross-object.
  void countReasons(const Decision &d) {
    for (const Decision::Reason &r : d.reasons) {
      m_stats.count(r.rule);
      if ((r.premises & PremAInb) && std::strcmp(r.rule, "arm.skip") == 0)
        m_stats.count("arm.skip-diff-base");
    }
  }
  /// Counts one rewrite (a decision used, a commute, a skip) that depends on
  /// the premises \p p: `premise.a-inb`. The number of firings in the run
  /// that an unsat would be conditional on; 0 means the answer does not use
  /// A-INB at all.
  void countPremises(unsigned p) {
    if (p & PremAInb)
      m_stats.count("premise.a-inb");
    if (p & PremLayout)
      m_stats.count("premise.layout");
    if (p & PremAlign)
      m_stats.count("premise.align");
    if (p & PremPtrIndex)
      m_stats.count("premise.ptr-index");
    m_stats.notePremise(p);
  }

  //===------------------------------------------------------------------===//
  // helpers shared by the rules
  //===------------------------------------------------------------------===//

  /// The bit-vector width of a term; of a pointer, its address width; 0 for
  /// a term the type checker gives neither sort (ill-sorted, or an operator
  /// with no sort rule), and for a width-0 term. sub.self, diff.self and
  /// incongruent check for 0 (no rewrite) before building at this width.
  unsigned widthOf(const Expr &e) const {
    unsigned w;
    if (op::bv::isBvNum(e, w))
      return w;
    if (op::bv::isBvConst(e))
      return op::bv::widthBvConst(e);
    // a pointer constant: its sort; a node: its base (S2e; the eraser asks
    // for the width of a node whose base is already erased, which does not
    // type-check)
    if (op::ptr::isPtrConst(e))
      return op::ptr::width(bind::rangeTy(e->first()));
    if (op::ptr::isPtrIndex(e))
      return widthOf(op::ptr::base(e));
    if (isOpX<BADD>(e) || isOpX<BSUB>(e) || isOpX<PTR_DIFF>(e) ||
        op::ptr::isMarker(e) || isOpX<OFFSET>(e))
      return widthOf(e->arg(0));
    if (isOpX<ITE>(e))
      return widthOf(e->arg(1));
    // One checker per Rules: its cache persists across calls. A fresh one
    // per call re-typed the whole memory chain under every select (each
    // pullIteNode / row call), quadratic on long store chains.
    if (!m_tc)
      m_tc = std::make_unique<TypeChecker>();
    Expr s = m_tc->typeOf(e);
    if (!isOpX<op::BVSORT>(s) && !op::ptr::isPtrSort(s))
      return 0;
    return op::ptr::addrSortWidth(s);
  }

  static mpz_class modPow2(const mpz_class &v, unsigned w) {
    mpz_class m, r;
    mpz_ui_pow_ui(m.get_mpz_t(), 2, w);
    mpz_fdiv_r(r.get_mpz_t(), v.get_mpz_t(), m.get_mpz_t());
    return r;
  }

  /// The numeral v mod 2^w (bv::bvnum does not reduce).
  Expr num(const mpz_class &v, unsigned w) const {
    return op::bv::bvnum(modPow2(v, w), w, m_efac);
  }

  /// The value of a numeral, reduced mod 2^width.
  static mpz_class numVal(const Expr &e) {
    return modPow2(op::bv::toMpz(e), op::bv::widthBvNum(e));
  }

  /// \p e is a numeral whose value is 0 (mod 2^w).
  static bool numVal0(const Expr &e) {
    unsigned w;
    return op::bv::isBvNum(e, w) && numVal(e) == 0;
  }

  /// `a` and `b` are numerals of one width: their values (reduced mod 2^w).
  static bool numerals(const Expr &a, const Expr &b, mpz_class &va,
                       mpz_class &vb) {
    unsigned wa, wb;
    if (!op::bv::isBvNum(a, wa) || !op::bv::isBvNum(b, wb) || wa != wb)
      return false;
    va = numVal(a);
    vb = numVal(b);
    return true;
  }

  /// bvadd of the terms, a single term, or the zero numeral of width \p w.
  Expr sumOf(const llvm::SmallVectorImpl<Expr> &terms, unsigned w) const {
    if (terms.empty())
      return num(0, w);
    if (terms.size() == 1)
      return terms.front();
    return mknary<BADD>(terms.begin(), terms.end());
  }

  /// RW_n for n levels (RW_FULL from 4 on, as the rewriter's statuses).
  static RWStatus levels(unsigned n) {
    return n >= 4 ? RWStatus::RW_FULL : static_cast<RWStatus>(n);
  }

  /// Number of leaves (non-ite terms) of the ite tree at \p e, counted up to
  /// about \p cap (a larger number only says "more than cap").
  unsigned leafCount(const Expr &e, unsigned cap) const {
    if (!isOpX<ITE>(e))
      return 1;
    unsigned n = leafCount(e->arg(1), cap);
    if (n > cap)
      return n;
    return n + leafCount(e->arg(2), cap);
  }

  //===------------------------------------------------------------------===//
  // PTR_INDEX nodes (docs/design/PtrIndex.md)
  //===------------------------------------------------------------------===//
  //
  // The VC builds a node for sea.ptr.index only. A node stays where
  // an address is used as one: the index of select / store / wselect /
  // wstore, the address arguments of a memory-operation term, a branch of an
  // ite, the base of a node. Everywhere else (compares, arithmetic, stored
  // values) the rewriter replaces it by its flat form (doRewrite), and the
  // deciders read an address through it (view, decidePair, decideEq,
  // wideRelation, offsetDiff, congruentStarts, ARM). The flat form is the
  // normalized term produced by sea-lower-ptr-index: the sum of the base's
  // summands, one x_k * s_k per index (normalized as bv.ac does) and the
  // numeral c + K. So every rule decides from the same normalized arithmetic as
  // the lowered program. The node is kept for the stride rule. The boundary
  // (SeaMRewriter::expandPtrIndex) replaces every node by its flat form.

  /// The summands of an address term into \p terms and the numeral \p sum:
  /// a numeral is added, a bvadd's arguments taken one by one, a node gives
  /// its base's summands, its index summands and c + K; any other term is
  /// taken as flatDeep of it.
  void addrSummands(const Expr &e, llvm::SmallVectorImpl<Expr> &terms,
                    mpz_class &sum, bvnorm::Rules *bv = nullptr) const {
    unsigned tw;
    if (op::bv::isBvNum(e, tw)) {
      sum = sum + numVal(e);
      return;
    }
    if (isOpX<BADD>(e)) {
      for (size_t i = 0; i < e->arity(); ++i)
        addrSummands(e->arg(i), terms, sum, bv);
      return;
    }
    if (!op::ptr::isPtrIndex(e)) {
      // an ite (a node's base) may hold nodes in its leaves: their flat
      // forms (else ARM and the A-INB condition
      // see an unknown term: a lost condition, byte_cursor_advance)
      terms.push_back(flatDeep(e));
      return;
    }
    unsigned n = widthOf(op::ptr::base(e));
    addrSummands(op::ptr::base(e), terms, sum, bv);
    for (size_t k = 0; k < op::ptr::numIndices(e); ++k)
      addrSummands(
          indexSummand(op::ptr::index(e, k), op::ptr::stride(e, k), n, bv),
          terms, sum, bv);
    sum = sum + op::ptr::constant(e) + op::ptr::byteOffset(e);
  }

  /// x * s as the rewriter normalizes the lowering's mul (bv.ac with
  /// simplify: x for s = 1, a numeral for a numeral x, a product flattened;
  /// without simplify the bvmul as it is).
  Expr indexSummand(const Expr &x, const mpz_class &s, unsigned n,
                    bvnorm::Rules *bv = nullptr) const {
    Expr m = mk<BMUL>(x, num(s, n));
    if (!m_opts.simplify)
      return m;
    return (bv ? *bv : const_cast<bvnorm::Rules &>(m_bv)).ac(m).exp;
  }

  /// The summands of \p e ordered as the add rule orders a sum: bases
  /// first, then by id (simplify) or as they came; the numeral last, none if
  /// zero.
  void sortedSummands(const Expr &e, llvm::SmallVectorImpl<Expr> &out) const {
    llvm::SmallVector<Expr, 8> terms;
    mpz_class sum = 0;
    addrSummands(e, terms, sum);
    unsigned w = widthOf(e);
    sum = modPow2(sum, w);
    if (m_opts.simplify)
      m_bv.sortSum(terms);
    else
      std::stable_partition(terms.begin(), terms.end(),
                            [&](const Expr &t) { return m_bases.isBase(t); });
    out.append(terms.begin(), terms.end());
    if (sum != 0)
      out.push_back(num(sum, w));
  }

  /// The flat form of a node: its summands as one normalized sum (a single
  /// term or a numeral when that is all there is). Memoized per node.
  Expr flat(const Expr &e) const {
    if (!op::ptr::isPtrIndex(e))
      return e;
    auto it = m_flatMemo.find(e.get());
    if (it != m_flatMemo.end())
      return it->second;
    llvm::SmallVector<Expr, 8> s;
    sortedSummands(e, s);
    Expr res = sumOf(s, widthOf(e));
    m_flatPin.push_back(e);
    m_flatMemo.emplace(e.get(), res);
    return res;
  }

  /// \p e with the nodes of its ite tree (ites, then a node) replaced by
  /// their flat forms: what the old lowering had at that position. \p e
  /// itself when there is none. Memoized per node.
  Expr flatDeep(const Expr &e) const {
    if (op::ptr::isPtrIndex(e))
      return flat(e);
    if (!isOpX<ITE>(e))
      return e;
    auto it = m_flatDeepMemo.find(e.get());
    if (it != m_flatDeepMemo.end())
      return it->second;
    Expr t = flatDeep(e->arg(1)), f = flatDeep(e->arg(2));
    Expr res = t == e->arg(1) && f == e->arg(2) ? e : mk<ITE>(e->arg(0), t, f);
    m_flatPin.push_back(e);
    m_flatDeepMemo.emplace(e.get(), res);
    return res;
  }

  /// Equal terms modulo nodes: the same term, or the same old term (a node
  /// with a promise and the same address without one, `&t->count` inbounds
  /// and a ptr.add, and the reads at them; PtrIndex.md cmp.refl-mod-flags).
  bool sameAddr(const Expr &a, const Expr &b) const {
    if (a == b || canon(a) == canon(b))
      return true;
    // canon equates nodes only; the erased forms (S2a's test: the flat
    // bit-vector sums) also equate a node with a plain sum of the same
    // value, which canon misses for a node whose index is itself a loaded
    // value (NKI gather: obj + 3*sext(idx) + 1 as a node vs a flat sum;
    // S2b took the test out, and gather's 0 Ackermann constraints became
    // 8 Ackermann constraints). Only where canon says no (counted
    // cmp.refl-erase). a separate eraser: erasing with the boundary's (m_old)
    // during the rewrite fills its memo early, and the boundary's recording
    // hooks (accessOrigins, the eq-audit's memop items) then miss those terms
    auto cmpErase = [this](const Expr &e) {
      return m_cmpOld.has(e) ? m_cmpOld.expand(e) : e;
    };
    // the flat forms run normalization rules: their counts go to a scratch
    // set, not into the firing being made (its rule list is the trail's)
    RewriteStats scratch;
    std::swap(scratch, m_stats);
    const bool same = cmpErase(a) == cmpErase(b);
    std::swap(scratch, m_stats);
    if (same) {
      ++m_stats.rules["cmp.refl-erase"]; // bookkeeping: not in the trail
      return true;
    }
    return false;
  }

  /// The canonical form of a term for sameAddr (docs/design/PtrSort.md):
  /// every node, at any depth, under the unflagged operator (a promise
  /// does not change the value), a chain of nodes merged into one over the
  /// innermost base, numeral indices folded into c (mod 2^n), stride-0
  /// pairs dropped, K added into c, the pairs sorted by (index id, stride);
  /// a node with no pair left and c = 0 is its base. Equal canonical forms
  /// are equal values (not a provenance claim: markers are kept). This
  /// comparison is restricted to nodes; a plain bvadd and a node at
  /// the same address are not equated.
  /// Memoized over the rewriter's life; iterative (memory chains are deep).
  Expr canon(const Expr &e) const {
    if (e->arity() == 0)
      return e;
    auto known = m_canonMemo.find(e.get());
    if (known != m_canonMemo.end())
      return known->second;
    llvm::SmallVector<std::pair<Expr, bool>, 32> stack;
    stack.push_back({e, false});
    while (!stack.empty()) {
      auto [p, expanded] = stack.back();
      stack.pop_back();
      if (m_canonMemo.count(p.get()))
        continue;
      if (p->arity() == 0) {
        m_canonPin.push_back(p);
        m_canonMemo.emplace(p.get(), p);
        continue;
      }
      if (!expanded) {
        stack.push_back({p, true});
        for (size_t i = 0; i < p->arity(); ++i)
          if (!m_canonMemo.count(p->arg(i)))
            stack.push_back({p->arg(i), false});
        continue;
      }
      ExprVector kids;
      bool changed = false;
      for (size_t i = 0; i < p->arity(); ++i) {
        kids.push_back(m_canonMemo.find(p->arg(i))->second);
        changed = changed || kids.back() != p->arg(i);
      }
      Expr res = changed ? p->efac().mkNary(p->op(), kids) : p;
      if (op::ptr::isPtrIndex(res))
        res = canonNode(res);
      else if (isOpX<BADD>(res))
        res = canonSum(res);
      m_canonPin.push_back(p);
      m_canonPin.push_back(res);
      m_canonMemo.emplace(p.get(), res);
    }
    return m_canonMemo.find(e.get())->second;
  }

  /// canon of a bvadd whose kids are canonical: a pointer plus integers
  /// (exactly one addend a registered base or a node) is the node it
  /// equals, base + sum x_k * s_k + c (a product by a numeral is the pair
  /// (x, s), any other addend (x, 1), numerals into c), so a flat sum
  /// (ptrindex.unflagged-flat, a ptr.add) and a node at the same address
  /// are equated (string_new_from_string: `obj + 8` and the inbounds
  /// node(obj, c = 8), the S2b gate). Any other sum as it is.
  Expr canonSum(const Expr &e) const {
    Expr ptr;
    for (size_t i = 0; i < e->arity(); ++i) {
      const Expr &a = e->arg(i);
      if (m_bases.isBase(a) || op::ptr::isPtrIndex(a)) {
        if (ptr)
          return e;
        ptr = a;
      }
    }
    if (!ptr)
      return e;
    std::vector<std::pair<Expr, mpz_class>> idx;
    mpz_class c = 0;
    for (size_t i = 0; i < e->arity(); ++i) {
      const Expr &a = e->arg(i);
      unsigned aw;
      if (a == ptr)
        continue;
      if (op::bv::isBvNum(a, aw)) {
        c = c + numVal(a);
      } else if (isOpX<BMUL>(a) && a->arity() == 2 &&
                 op::bv::isBvNum(a->arg(1), aw)) {
        idx.push_back({a->arg(0), numVal(a->arg(1))});
      } else if (isOpX<BMUL>(a) && a->arity() == 2 &&
                 op::bv::isBvNum(a->arg(0), aw)) {
        idx.push_back({a->arg(1), numVal(a->arg(0))});
      } else {
        idx.push_back({a, mpz_class(1L)});
      }
    }
    return canonNode(op::ptr::ptrIndex(op::PtrOpKind::PTR_INDEX, ptr, idx, c,
                                       mpz_class(0L)));
  }

  /// canon of one node whose kids are canonical (see canon).
  Expr canonNode(const Expr &e) const {
    using namespace op;
    Expr b = ptr::base(e);
    unsigned n = widthOf(b);
    std::vector<std::pair<Expr, mpz_class>> idx;
    mpz_class c = ptr::constant(e) + ptr::byteOffset(e);
    if (ptr::isPtrIndex(b)) { // canonical: unflagged, K = 0
      for (size_t k = 0; k < ptr::numIndices(b); ++k)
        idx.push_back({ptr::index(b, k), ptr::stride(b, k)});
      c = c + ptr::constant(b);
      b = ptr::base(b);
    }
    for (size_t k = 0; k < ptr::numIndices(e); ++k) {
      Expr x = ptr::index(e, k);
      mpz_class st = modPow2(ptr::stride(e, k), n);
      unsigned xw;
      if (bv::isBvNum(x, xw))
        c = c + st * numVal(x);
      else if (st != 0)
        idx.push_back({x, st});
    }
    c = modPow2(c, n);
    if (idx.empty() && c == 0)
      return b;
    std::sort(idx.begin(), idx.end(), [](const auto &l, const auto &r) {
      if (l.first->getId() != r.first->getId())
        return l.first->getId() < r.first->getId();
      return l.second < r.second;
    });
    return ptr::ptrIndex(PtrOpKind::PTR_INDEX, b, idx, c, mpz_class(0L));
  }

  /// \p e with every node, at any depth, replaced by its flat form, and
  /// every marker by its argument: the term the boundary gives
  /// (SeaMRewriter::expandPtrIndex shares this memo). Since PtrSort S2a the
  /// deciders read their arguments as they are (nodes, markers: view, ARM,
  /// the OFFSET cancellation); erase() is read only by sameAddr (until
  /// S2b's canon), congruentStarts (a linear form; the eraser as a decider
  /// input builds no VC term) and the read record. \p e itself if it holds
  /// no node.
  Expr erase(const Expr &e) const { return m_old.has(e) ? m_old.expand(e) : e; }
  PtrIndexExpander &eraser() const { return m_old; }

  /// The rule of a node (kids normal):
  ///  - ptrindex.fold: an index that is a numeral v folds into c, c' = c +
  ///    s*v (v signed for nusw, unsigned for nuw); the operator is kept if
  ///    c' fits n bits (signed / unsigned), else the node is unflagged
  ///    (ptrindex.fold-dropped-flags). No assumption.
  ///  - an unflagged node with no index left is its flat form (step (a):
  ///    ptr.add / addr.add stay bvadd). RW_DONE (the flat form of normal
  ///    kids is normal unless the pull below applies, checked first).
  ///  - ptrindex.pull-ite: exactly when add.pull-ite would pull the flat
  ///    form (same order, same bound, same pointer test), through the same
  ///    ite: if it is the base, or the index of a stride-1 index (parity,
  ///    parity rule), the node is pushed into the branches with its operator,
  ///    ite(g, P(p, ...), P(q, ...)) (exact; each leaf's promise is about
  ///    that leaf under its guard, PtrIndex.md); otherwise the flat form
  ///    is pulled as add.pull-ite does (ptrindex.pull-ite-flat, the node is
  ///    lost). RW_1: the ite at level 0, the new leaves at level 1.
  RewriteResult ptrIndex(const Expr &exp) {
    using namespace op;
    Expr e = exp;
    unsigned n = widthOf(ptr::base(e));
    if (m_opts.simplify) {
      std::vector<std::pair<Expr, mpz_class>> idx;
      mpz_class c = ptr::constant(e);
      bool folded = false;
      const bool nuw = isOpX<PTR_INDEX_NUW>(e);
      for (size_t k = 0; k < ptr::numIndices(e); ++k) {
        Expr x = ptr::index(e, k);
        unsigned xw;
        if (bv::isBvNum(x, xw)) {
          mpz_class v = numVal(x);
          if (!nuw && v >= pow2(n - 1))
            v = v - pow2(n);
          c = c + ptr::stride(e, k) * v;
          folded = true;
        } else {
          idx.push_back({x, ptr::stride(e, k)});
        }
      }
      if (folded) {
        m_stats.count("ptrindex.fold");
        PtrOpKind kind = isOpX<PTR_INDEX_NUSW>(e) ? PtrOpKind::PTR_INDEX_NUSW
                         : nuw                    ? PtrOpKind::PTR_INDEX_NUW
                                                  : PtrOpKind::PTR_INDEX;
        bool fits = kind == PtrOpKind::PTR_INDEX_NUSW
                        ? c >= mpz_class(0L) - pow2(n - 1) && c < pow2(n - 1)
                        : c >= mpz_class(0L) && c < pow2(n);
        if (kind == PtrOpKind::PTR_INDEX) {
          c = modPow2(c, n);
        } else if (!fits) {
          m_stats.count("ptrindex.fold-dropped-flags");
          kind = PtrOpKind::PTR_INDEX;
          c = modPow2(c, n);
        }
        e = ptr::ptrIndex(kind, ptr::base(e), idx, c, ptr::byteOffset(e));
      }
    }
    // ptrindex.base-num (S2c): an unflagged node over a sum with a numeral
    // takes the numeral into c (the same value; a pointer built as a flat
    // sum minus a constant, sub.const, is one node over its base)
    if (!ptr::isFlagged(e.get()) && isOpX<BADD>(ptr::base(e))) {
      const Expr b = ptr::base(e);
      llvm::SmallVector<Expr, 4> rest;
      mpz_class bc = 0;
      bool hasNum = false;
      for (size_t i = 0; i < b->arity(); ++i) {
        unsigned bw;
        if (bv::isBvNum(b->arg(i), bw)) {
          bc = bc + numVal(b->arg(i));
          hasNum = true;
        } else {
          rest.push_back(b->arg(i));
        }
      }
      if (hasNum && !rest.empty()) {
        std::vector<std::pair<Expr, mpz_class>> idx;
        for (size_t k = 0; k < ptr::numIndices(e); ++k)
          idx.push_back({ptr::index(e, k), ptr::stride(e, k)});
        m_stats.count("ptrindex.base-num");
        e = ptr::ptrIndex(PtrOpKind::PTR_INDEX, sumOf(rest, n), idx,
                          modPow2(ptr::constant(e) + bc, n),
                          ptr::byteOffset(e));
      }
    }
    // ptrindex.merge (PtrSort S2b): an unflagged node over an unflagged
    // node is one node, the pairs inner first, c and K summed (the same
    // value and the same erased sum; no promise is lost: neither has one)
    if (m_opts.simplify && !ptr::isFlagged(e.get()) &&
        ptr::isPtrIndex(ptr::base(e)) && !ptr::isFlagged(ptr::base(e).get())) {
      const Expr in = ptr::base(e);
      std::vector<std::pair<Expr, mpz_class>> idx;
      for (size_t k = 0; k < ptr::numIndices(in); ++k)
        idx.push_back({ptr::index(in, k), ptr::stride(in, k)});
      for (size_t k = 0; k < ptr::numIndices(e); ++k)
        idx.push_back({ptr::index(e, k), ptr::stride(e, k)});
      m_stats.count("ptrindex.merge");
      e = ptr::ptrIndex(PtrOpKind::PTR_INDEX, ptr::base(in), idx,
                        modPow2(ptr::constant(in) + ptr::constant(e), n),
                        modPow2(ptr::byteOffset(in) + ptr::byteOffset(e), n));
    }
    if (std::optional<RewriteResult> r = pullIteNode(e))
      return *r;
    // ptrindex.trivial (S2c): an unflagged node that adds nothing is its
    // base; any other node stays a node, at every position
    if (!ptr::isFlagged(e.get()) && ptr::numIndices(e) == 0 &&
        modPow2(ptr::constant(e) + ptr::byteOffset(e), n) == 0) {
      m_stats.count("ptrindex.trivial");
      return {ptr::base(e), RWStatus::RW_DONE};
    }
    return {e, RWStatus::RW_DONE};
  }

  static mpz_class pow2(unsigned k) {
    mpz_class r;
    mpz_ui_pow_ui(r.get_mpz_t(), 2, k);
    return r;
  }

  /// R-STRIDE (PtrIndex.md section 0c): is the entry
  /// at b + o (o the store-map's numeral offset) never the address idx?
  /// Every ite leaf of idx (at most maxDecideLeaves) must be a promised node
  /// N = PTR_INDEX_F(b, x_k, s_k, c, K) directly over b with an index, and:
  ///  - b + o < 2^n as integers, b the layout numeral (so the entry address
  ///    is exactly b + o);
  ///  - by P3 of N, b + O is an integer in [0, 2^n); for K != 0, with
  ///    b + size <= 2^n and b + size - 2^n <= K <= b, A-INB of the access
  ///    (its address in [b, b + size)) excludes both wraps of b + O + K
  ///    (PtrIndex.md section 0c): N's address is the integer b + O + K, so
  ///    it equals b + o iff O + K = o, i.e. sum x_k s_k = o - c - K. A-INB
  ///    alone does not give this for other K: a program ptr.add writes K
  ///    signed and unbounded (op::ptr::addressPlus);
  ///  - no integer solution if g = gcd(|s_k|) does not divide o - c - K.
  /// \p prem gets PremPtrIndex and PremLayout (the numeral b: for o >=
  /// 2^(n-1), P2 alone does not exclude the wrapped solution), and PremAInb
  /// if some K != 0.
  bool strideApartEntry(const Expr &b, const mpz_class &o, const Expr &idx,
                        unsigned &prem) const {
    const ObjectBase *ob = m_bases.lookup(b);
    if (!ob)
      return false;
    unsigned n = ob->base.getBitWidth();
    // the layout numeral, unsigned (toMpz reads an APInt as signed)
    mpz_class bv = toMpz(ob->base.zext(n + 1));
    if (bv + o >= pow2(n))
      return false;
    // the K range in which A-INB excludes a wrap of b + O + K (empty when
    // the object does not end within the space)
    const mpz_class size = toMpz(ob->size.zext(ob->size.getBitWidth() + 1));
    const mpz_class kLo = bv + size - pow2(n), kHi = bv;
    llvm::SmallVector<Expr, 8> stack{idx};
    unsigned leaves = 0;
    unsigned p = PremPtrIndex | PremLayout;
    while (!stack.empty()) {
      Expr l = stack.pop_back_val();
      if (isOpX<ITE>(l)) {
        stack.push_back(l->arg(1));
        stack.push_back(l->arg(2));
        continue;
      }
      if (++leaves > m_opts.maxDecideLeaves || !op::ptr::isFlagged(l.get()) ||
          op::ptr::base(l) != b || op::ptr::numIndices(l) == 0)
        return false;
      std::map<ENode *, mpz_class> coef;
      for (size_t k = 0; k < op::ptr::numIndices(l); ++k) {
        mpz_class &c = coef[op::ptr::index(l, k).get()];
        c = c + op::ptr::stride(l, k);
      }
      mpz_class g = 0L;
      for (const auto &[x, c] : coef)
        mpz_gcd(g.get_mpz_t(), g.get_mpz_t(), c.get_mpz_t());
      mpz_class d = o - op::ptr::constant(l) - op::ptr::byteOffset(l);
      if (g == mpz_class(0L) || mpz_divisible_p(d.get_mpz_t(), g.get_mpz_t()))
        return false;
      const mpz_class &k = op::ptr::byteOffset(l);
      if (k != mpz_class(0L)) {
        if (kLo > mpz_class(0L) || k < kLo || k > kHi)
          return false;
        p |= PremAInb;
      }
    }
    if ((p & PremAInb) && !m_opts.assumeInBounds)
      return false;
    prem = p;
    return true;
  }

  /// The promises a firing at \p lhs (a select or wselect) may use, as one
  /// formula over its read address: at a flagged leaf its promise, at
  /// another leaf true, at ite(c, x, y) ite(c, P(x), P(y)) (on the path
  /// where a leaf is selected only its promise is a fact, PtrIndex.md
  /// section 8). Null if no leaf is flagged.
  Expr accessPtrIndexPremise(const Expr &lhs) const {
    if (!isOpX<SELECT>(lhs) && !isOpX<W_SELECT>(lhs))
      return Expr();
    // iterative over the ite DAG, once per node (as inBoundsTree)
    std::unordered_map<Expr, Expr> memo;
    llvm::SmallVector<std::pair<Expr, bool>, 32> stack;
    stack.push_back({lhs->arg(1), false});
    bool flagged = false;
    while (!stack.empty()) {
      auto [p, expanded] = stack.back();
      stack.pop_back();
      if (memo.count(p))
        continue;
      if (!isOpX<ITE>(p)) {
        const bool f = op::ptr::isFlagged(p.get());
        flagged |= f;
        memo.emplace(p, f ? op::ptr::promise(p, widthOf(op::ptr::base(p)))
                          : Expr(mk<TRUE>(m_efac)));
        continue;
      }
      if (!expanded) {
        stack.push_back({p, true});
        for (unsigned i : {2u, 1u})
          if (!memo.count(p->arg(i)))
            stack.push_back({p->arg(i), false});
        continue;
      }
      const Expr &t = memo.find(p->arg(1))->second;
      const Expr &e = memo.find(p->arg(2))->second;
      memo.emplace(p, iteCond(p->arg(0), t, e));
    }
    return flagged ? memo.find(lhs->arg(1))->second : Expr();
  }

  /// The source address s + k of a read through a copy: a node
  /// (op::ptr::addressPlus; for a numeral k at a promised node s, s with
  /// K + k, the stride rule's K writers, PtrIndex.md section 4).
  Expr copySource(const Expr &s, const Expr &k) const {
    return op::ptr::addressPlus(s, k);
  }

  /// ptrindex.pull-ite (see ptrIndex): the condition of add.pull-ite on the
  /// flat form of \p e.
  std::optional<RewriteResult> pullIteNode(const Expr &e) {
    if (!m_opts.pullIte)
      return std::nullopt;
    if (std::optional<RewriteResult> r = pullIteParity(e))
      return r;
    if (m_opts.pullIteIndex)
      return pullIteAny(e);
    return std::nullopt;
  }

  /// The lifted pull (SeaMOptions::pullIteIndex): the node into the
  /// branches of its first ite, the base first, then the indices in order
  /// (any stride: ite(c, a, b) * s is ite(c, a * s, b * s)), when the
  /// product of the leaf counts of all its ites is within maxPullLeaves.
  /// Exact, no assumption. RW_1: the ite at level 0, the two nodes at 1.
  std::optional<RewriteResult> pullIteAny(const Expr &e) {
    using namespace op;
    const Expr b = ptr::base(e);
    unsigned prod = 1;
    int at = -2; // -1: the base; j >= 0: index j
    if (isOpX<ITE>(b) && !m_bases.lookup(b)) {
      prod *= leafCount(b, m_opts.maxPullLeaves);
      at = -1;
    }
    for (size_t j = 0; j < ptr::numIndices(e); ++j) {
      const Expr x = ptr::index(e, j);
      if (!isOpX<ITE>(x))
        continue;
      prod *= leafCount(x, m_opts.maxPullLeaves);
      if (at == -2)
        at = (int)j;
    }
    if (at == -2 || prod > m_opts.maxPullLeaves)
      return std::nullopt;
    auto rebuilt = [&](const Expr &kid) {
      ExprVector kids(e->args_begin(), e->args_end());
      kids[at < 0 ? 0 : 1 + 2 * at] = kid;
      return e->efac().mkNary(e->op(), kids);
    };
    const Expr it = at < 0 ? b : ptr::index(e, at);
    m_stats.count("ptrindex.pull-ite-index");
    return RewriteResult{
        mk<ITE>(it->arg(0), rebuilt(it->arg(1)), rebuilt(it->arg(2))),
        RWStatus::RW_1};
  }

  /// The parity rule: the pull add.pull-ite made on the flat form.
  std::optional<RewriteResult> pullIteParity(const Expr &e) {
    llvm::SmallVector<Expr, 8> sumTerms;
    sortedSummands(e, sumTerms);
    if (sumTerms.size() <= 1)
      return std::nullopt;
    unsigned prod = 1;
    size_t k = sumTerms.size();
    for (size_t i = 0; i < sumTerms.size(); ++i)
      if (isOpX<ITE>(sumTerms[i])) {
        if (k == sumTerms.size())
          k = i;
        prod *= leafCount(sumTerms[i], m_opts.maxPullLeaves);
        if (prod > m_opts.maxPullLeaves)
          break;
      }
    if (k == sumTerms.size() || prod > m_opts.maxPullLeaves)
      return std::nullopt;
    int budget = 16;
    bool pointer = false;
    for (const Expr &t : sumTerms)
      pointer = pointer || ptrish(t, budget);
    if (!pointer)
      return std::nullopt;
    const Expr ite = sumTerms[k];
    using namespace op;
    // the node into the branches, keeping its operator
    auto rebuilt = [&](const Expr &b, size_t at, const Expr &x) {
      ExprVector kids(e->args_begin(), e->args_end());
      if (b)
        kids[0] = b;
      else
        kids[1 + 2 * at] = x;
      return e->efac().mkNary(e->op(), kids);
    };
    // the summand is flatDeep of the ite: the node's own ite (its leaves
    // may be nodes) is the one pushed into
    const Expr b = ptr::base(e);
    if (isOpX<ITE>(b) && flatDeep(b) == ite) {
      m_stats.count("ptrindex.pull-ite");
      return RewriteResult{mk<ITE>(b->arg(0), rebuilt(b->arg(1), 0, Expr()),
                                   rebuilt(b->arg(2), 0, Expr())),
                           RWStatus::RW_1};
    }
    unsigned n = widthOf(e);
    for (size_t j = 0; j < ptr::numIndices(e); ++j) {
      const Expr x = ptr::index(e, j);
      if (isOpX<ITE>(x) && flatDeep(x) == ite &&
          modPow2(ptr::stride(e, j), n) == 1) {
        m_stats.count("ptrindex.pull-ite");
        return RewriteResult{mk<ITE>(x->arg(0), rebuilt(Expr(), j, x->arg(1)),
                                     rebuilt(Expr(), j, x->arg(2))),
                             RWStatus::RW_1};
      }
    }
    // elsewhere (inside a bvadd base, a product): the node stays a node
    // (S2c: no flat pull; the ite is the solver's)
    return std::nullopt;
  }

  /// A term that may be a pointer: an object base, a bvadd with such an
  /// argument, or an ite with such a branch (looks at `budget` nodes at most;
  /// a false answer only means "not seen").
  bool ptrish(const Expr &e, int &budget) const {
    if (--budget < 0)
      return false;
    if (m_bases.isBase(e))
      return true;
    if (op::ptr::isPtrIndex(e))
      return true; // a node is a pointer (S2c)
    if (op::ptr::isObservation(e))
      return ptrish(e->arg(0), budget);
    if (isOpX<BADD>(e)) {
      for (size_t i = 0; i < e->arity(); ++i)
        if (ptrish(e->arg(i), budget))
          return true;
      return false;
    }
    if (isOpX<ITE>(e))
      return ptrish(e->arg(1), budget) || ptrish(e->arg(2), budget);
    return false;
  }

  /// Does `e` mention a registered base? Does not look into the value of a
  /// load (select) or a memory (store): what memory holds is not a syntactic
  /// fact (A-INB covers an access address that depends on it). Exact over
  /// the DAG: memoized per node (a property of the term), iterative. (A
  /// tree walk with a budget, answering true when exhausted, refused the
  /// view of a pointer whose offset is a large DAG: hash_table_eq, null + a
  /// 668-node bucket offset, so addr.prune-size0 could not see the null
  /// leaf.)
  bool mentionsBase(const Expr &e) const {
    auto known = m_mentionsMemo.find(e);
    if (known != m_mentionsMemo.end())
      return known->second;
    llvm::SmallVector<std::pair<Expr, bool>, 32> stack;
    stack.push_back({e, false});
    while (!stack.empty()) {
      auto [p, expanded] = stack.back();
      stack.pop_back();
      if (m_mentionsMemo.count(p))
        continue;
      if (m_bases.isBase(p)) {
        m_mentionsMemo.emplace(p, true);
        continue;
      }
      // a value read from memory is data (its address is not part of the
      // offset); the wide read too (--seam-wide), like SELECT
      if (isOpX<SELECT>(p) || isOpX<STORE>(p) || isOpX<W_SELECT>(p) ||
          isOpX<W_STORE>(p) || p->arity() == 0) {
        m_mentionsMemo.emplace(p, false);
        continue;
      }
      if (!expanded) {
        stack.push_back({p, true});
        for (size_t i = 0; i < p->arity(); ++i)
          if (!m_mentionsMemo.count(p->arg(i)))
            stack.push_back({p->arg(i), false});
        continue;
      }
      bool any = false;
      for (size_t i = 0; i < p->arity() && !any; ++i)
        any = m_mentionsMemo.find(p->arg(i))->second;
      m_mentionsMemo.emplace(p, any);
    }
    return m_mentionsMemo.find(e)->second;
  }

  /// Pointer view: `e` is obj.k, or bvadd of exactly one registered base and
  /// other terms (the offset), none of which mentions a registered base: a
  /// sum such as obj.a + (obj.b - obj.a) (integer casts) is the pointer
  /// obj.b, not a pointer into a. Needs a normalized \p e only for the
  /// offset to be a normal term.
  std::optional<PtrView> view(const Expr &e0) const {
    // a node is read through its flat form, a bvadd a rule built over one
    // (splitRead's j + k) too. Shallow: the offset keeps the nodes of the
    // loads in it (rules build terms from it: sub.same-base, the compare
    // reduction); a node deep in the offset stays (PtrSort S2a)
    if (op::ptr::isObservation(e0)) {
      // the address of a pointer is read as the pointer (S1.6c)
      m_stats.rules["ptr.to-int-view"]++;
      return view(op::ptr::stripObservation(e0));
    }
    Expr e = flatDeep(e0);
    if (m_bases.isBase(e))
      return PtrView{e, num(0, widthOf(e))};
    if (!isOpX<BADD>(e))
      return std::nullopt;
    Expr base;
    llvm::SmallVector<Expr, 4> rest;
    bool viaNode = false;
    for (size_t i = 0; i < e->arity(); ++i) {
      const Expr &a = e->arg(i);
      // a base through an observation marker (to_int(obj) + k, S1.6c)
      const Expr ab = op::ptr::stripObservation(a);
      if (m_bases.isBase(ab)) {
        if (base)
          return std::nullopt; // two bases: not a pointer plus an offset
        base = ab;
      } else if (op::ptr::isPtrIndex(ab)) {
        // a node in an integer sum (to_int(p + k) + j, S2c): its view's
        // base, its offset into the rest
        std::optional<PtrView> v = view(ab);
        if (!v || base)
          return std::nullopt;
        base = v->base;
        rest.push_back(offSum(v->off));
        viaNode = true;
      } else {
        rest.push_back(a);
      }
    }
    if (!base)
      return std::nullopt;
    for (const Expr &r : rest)
      if (mentionsBase(r))
        return std::nullopt;
    if (viaNode) {
      // the offset as the add rule normalizes a sum: summands flattened,
      // numerals summed last, the others sorted (bv.ac order)
      unsigned w = widthOf(base);
      llvm::SmallVector<Expr, 8> terms;
      mpz_class c = 0;
      for (const Expr &r : rest) {
        llvm::SmallVector<Expr, 4> parts;
        if (isOpX<BADD>(r))
          parts.append(r->args_begin(), r->args_end());
        else
          parts.push_back(r);
        for (const Expr &t : parts) {
          unsigned tw;
          if (op::bv::isBvNum(t, tw))
            c = c + numVal(t);
          else
            terms.push_back(t);
        }
      }
      if (m_opts.simplify)
        m_bv.sortSum(terms);
      c = modPow2(c, w);
      if (c != 0)
        terms.push_back(num(c, w));
      m_stats.rules["view.offset-sum"]++;
      return PtrView{base, sumOf(terms, w)};
    }
    Expr sum = sumOf(rest, widthOf(base));
    unsigned nw;
    if (op::bv::isBvNum(sum, nw))
      return PtrView{base, sum}; // A4: a numeral offset stays a numeral
    if (Expr off = structuralOffset(e0, base, sum)) {
      m_stats.rules["view.offset-node"]++;
      return PtrView{base, off};
    }
    m_stats.rules["view.offset-sum"]++;
    return PtrView{base, sum};
  }

  /// The OFFSET of a node chain PTR_INDEX*(... PTR_INDEX*(base, I_1, ...)
  /// ..., I_n, ...) over the object \p base (S1.4, PtrSort.md B1): the pairs
  /// innermost first, c and K summed. Null when \p e is not such a chain.
  /// Its expansion is \p sum, the offset view() took from the flat form.
  Expr structuralOffset(const Expr &e, const Expr &base,
                        [[maybe_unused]] const Expr &sum) const {
    llvm::SmallVector<Expr, 4> chain;
    Expr p = e;
    while (op::ptr::isPtrIndex(p)) {
      chain.push_back(p);
      p = op::ptr::base(p);
    }
    if (chain.empty() || p != base)
      return Expr();
    unsigned n = widthOf(base);
    std::vector<std::pair<Expr, mpz_class>> idx;
    mpz_class c = 0, K = 0;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      for (size_t k = 0; k < op::ptr::numIndices(*it); ++k)
        idx.push_back({op::ptr::index(*it, k), op::ptr::stride(*it, k)});
      c = c + op::ptr::constant(*it);
      K = K + op::ptr::byteOffset(*it);
    }
    if (idx.empty())
      return Expr();
    Expr off = op::ptr::offset(idx, modPow2(c, n), modPow2(K, n));
    assert(offSum(off) == sum &&
           "view: the OFFSET does not expand to the flat offset");
    return off;
  }

  /// The integer term of a view's offset (rule `offset.flat`): an OFFSET as
  /// the normalized sum the flat form of its node has without the base (the
  /// summands of each x_k * s_k, then c + K, ordered as sortedSummands
  /// orders them); any other offset as it is. Memoized per OFFSET. No side
  /// effect: the products are normalized by a scratch bvnorm::Rules, so
  /// neither the counters nor the trail of the firing that asks see it (the
  /// audit matches a firing by its rules: eq-audit.mlir).
  Expr offSum(const Expr &off) const {
    if (!isOpX<OFFSET>(off))
      return off;
    auto it = m_flatMemo.find(off.get());
    if (it != m_flatMemo.end())
      return it->second;
    unsigned n = widthOf(off);
    llvm::SmallVector<Expr, 8> terms;
    mpz_class sum = 0;
    for (size_t k = 0; k < op::ptr::offsetIndices(off); ++k)
      addrSummands(indexSummand(op::ptr::offsetIndex(off, k),
                                op::ptr::offsetStride(off, k), n, &m_scratchBv),
                   terms, sum, &m_scratchBv);
    sum = modPow2(sum + getTerm<mpz_class>(off->arg(off->arity() - 2)) +
                      getTerm<mpz_class>(off->arg(off->arity() - 1)),
                  n);
    if (m_opts.simplify)
      m_bv.sortSum(terms);
    else
      std::stable_partition(terms.begin(), terms.end(),
                            [&](const Expr &t) { return m_bases.isBase(t); });
    if (sum != 0)
      terms.push_back(num(sum, n));
    Expr res = sumOf(terms, n);
    m_flatPin.push_back(off);
    m_flatMemo.emplace(off.get(), res);
    return res;
  }

  /// Two view offsets are equal: as terms, or as integer terms (an OFFSET
  /// and the sum of another path, two OFFSETs of one sum).
  bool sameOff(const Expr &a, const Expr &b) const {
    return a == b || offSum(a) == offSum(b);
  }

  //===------------------------------------------------------------------===//
  // A-INB, as a condition on an access
  //===------------------------------------------------------------------===//

  /// The condition under which an access of \p units address units at the
  /// normalized address \p addr stays in its object, per leaf of an ite
  /// (A-INB, see the header): `off <= size - units` for a leaf that is a
  /// pointer into object k with offset `off`; true for a leaf that is not
  /// (A-INB says nothing about it); false for a leaf that cannot hold the
  /// access (a numeral offset out of bounds, an object smaller than the
  /// access: every leaf in the null object or another object of size 0).
  /// Guarded aggregation (docs/design/SeaM-null.md): an
  /// impossible leaf is `false` in the condition, and only the access as a
  /// whole is judged: \p violated is set iff the condition is `false` (every
  /// alternative impossible, syntactically; a definite violation, which
  /// sea-vc answers by running without A-INB). A mixed `ite(c, null, A + i)`
  /// has the satisfiable condition `ite(c, false, i <= size_A - units)`.
  Expr inBounds(const Expr &addr, uint64_t units, bool &violated) const {
    // on the term as it is: a leaf node by its view (or ARM's node case);
    // the condition holds nodes and markers, erased at the boundary
    Expr res = inBoundsTree(addr, units);
    if (isOpX<FALSE>(res))
      violated = true;
    return res;
  }

  /// Why an access judged violated by inBounds (\p range false) or
  /// rangeInBounds (\p range true, \p n the length) cannot stay in its
  /// object: the first leaf of the address's ite tree, described (which
  /// check failed: numeral offset outside, object smaller than the access /
  /// of size 0, ARM range outside); "every one of K alternatives" when the
  /// address is an ite. For messages only.
  std::string explainViolation(const Expr &addr, uint64_t units, bool range,
                               const Expr &n = Expr()) const {
    llvm::SmallVector<Expr, 8> leaves;
    llvm::SmallVector<Expr, 32> stack{addr};
    std::unordered_set<Expr> seen;
    while (!stack.empty()) {
      Expr p = stack.pop_back_val();
      if (!seen.insert(p).second)
        continue;
      if (isOpX<ITE>(p)) {
        stack.push_back(p->arg(2));
        stack.push_back(p->arg(1));
        continue;
      }
      leaves.push_back(p);
    }
    std::string s;
    llvm::raw_string_ostream os(s);
    if (leaves.size() > 1)
      os << "every one of " << leaves.size() << " alternatives; first: ";
    const Expr &leaf = leaves.front();
    std::optional<PtrView> v = view(leaf);
    const ObjectBase *o = v ? m_bases.lookup(v->base) : nullptr;
    if (!o) {
      os << (range ? "range"
                   : "access of " + std::to_string(units) + " unit(s)")
         << ": the address range (ARM) lies outside its object";
      return os.str();
    }
    std::string size = std::to_string(o->size.getLimitedValue());
    unsigned w;
    // a numeral offset, signed (an offset below the base is negative)
    std::string off;
    if (op::bv::isBvNum(v->off, w)) {
      mpz_class x = numVal(v->off), half = mpz_class(1UL) << (w - 1);
      off = x >= half ? "-" + mpz_class((half << 1) - x).to_string()
                      : x.to_string();
    }
    if (range) {
      std::string len =
          n && op::bv::isBvNum(n, w) ? numVal(n).to_string() : std::string("n");
      if (o->size.isZero())
        os << "range of length " << len << " in object " << o->name
           << " of size 0";
      else if (!off.empty())
        os << "range outside: [" << off << ", " << off << " + " << len
           << ") not in object " << o->name << " of size " << size;
      else
        os << "range outside: length " << len << " > size " << size
           << " of object " << o->name;
      return os.str();
    }
    if (o->size.isZero())
      os << "access of " << units << " unit(s) in object " << o->name
         << " of size 0";
    else if (o->size.getLimitedValue() < units)
      os << "access of " << units << " unit(s) > size " << size << " of object "
         << o->name;
    else
      os << "numeral offset outside: offset " << (off.empty() ? "?" : off)
         << " + " << units << " unit(s) > size " << size << " of object "
         << o->name;
    return os.str();
  }

  /// ite(c, t, e) for conditions, with ite(c, t, t) = t.
  static Expr iteCond(const Expr &c, const Expr &t, const Expr &e) {
    return t == e ? t : mk<ITE>(c, t, e);
  }

  /// Is \p base the base of a registered object of size 0 (null, a
  /// zero-sized allocation)? No access of positive width is in such an
  /// object (A-INB is false for it).
  bool size0(const Expr &base) const {
    const ObjectBase *o = m_bases.lookup(base);
    return o && o->size.isZero();
  }

  /// addr.prune-size0 (docs/design/SeaM-provenance.md): \p addr
  /// without its syntactic ite leaves (ites that are not registered bases)
  /// whose pointer view is in an object of size 0 (null, a zero-sized
  /// allocation); the guards of the other leaves are kept. Null if every
  /// leaf is such; \p addr itself if none is. A numeral leaf, or a leaf
  /// without a pointer view (an ite beneath an addition), is kept. Valid only
  /// at an access position, under that access's A-INB (inBounds /
  /// rangeInBounds of \p addr, which make a size-0 leaf false or n = 0):
  /// never for the address term itself (a compare, a stored pointer).
  /// Iterative over the ite DAG.
  Expr pruneSize0(const Expr &addr) const {
    if (!isOpX<ITE>(addr) || m_bases.lookup(addr))
      return addr;
    std::unordered_map<Expr, Expr> memo; // null: every leaf pruned
    llvm::SmallVector<std::pair<Expr, bool>, 16> stack;
    stack.push_back({addr, false});
    while (!stack.empty()) {
      auto [p, expanded] = stack.back();
      stack.pop_back();
      if (memo.count(p))
        continue;
      if (!isOpX<ITE>(p) || m_bases.lookup(p)) {
        std::optional<PtrView> v = view(p);
        memo.emplace(p, v && size0(v->base) ? Expr() : p);
        continue;
      }
      if (!expanded) {
        stack.push_back({p, true});
        for (unsigned i : {2u, 1u})
          if (!memo.count(p->arg(i)))
            stack.push_back({p->arg(i), false});
        continue;
      }
      const Expr t = memo.find(p->arg(1))->second;
      const Expr e = memo.find(p->arg(2))->second;
      Expr res;
      if (!t)
        res = e;
      else if (!e)
        res = t;
      else if (t == p->arg(1) && e == p->arg(2))
        res = p;
      else
        res = t == e ? t : Expr(mk<ITE>(p->arg(0), t, e));
      memo.emplace(p, res);
    }
    return memo.find(addr)->second;
  }

  /// Is \p mem a shadow memory (docs/design/SeaM-null.md, "Shadow
  /// memories")? A memory term built (stores, store-maps, ites, memory-
  /// operation terms) over a start the registry knows as a shadow root
  /// (BaseRegistry::addShadowRoot: `sea.mem.const ... shadow`). An ite with
  /// one shadow branch counts as shadow (no A-INB rule for it: the safe
  /// side). A property of the term, so the rewriter's cache keeps it right.
  /// Memoized; iterative (store chains are deep).
  bool shadowMemory(const Expr &mem) const {
    if (!m_bases.hasShadowRoots())
      return false;
    auto kids = [&](const Expr &e, llvm::SmallVectorImpl<Expr> &out) {
      if (isOpX<STORE>(e) || isOpX<STORE_MAP>(e) || isOpX<W_STORE>(e))
        out.push_back(e->arg(0));
      else if (isOpX<ITE>(e)) {
        out.push_back(e->arg(1));
        out.push_back(e->arg(2));
      } else if (isMemOpTerm(e)) {
        out.push_back(memOpBase(e));
      }
    };
    llvm::SmallVector<std::pair<Expr, bool>, 32> stack;
    stack.push_back({mem, false});
    while (!stack.empty()) {
      auto [e, expanded] = stack.back();
      stack.pop_back();
      if (m_shadowMemo.count(e))
        continue;
      llvm::SmallVector<Expr, 2> ks;
      kids(e, ks);
      if (ks.empty()) {
        m_shadowMemo.emplace(e, m_bases.isShadowRoot(e));
        continue;
      }
      if (!expanded) {
        stack.push_back({e, true});
        for (const Expr &k : ks)
          if (!m_shadowMemo.count(k))
            stack.push_back({k, false});
        continue;
      }
      bool sh = false;
      for (const Expr &k : ks)
        sh = sh || m_shadowMemo.find(k)->second;
      m_shadowMemo.emplace(e, sh);
    }
    return m_shadowMemo.find(mem)->second;
  }

  /// A shadow root was added to the registry: forget what shadowMemory knew.
  void forgetShadowMemo() const { m_shadowMemo.clear(); }

  /// inBounds without the verdict: the guarded condition over the ite tree.
  Expr inBoundsTree(const Expr &addr, uint64_t units) const {
    // iterative over the ite DAG (a VC has deep chains), once per node
    std::unordered_map<Expr, Expr> memo;
    llvm::SmallVector<std::pair<Expr, bool>, 32> stack;
    stack.push_back({addr, false});
    while (!stack.empty()) {
      auto [p, expanded] = stack.back();
      stack.pop_back();
      if (memo.count(p))
        continue;
      if (!isOpX<ITE>(p)) {
        memo.emplace(p, inBoundsLeaf(p, units));
        continue;
      }
      if (!expanded) {
        stack.push_back({p, true});
        for (unsigned i : {2u, 1u})
          if (!memo.count(p->arg(i)))
            stack.push_back({p->arg(i), false});
        continue;
      }
      const Expr &t = memo.find(p->arg(1))->second;
      const Expr &e = memo.find(p->arg(2))->second;
      memo.emplace(p, iteCond(p->arg(0), t, e));
    }
    return memo.find(addr)->second;
  }

  /// inBounds for an address that is not an ite: its condition, `false` if
  /// the leaf cannot hold the access (not a verdict on the whole access).
  Expr inBoundsLeaf(const Expr &addr, uint64_t units) const {
    std::optional<PtrView> v = view(addr);
    if (!v)
      return armInBounds(addr, units);
    const ObjectBase *o = m_bases.lookup(v->base);
    if (!o || o->size.getBitWidth() > 64)
      return mk<TRUE>(m_efac);
    uint64_t size = o->size.getZExtValue();
    if (size < units)
      return mk<FALSE>(m_efac);
    unsigned w = widthOf(v->base);
    mpz_class last = size - units;
    unsigned nw;
    if (op::bv::isBvNum(v->off, nw))
      return numVal(v->off) <= last ? mk<TRUE>(m_efac) : mk<FALSE>(m_efac);
    return mk<BULE>(offSum(v->off), num(last, w));
  }

  /// Does the ARM map of a leaf address without a pointer view have every
  /// base entry inside its object for an access of \p units (no A-INB
  /// condition is needed)? The map is cached, so this is cheap. An absolute
  /// entry says nothing (A-INB is about pointers); a top map makes no ARM
  /// claim (true: nothing to condition); a top interval of a base is not
  /// inside. A map with an entry wholly outside
  /// its object is not a verdict on the whole access: armInBounds makes that
  /// provenance `false`, guarded by the ites that select it (SeaM-null.md,
  /// guarded aggregation).
  bool armAllIn(const Expr &leaf, uint64_t units) const {
    if (!m_opts.arm)
      return true;
    const AddrRangeMap &m = m_arm.alpha(leaf);
    if (m.isTop())
      return true;
    for (const AddrRangeMap::Entry &e : m.entries()) {
      if (e.first == AddrRangeMap::kAbs)
        continue;
      bool out, in;
      entryBounds(m_bases.objects()[e.first], e.second, units, out, in);
      if (!in)
        return false;
    }
    return true;
  }

  /// One ARM entry (object \p o, offsets \p r): every offset of
  /// \p r out of the object for the access (\p out), or every one in it
  /// (\p in). An object smaller than the access: out.
  static void entryBounds(const ObjectBase &o, const AddrRange &r,
                          uint64_t units, bool &out, bool &in) {
    out = false;
    in = false;
    unsigned w = std::max(r.isRange() ? r.lo().getBitWidth() : 1u,
                          o.size.getBitWidth()) +
                 2;
    llvm::APInt size = o.size.zext(w), u(w, units);
    if (size.ult(u)) {
      out = true;
      return;
    }
    if (!r.isRange())
      return;
    llvm::APInt last = size - u;
    if (r.hi().sext(w).isNegative() || r.lo().sext(w).sgt(last))
      out = true;
    if (!(r.lo().sext(w).isNegative() || r.hi().sext(w).sgt(last)))
      in = true;
  }

  /// The ARM provenance of a bvadd: its one argument that has a bases; the
  /// others are offsets (alpha: only absolute entries). Null for two
  /// pointers, or an argument ARM gives up on (no claim).
  Expr armPtrArg(const Expr &p) const {
    Expr ptr;
    for (size_t i = 0; i < p->arity(); ++i) {
      const AddrRangeMap &m = m_arm.alpha(p->arg(i));
      if (m.isTop())
        return Expr();
      if (m.hasBases()) {
        if (ptr)
          return Expr();
        ptr = p->arg(i);
      }
    }
    return ptr;
  }

  /// The ARM provenance of a node (PtrSort S2a): its base, when ARM reads
  /// the base as a pointer and no index summand as one (as armPtrArg of the
  /// sum the node erases to: a stride-1 index is its own summand, any other
  /// a product, which ARM reads as top when it mentions a base). Null
  /// otherwise (no claim).
  Expr armNodeBase(const Expr &p) const {
    const AddrRangeMap &mb = m_arm.alpha(op::ptr::base(p));
    if (mb.isTop() || !mb.hasBases())
      return Expr();
    for (size_t k = 0; k < op::ptr::numIndices(p); ++k) {
      const Expr &x = op::ptr::index(p, k);
      mpz_class s = modPow2(op::ptr::stride(p, k), widthOf(x));
      if (s == 0)
        continue;
      if (s != 1) {
        if (m_arm.mentionsBase(x))
          return Expr();
        continue;
      }
      const AddrRangeMap &mx = m_arm.alpha(x);
      if (mx.isTop() || mx.hasBases())
        return Expr();
    }
    return op::ptr::base(p);
  }

  /// A-INB for a leaf address that has no pointer view but that ARM
  /// (AddrRangeMap.hh) reads as a pointer: a sum over an ite of pointers,
  /// `bvadd(ite(c, obj.a, obj.b), x)`. The ARM claim of the compare uses the
  /// provenance of each leaf of the term, the base of the bvadd under the ites
  /// that select it, so the access condition is per provenance, not "in one
  /// of the objects of the map" (a leaf of b that lands in a by luck would
  /// pass the latter, and the compare would be decided with the wrong base):
  ///   ite(c, in range of obj.a, in range of obj.b)   for the sum above
  /// each range check being `V - obj.k <= size_k - units` for the value V
  /// of the whole address. A part that ARM does not read as a pointer (an
  /// absolute number, top) has condition true: ARM then claims nothing for it
  /// (an absolute entry is never decided different from a base). Always
  /// built (so that the access counts as an assumed one, as in the view
  /// case), except when the map says every entry is inside its object. A leaf
  /// provenance that cannot hold the access is `false` (guarded aggregation,
  /// as in the view case): its object is smaller than the access (size 0:
  /// null), or the entry of its object in the map of the whole address is
  /// wholly outside it (every provenance-k leaf has its value in that entry's
  /// interval, so the range check is false there anyway). The condition is
  /// `false` only if every provenance is impossible and no part is absolute.
  /// Iterative: the ite chains of a VC are deep.
  Expr armInBounds(const Expr &addr, uint64_t units) const {
    if (!m_opts.arm || armAllIn(addr, units))
      return mk<TRUE>(m_efac);
    const AddrRangeMap &whole = m_arm.alpha(addr);
    std::unordered_map<Expr, Expr> memo;
    struct Frame {
      Expr p;
      bool expanded;
    };
    llvm::SmallVector<Frame, 32> stack;
    stack.push_back({addr, false});
    while (!stack.empty()) {
      Frame f = stack.back();
      stack.pop_back();
      const Expr &p = f.p;
      if (memo.count(p))
        continue;
      Expr kid0, kid1; // the sub-terms whose result p's depends on
      if (isOpX<ITE>(p) && !m_bases.lookup(p)) {
        kid0 = p->arg(1);
        kid1 = p->arg(2);
      } else if (isOpX<BADD>(p) && !m_bases.lookup(p)) {
        kid0 = armPtrArg(p);
      } else if (op::ptr::isObservation(p)) {
        kid0 = p->arg(0);
      } else if (op::ptr::isPtrIndex(p)) {
        kid0 = armNodeBase(p);
      }
      if (!f.expanded && (kid0 || kid1)) {
        stack.push_back({p, true});
        if (kid1 && !memo.count(kid1))
          stack.push_back({kid1, false});
        if (kid0 && !memo.count(kid0))
          stack.push_back({kid0, false});
        continue;
      }
      Expr res = mk<TRUE>(m_efac);
      if (const ObjectBase *o = m_bases.lookup(p)) {
        const AddrRange *r = whole.find(o->order);
        bool eout = false, ein = false;
        if (r)
          entryBounds(*o, *r, units, eout, ein);
        res = eout ? Expr(mk<FALSE>(m_efac)) : rangeCond(*o, addr, units);
      } else if (kid1) { // ite
        const Expr &t = memo.find(kid0)->second, &e = memo.find(kid1)->second;
        res = iteCond(p->arg(0), t, e);
      } else if (kid0) { // bvadd
        res = memo.find(kid0)->second;
      }
      memo.emplace(p, res);
    }
    return memo.find(addr)->second;
  }

  /// `value - obj.k <= size_k - units` (unsigned), false if the object is
  /// smaller than the access.
  Expr rangeCond(const ObjectBase &o, const Expr &value, uint64_t units) const {
    if (o.size.getBitWidth() > 64)
      return mk<TRUE>(m_efac);
    uint64_t size = o.size.getZExtValue();
    if (size < units)
      return mk<FALSE>(m_efac);
    unsigned w = widthOf(o.constant);
    // over the addresses (ADDR, S2d): erased, value - obj.k
    return mk<BULE>(mk<BSUB>(mk<ADDR>(value), mk<ADDR>(o.constant)),
                    num(size - units, w));
  }

  /// the access addresses a memory rule at \p lhs relies on when
  /// it uses A-INB: the address of the access (the read index of a select, the
  /// address of a store) and the addresses of the write directly below it (a
  /// store's address, every entry of a store-map). Every A-INB rule (ROW hit /
  /// skip and its compare tree, R-SKIP, R-ABORT, WOW-COMMUTE, WOSMAP-COMMUTE)
  /// compares exactly these. False for another term (the premise cannot be
  /// stated from the term).
  ///
  /// With width terms (docs/design/SeaM-mixed-width.md) each address comes
  /// with the span of its own access in address units: per-access width
  /// accounting. A select / store has \p ctxUnits (the cell, from the
  /// firing's context); a wselect has its result width, a wstore (the write
  /// below) its value width, in bytes; a store or store-map entry below a
  /// wselect is one byte (wide reads are only of byte memories).
  bool
  accessSpans(const Expr &lhs, uint64_t ctxUnits,
              llvm::SmallVectorImpl<std::pair<Expr, uint64_t>> &out) const {
    uint64_t readUnits;
    // a select / store moves one cell, whatever access the firing's context
    // is (a byte read made by the byte fallback runs in a wide load's
    // context): at most the cell's own bytes (address-unit memories keep the
    // context's units). A pointer-sorted cell (a pointer companion, a cell
    // per unit: its mirror discharges 1 unit, OpSem settleKeyed) is 1 unit,
    // whatever its width.
    auto cellSpan = [&](const Expr &cellValue) {
      if (!m_tc)
        m_tc = std::make_unique<TypeChecker>();
      if (isOpX<PTRSORT>(m_tc->typeOf(cellValue)))
        return uint64_t(1);
      uint64_t bytes = std::max<uint64_t>(1, widthOf(cellValue) / 8);
      return std::min<uint64_t>(ctxUnits, bytes);
    };
    if (isOpX<SELECT>(lhs))
      readUnits = cellSpan(lhs);
    else if (isOpX<STORE>(lhs))
      readUnits = cellSpan(lhs->arg(2));
    else if (isOpX<W_SELECT>(lhs))
      readUnits = op::bv::width(lhs->arg(2)) / 8;
    else
      return false;
    out.push_back({lhs->arg(1), readUnits});
    const uint64_t cellUnits = isOpX<W_SELECT>(lhs) ? 1 : readUnits;
    const Expr arr = lhs->arg(0);
    if (isOpX<STORE>(arr)) {
      out.push_back({arr->arg(1), cellUnits});
    } else if (isOpX<W_STORE>(arr)) {
      out.push_back({arr->arg(1), widthOf(arr->arg(2)) / 8});
    } else if (isOpX<STORE_MAP>(arr)) {
      for (const SMapEntry &e : StoreMapTable::entries(arr->arg(2)))
        out.push_back({ptrAt(arr->arg(1), e.offset), cellUnits});
    }
    return true;
  }

  /// The alignment promises a firing at \p lhs relies on (A-ALIGN): each
  /// width term's address with its own promised alignment (the read, the
  /// write below); only promises >= 2.
  void
  accessAligns(const Expr &lhs,
               llvm::SmallVectorImpl<std::pair<Expr, uint64_t>> &out) const {
    if (isOpX<W_SELECT>(lhs) && op::array::promisedAlign(lhs) >= 2)
      out.push_back({lhs->arg(1), op::array::promisedAlign(lhs)});
    if ((isOpX<W_SELECT>(lhs) || isOpX<SELECT>(lhs)) &&
        isOpX<W_STORE>(lhs->arg(0)) &&
        op::array::promisedAlign(lhs->arg(0)) >= 2)
      out.push_back(
          {lhs->arg(0)->arg(1), op::array::promisedAlign(lhs->arg(0))});
  }

  //===------------------------------------------------------------------===//
  // width terms (docs/design/SeaM-mixed-width.md rev 3)
  //===------------------------------------------------------------------===//

  /// How a read of r bytes at j relates to a write of w bytes at i. Super:
  /// the write is inside the read (w < r), at byte k of the read.
  enum class WRel { Unknown, Hit, Sub, Skip, Super };

  /// The relation of the read span [j, j + r) to the write span [i, i + w)
  /// (circular, modulo 2^n; widths within the domain 0 < r, w <= 2^n, as
  /// mathematical integers). \p k is the byte offset of the read in the
  /// write (Sub / Hit), or of the write in the read (Super). Exact for one
  /// base and numeral offsets (and for i == j); across objects (single pointer
  /// views, different registered bases) Skip under A-INB (whole spans in their
  /// objects), not in a shadow memory. Unknown otherwise (the term stays).
  WRel wideRelation(const Expr &i0, uint64_t w, const Expr &j0, uint64_t r,
                    uint64_t &k, Decision &d, bool shadow) const {
    const Expr i = op::ptr::stripObservation(i0),
               j = op::ptr::stripObservation(j0);
    if (sameAddr(i, j)) {
      k = 0;
      return r == w ? WRel::Hit : (r < w ? WRel::Sub : WRel::Super);
    }
    std::optional<PtrView> vi = view(i), vj = view(j);
    if (!vi || !vj)
      return WRel::Unknown;
    if (vi->base == vj->base) {
      mpz_class x, y;
      if (sameOff(vi->off, vj->off)) {
        k = 0;
        d.add("cmp.same-base");
        return r == w ? WRel::Hit : (r < w ? WRel::Sub : WRel::Super);
      }
      if (!numerals(vi->off, vj->off, x, y))
        return WRel::Unknown;
      unsigned n = widthOf(vi->off);
      mpz_class kk = modPow2(y - x, n), back = modPow2(x - y, n);
      d.add("cmp.same-base");
      if (kk + r <= w) {
        k = kk.get_ui();
        return k == 0 && r == w ? WRel::Hit : WRel::Sub;
      }
      if (kk >= w && back >= r)
        return WRel::Skip;
      if (back + w <= r) {
        k = back.get_ui();
        return WRel::Super;
      }
      return WRel::Unknown;
    }
    if (shadow || !m_opts.assumeInBounds || !m_bases.isBase(vi->base) ||
        !m_bases.isBase(vj->base))
      return WRel::Unknown;
    d.add("cmp.diff-base", PremAInb); // uses: A-INB (whole spans)
    checkInbPremise(i, w, j, r);
    return WRel::Skip;
  }

  /// The byte fallback (rev 4, measured: a width term left to the boundary
  /// is read there over the whole remaining chain with no SeaM rule; the
  /// hash_iter jobs went 0.7 -> 27 s): an undecided wide read becomes the
  /// concat of its byte reads at j + k over the same memory (the byte rules
  /// go on), an undecided byte read over a wstore reads the wstore's byte
  /// stores. Exact (the definition of the terms); the worst case is the old
  /// byte splitting.
  Expr splitRead(const Expr &mem, const Expr &j, uint64_t r) {
    unsigned n = widthOf(j);
    auto at = [&](uint64_t k) -> Expr {
      if (k == 0)
        return j;
      // a K-node at a promised node (addressPlus)
      return op::ptr::addressPlus(j, num(mpz_class((unsigned long)k), n));
    };
    Expr res = op::array::select(mem, at(0));
    for (uint64_t k = 1; k < r; ++k)
      res = op::bv::concat(op::array::select(mem, at(k)), res);
    return res;
  }
  Expr expandWrite(const Expr &wst) {
    const Expr i = wst->arg(1), v = wst->arg(2);
    const uint64_t w = widthOf(v) / 8;
    unsigned n = widthOf(i);
    Expr res = wst->arg(0);
    for (uint64_t k = 0; k < w; ++k)
      res = op::array::store(
          res,
          k == 0 ? i
                 : op::ptr::addressPlus(i, num(mpz_class((unsigned long)k), n)),
          op::bv::extract(unsigned(8 * k + 7), unsigned(8 * k), v));
    return res;
  }
  /// Byte k of the w-byte value v (little endian), k an n-bit term with
  /// k < w on the path that uses it: constant extracts selected by k:
  /// extract, not a shift; a constant extract is free when bit-blasting, a
  /// w-way selection of 8-bit values is cheaper than a barrel shifter:
  ///   ite(k = 0, v[7:0], ite(k = 1, v[15:8], ... v[8w-1 : 8w-8]))
  /// a numeral k: the one extract.
  Expr byteOf(const Expr &v, const Expr &k, uint64_t w, unsigned n) {
    unsigned kw;
    if (op::bv::isBvNum(k, kw)) {
      uint64_t c = numVal(k).get_ui();
      return op::bv::extract(unsigned(8 * c + 7), unsigned(8 * c), v);
    }
    // the offsets k can take: < w and < 2^n
    uint64_t last = w - 1;
    if (n < 64 && last > ((uint64_t(1) << n) - 1))
      last = (uint64_t(1) << n) - 1;
    Expr res = op::bv::extract(unsigned(8 * last + 7), unsigned(8 * last), v);
    for (uint64_t c = last; c-- > 0;)
      res = mk<ITE>(mk<EQ>(k, num(mpz_class((unsigned long)c), n)),
                    op::bv::extract(unsigned(8 * c + 7), unsigned(8 * c), v),
                    res);
    return res;
  }

  RewriteResult byteFallback(const Expr &exp) {
    const Expr arr = exp->arg(0), j = exp->arg(1);
    if (isOpX<W_SELECT>(exp)) {
      m_stats.count("wide.split-read");
      return {splitRead(arr, j, op::bv::width(exp->arg(2)) / 8),
              RWStatus::RW_FULL};
    }
    if (isOpX<W_STORE>(arr)) {
      // W-BYTE-IN: a byte read at j over a w-byte write at i, exactly (no
      // premise, any alignment; addresses mod 2^n): j is in the write iff
      // (j - i) <u w, and then the byte is byte (j - i) of v, by constant
      // extracts selected by the offset (byteOf): extract, not a
      // shift:
      //   ite((j - i) <u w, byteOf(v, j - i), M[j])
      // one compare instead of w byte stores and compares
      const Expr i = arr->arg(1), v = arr->arg(2);
      const unsigned vw = widthOf(v), n = widthOf(i);
      const uint64_t w = vw / 8;
      // only where the byte rules could not decide either: both addresses
      // single pointer views into one object at symbolic offsets (ites
      // over objects, different objects: the byte path decides per leaf
      // pair after expanding the write; measured: W-BYTE-IN everywhere
      // grew the vcc VC 1,016,660 -> 1,060,409)
      std::optional<PtrView> vi = view(i), vj = view(j);
      const bool sameObjectSymbolic = vi && vj && vi->base == vj->base;
      if (w >= 2 && m_opts.wideByteIn && sameObjectSymbolic) {
        m_stats.count("wide.byte-in");
        Expr k = mk<PTR_DIFF>(j, i); // S1.5: a pointer difference
        // a write of the whole address space (w = 2^n) covers every j: the
        // byte unconditionally. The guard's numeral w would be 0 at n bits
        // (k <u 0 always false: the old memory, wrong; reproduced at 2-bit
        // addresses, test/VC/seam/wide-byte-in-full-circle.mlir)
        if (n < 64 && w >= (uint64_t(1) << n))
          return {byteOf(v, k, w, n), RWStatus::RW_FULL};
        Expr in = mk<BULT>(k, num(mpz_class((unsigned long)w), n));
        // IS_OVERLAP(i, w, j, 1) within one object: a same-object alias
        // question in its reduced form, for the census (D13)
        m_aliasReduced.insert(in);
        return {
            mk<ITE>(in, byteOf(v, k, w, n), op::array::select(arr->arg(0), j)),
            RWStatus::RW_FULL};
      }
      m_stats.count("wide.expand-write");
      return {op::array::select(expandWrite(arr), j), RWStatus::RW_FULL};
    }
    return {exp, RWStatus::RW_DONE};
  }

  /// Are i and j congruent modulo w (w a power of two within the width
  /// domain)? The difference of their linear forms (Ackermann.hh
  /// LinearForms: bvadd / bvsub / bvneg / a bvmul by a numeral, numerals
  /// summed mod 2^n; other terms atoms) has a numeral part and every
  /// coefficient divisible by w: then i - j = 0 mod w for every assignment.
  /// Same-base pointers `b + 8x` and `b + 8y` (the base cancels).
  bool congruentStarts(const Expr &i0, const Expr &j0, uint64_t w) const {
    // linear forms of the erased terms (a node is its sum)
    const Expr i = erase(i0), j = erase(j0);
    if (w < 2 || (w & (w - 1)) != 0)
      return false;
    unsigned n = widthOf(i);
    if (n < 64 && w > (uint64_t(1) << n))
      return false;
    unsigned t = 0;
    while ((uint64_t(1) << t) < w)
      ++t;
    ackermann_detail::LinearForms lf(n);
    const ackermann_detail::Linear &a = lf.of(i.get());
    const ackermann_detail::Linear &b = lf.of(j.get());
    auto tz = [&](const mpz_class &x) { return LeafDecider::tz(x, n); };
    if (tz(mpz_class(a.c - b.c)) < t)
      return false;
    auto ia = a.coef.begin(), ib = b.coef.begin();
    while (ia != a.coef.end() || ib != b.coef.end()) {
      mpz_class c;
      if (ib == b.coef.end() || (ia != a.coef.end() && ia->first < ib->first))
        c = (ia++)->second;
      else if (ia == a.coef.end() || ib->first < ia->first)
        c = (ib++)->second;
      else
        c = (ia++)->second - (ib++)->second;
      if (tz(c) < t)
        return false;
    }
    return true;
  }

  /// The relation of one leaf pair (li, lj) of a read over a write (as
  /// rowWide: wideRelation, then for equal power-of-two widths whose spans
  /// coincide or are disjoint, congruent starts or both `align` promises,
  /// the compare of row(): decideEq with the access context). \p d gets the
  /// reasons, \p prem the ALIGN premise when the promises were used.
  WRel wideLeafRelation(const Expr &exp, const Expr &arr, const Expr &li,
                        const Expr &lj, uint64_t &k, Decision &d,
                        unsigned &prem, bool shadow) {
    const bool wideRead = isOpX<W_SELECT>(exp);
    const uint64_t r = wideRead ? op::bv::width(exp->arg(2)) / 8 : 1;
    const uint64_t w = widthOf(arr->arg(2)) / 8;
    Decision dl;
    WRel rel = wideRelation(li, w, lj, r, k, dl, shadow);
    if (rel != WRel::Unknown) {
      d.add(dl);
      return rel;
    }
    if (!wideRead || !isOpX<W_STORE>(arr) || r != w)
      return WRel::Unknown;
    const bool congruent = congruentStarts(li, lj, w);
    if (!congruent && !alignedPair(exp, arr, w))
      return WRel::Unknown;
    Decision de;
    Tri t = decideEq(li, lj, de, /*diffBase=*/!shadow, shadow);
    if (t == Tri::Unknown)
      return WRel::Unknown;
    d.add(de);
    if (!congruent)
      prem |= PremAlign;
    k = 0;
    return t == Tri::Yes ? WRel::Hit : WRel::Skip;
  }

  /// Both accesses promise w-alignment (W-ROW-ALIGNED's condition): w a
  /// power of two >= 2 dividing both `align` promises (trusted).
  bool alignedPair(const Expr &exp, const Expr &arr, uint64_t w) const {
    return m_opts.assumeAligned && (w & (w - 1)) == 0 && w >= 2 &&
           // w divides both promises (not >= w: equal for the power-of-two
           // `align` the dialect verifies, but the Expr layer does not
           // re-check it here).
           op::array::promisedAlign(exp) % w == 0 &&
           op::array::promisedAlign(exp) != 0 &&
           op::array::promisedAlign(arr) % w == 0 &&
           op::array::promisedAlign(arr) != 0;
  }

  /// The ite tree of a wide read over the leaf pairs of a write address \p i
  /// and a read address \p j (ITE-EQ's pairing, as eqPairs and buildEq: ites
  /// with one condition branch by branch; at most maxDecideLeaves pairs).
  /// \p leaf gives the value of the read on the paths that select the pair
  /// (li, lj): there i = li and j = lj, so the tree is the read. Its
  /// arguments: the leaves (observations stripped), lj as it is in j, and
  /// whether j was split on the way (an ite of j above the pair). A null
  /// value (an undecided pair) fails the whole tree: null. All leaves one
  /// value: that value.
  using LeafValue =
      llvm::function_ref<Expr(const Expr &, const Expr &, const Expr &, bool)>;
  Expr leafTree(const Expr &i, const Expr &j, LeafValue leaf) {
    llvm::SmallVector<std::pair<Expr, Expr>, 8> pairs;
    if (!eqPairs(i, j, pairs, m_opts.maxDecideLeaves))
      return Expr();
    bool failed = false;
    llvm::SmallVector<Expr, 8> values;
    std::function<Expr(const Expr &, const Expr &, bool)> tree =
        [&](const Expr &l0, const Expr &r0, bool splitJ) -> Expr {
      if (failed)
        return Expr();
      const Expr a = op::ptr::stripObservation(l0),
                 b = op::ptr::stripObservation(r0);
      if (isOpX<ITE>(a) && isOpX<ITE>(b) && a->arg(0) == b->arg(0)) {
        Expr t = tree(a->arg(1), b->arg(1), true);
        Expr e = tree(a->arg(2), b->arg(2), true);
        return failed ? Expr() : mk<ITE>(a->arg(0), t, e);
      }
      if (isOpX<ITE>(a)) {
        Expr t = tree(a->arg(1), r0, splitJ);
        Expr e = tree(a->arg(2), r0, splitJ);
        return failed ? Expr() : mk<ITE>(a->arg(0), t, e);
      }
      if (isOpX<ITE>(b)) {
        Expr t = tree(l0, b->arg(1), true);
        Expr e = tree(l0, b->arg(2), true);
        return failed ? Expr() : mk<ITE>(b->arg(0), t, e);
      }
      Expr val = leaf(a, b, r0, splitJ);
      if (!val)
        failed = true;
      else
        values.push_back(val);
      return val;
    };
    Expr res = tree(i, j, false);
    if (failed || !res)
      return Expr();
    if (llvm::all_of(values, [&](const Expr &x) { return x == values[0]; }))
      return values[0];
    return res;
  }

  /// The w-byte value \p v written at byte \p k of an r-byte read whose
  /// other bytes are those of \p below (W-SPLICE: a write inside the read,
  /// little endian): concat(below[8r-1 : 8(k+w)], v, below[8k-1 : 0]), the
  /// parts of below that are empty left out.
  Expr splice(const Expr &below, const Expr &v, uint64_t k, uint64_t w,
              uint64_t r) {
    Expr res = v;
    if (k > 0)
      res = op::bv::concat(res, op::bv::extract(unsigned(8 * k - 1), 0, below));
    if (k + w < r)
      res = op::bv::concat(
          op::bv::extract(unsigned(8 * r - 1), unsigned(8 * (k + w)), below),
          res);
    return res;
  }

  /// W-ITE (wide ROW over ite addresses): a read (select, wselect) at j over
  /// a store / wstore at i, one of them an ite (at most maxDecideLeaves leaf
  /// pairs, as decideEq): every leaf pair is decided on its own
  /// (wideLeafRelation) and the read is the ite tree of the leaf values,
  /// under the ite conditions (leafTree):
  ///   Hit -> v, Sub(k) -> bytes [k, k + r) of v, Super(k) -> the read below
  ///   at j with v at byte k (splice), Skip -> the read below at j, an
  ///   undecided pair -> the read at the leaf address lj (only where j was
  ///   split: on that path j = lj; it is rewritten as its own read). One
  ///   open pair where j is not split: no rewrite here.
  /// The value keeps its word shape: a read at ite(c, p, q) over writes at
  /// p and q is ite(c, v_p, v_q), not the concat of its bytes. Uses the
  /// premises of the leaf decisions (A-INB: cmp.diff-base, cmp.size0-
  /// vacuous, arm.skip across bases; ALIGN), stated on the whole accesses
  /// (Rules::inBounds and the alignment of an ite address hold leaf by
  /// leaf). Nothing when no pair is decided. RW_FULL (new reads and
  /// extracts below the ite tree).
  std::optional<RewriteResult> rowWideLeaves(const Expr &exp, const Expr &arr,
                                             bool shadow) {
    const Expr i = arr->arg(1), j = exp->arg(1), v = arr->arg(2);
    const bool wideRead = isOpX<W_SELECT>(exp);
    const uint64_t r = wideRead ? op::bv::width(exp->arg(2)) / 8 : 1;
    const uint64_t w = widthOf(v) / 8;
    const uint64_t al = wideRead ? op::array::promisedAlign(exp) : 0;
    auto readAt = [&](const Expr &mem, const Expr &a) -> Expr {
      return wideRead ? op::array::wselect(mem, a, exp->arg(2), al)
                      : op::array::select(mem, a);
    };
    const Expr below = readAt(arr->arg(0), j);
    unsigned resolved = 0, prem = PremNone;
    Decision all;
    llvm::StringMap<uint64_t> leafCounts;
    auto leaf = [&](const Expr &a, const Expr &b, const Expr &r0,
                    bool splitJ) -> Expr {
      uint64_t k = 0;
      WRel rel = wideLeafRelation(exp, arr, a, b, k, all, prem, shadow);
      switch (rel) {
      case WRel::Hit:
        ++resolved;
        ++leafCounts["wide.ite-leaf-hit"];
        return v;
      case WRel::Sub:
        ++resolved;
        ++leafCounts["wide.ite-leaf-sub"];
        return op::bv::extract(unsigned(8 * (k + r) - 1), unsigned(8 * k), v);
      case WRel::Super:
        ++resolved;
        ++leafCounts["wide.ite-leaf-splice"];
        return splice(below, v, k, w, r);
      case WRel::Skip:
        ++resolved;
        ++leafCounts["wide.ite-leaf-skip"];
        return below;
      case WRel::Unknown:
        break;
      }
      if (!splitJ || !m_opts.wideIteOpenLeaves)
        return Expr();
      ++leafCounts["wide.ite-leaf-open"];
      return readAt(arr, r0);
    };
    Expr res = leafTree(i, j, leaf);
    if (!res || resolved == 0)
      return std::nullopt;
    m_stats.count("wide.ite-split");
    for (auto &kv : leafCounts)
      m_stats.rules[kv.getKey()] += kv.getValue();
    countReasons(all);
    countPremises(prem | all.premises);
    return RewriteResult{res, RWStatus::RW_FULL};
  }

  /// W-ROW-CONGRUENT / W-ROW-ALIGNED: a wselect at j over a wstore of the
  /// same width at i whose spans coincide or are disjoint (\p prem: the
  /// premise that says so, none for congruent starts, ALIGN for the `align`
  /// promises). Then the spans overlap iff i = j, so the step is row()'s on
  /// the compare i = j, decided by the same procedure with the access context
  /// (decideEq, diffBase = !shadow; a shadow memory: object identity):
  ///   Yes: v (wide.hit), RW_DONE; No: the read below (wide.skip), RW_1;
  ///   Unknown: ite(<rowCompare(i, j)>, v, the read below), the compare tree
  ///   over the leaves (leaves decided by A-INB, ARM already true / false,
  ///   the rest alias questions). uses: \p prem, and A-INB through the
  ///   compare (cmp.diff-base, cmp.size0-vacuous, arm.skip across bases).
  ///   RW_FULL for the ite (the compare tree and the new wselect).
  RewriteResult rowEqWidth(const Expr &exp, const Expr &arr, const char *rule,
                           unsigned prem, bool shadow) {
    const Expr i = arr->arg(1), j = exp->arg(1), v = arr->arg(2);
    m_stats.count(rule);
    Expr below = op::array::wselect(arr->arg(0), j, exp->arg(2),
                                    op::array::promisedAlign(exp));
    Decision d;
    Tri t = decideEq(i, j, d, /*diffBase=*/!shadow, shadow);
    if (t != Tri::Unknown) {
      countReasons(d);
      countPremises(prem | d.premises);
      if (t == Tri::Yes) {
        m_stats.count("wide.hit");
        return {v, RWStatus::RW_DONE};
      }
      m_stats.count("wide.skip");
      return {below, RWStatus::RW_1};
    }
    countPremises(prem);
    unsigned depth, premises;
    bool reduced;
    Expr cmp = rowCompare(i, j, depth, reduced, premises, shadow);
    return {mk<ITE>(cmp, v, below), RWStatus::RW_FULL};
  }

  /// A read (select: \p r = 1, or wselect) at j over a store / wstore
  /// \p arr: W-HIT, W-SUB, W-SPLICE, W-SKIP, else the byte fallback.
  RewriteResult rowWide(const Expr &exp, const Expr &arr) {
    const bool wideRead = isOpX<W_SELECT>(exp);
    const Expr j = exp->arg(1);
    const uint64_t r = wideRead ? op::bv::width(exp->arg(2)) / 8 : 1;
    const Expr i = arr->arg(1), v = arr->arg(2);
    const uint64_t w = widthOf(v) / 8;
    if (w == 0)
      return {exp, RWStatus::RW_DONE};
    const bool shadow = shadowMemory(arr);
    if (std::optional<RewriteResult> res = leafSplit(exp))
      return *res;
    if (isOpX<ITE>(op::ptr::stripObservation(i)) ||
        isOpX<ITE>(op::ptr::stripObservation(j)))
      if (std::optional<RewriteResult> res = rowWideLeaves(exp, arr, shadow))
        return *res;
    Decision d;
    uint64_t k = 0;
    WRel rel = wideRelation(i, w, j, r, k, d, shadow);
    // W-ROW-CONGRUENT (rev 5): equal power-of-two widths whose starts are
    // congruent mod w: the spans coincide or are disjoint, exactly (no
    // premise). W-ROW-ALIGNED (rev 5): equal power-of-two widths, both
    // accesses annotated w-aligned (trusted `align`, PremAlign): the same.
    // Either way the read is a ROW step on the compare i = j, which row()'s
    // decision procedure answers with the access context (rowEqWidth)
    if (rel == WRel::Unknown && wideRead && isOpX<W_STORE>(arr) && r == w &&
        congruentStarts(i, j, w))
      return rowEqWidth(exp, arr, "wide.row-congruent", PremNone, shadow);
    if (rel == WRel::Unknown && wideRead && isOpX<W_STORE>(arr) && r == w &&
        alignedPair(exp, arr, w))
      return rowEqWidth(exp, arr, "wide.row-aligned", PremAlign, shadow);
    if (rel == WRel::Unknown) {
      // what the fallback pays for (census for an aligned word-level rule)
      if (wideRead && isOpX<W_STORE>(arr))
        m_stats.count(r == w ? "wide.undecided-eqw" : "wide.undecided-neqw");
      else if (wideRead)
        m_stats.count("wide.undecided-over-byte");
      return byteFallback(exp);
    }
    countReasons(d);
    countPremises(d.premises);
    if (rel == WRel::Hit) {
      m_stats.count("wide.hit");
      return {v, RWStatus::RW_DONE};
    }
    if (rel == WRel::Sub) {
      m_stats.count("wide.sub");
      return {op::bv::extract(unsigned(8 * (k + r) - 1), unsigned(8 * k), v),
              RWStatus::RW_1};
    }
    if (rel == WRel::Super) {
      // W-SPLICE: the write inside the read, a word read below with v at
      // byte k (exact; a narrower store under a wide read keeps the word)
      m_stats.count("wide.splice");
      return {splice(op::array::wselect(arr->arg(0), j, exp->arg(2),
                                        op::array::promisedAlign(exp)),
                     v, k, w, r),
              RWStatus::RW_FULL};
    }
    m_stats.count("wide.skip");
    const Expr below = arr->arg(0);
    return {wideRead ? op::array::wselect(below, j, exp->arg(2),
                                          op::array::promisedAlign(exp))
                     : op::array::select(below, j),
            RWStatus::RW_1};
  }

  /// A wselect of r bytes at j over a memory-operation term T (dst d, length
  /// n, below M), per leaf pair of j and d (leafTree), each decided by the
  /// in-test of the memop reads with the access context (decideInPair over
  /// the read span [lj, lj + r): mem.in-exact, arm.range, cmp.diff-base,
  /// arm.skip, cmp.size0-vacuous, as row.<op>):
  ///   In (every byte in [ld, ld + n)): the hit form, mcopy -> a wselect of
  ///     the source version at s + (lj - ld) (k a numeral when exact; a
  ///     changed address: alignment unknown), mset -> the byte repeated,
  ///     mhavoc -> a wselect of its H at j;
  ///   Out (no byte in it, or n = 0): the read moves below T, wselect(M, j);
  ///   else (a partial overlap, an open in-test): undecided.
  /// One pair: wide.memop-hit / wide.memop-skip; an ite: the ite of the leaf
  /// values (wide.memop-split, wide.memop-leaf-*), when every pair is
  /// decided. uses: the premises of the decisions (A-INB across objects: the
  /// read span and the range [d, d + n) in their objects). Undecided:
  /// unchanged here; rowW then takes the byte fallback (rev 3's "the term
  /// stays" was replaced by the fallback after measurement).
  RewriteResult rowWideMemOp(const Expr &exp, const Expr &arr) {
    const Expr j = exp->arg(1), sort = exp->arg(2);
    const uint64_t r = op::bv::width(sort) / 8;
    const uint64_t al = op::array::promisedAlign(exp);
    const Expr d = memOpDst(arr), n = memOpLen(arr);
    const Expr below = op::array::wselect(memOpBase(arr), j, sort, al);
    mpz_class nlo, nhi;
    const bool haveN = lenRange(n, nlo, nhi);
    if (haveN && nhi == 0) {
      m_stats.count("wide.memop-empty");
      Decision de;
      de.add("mem.empty"); // as decideIn
      countReasons(de);
      return {below, RWStatus::RW_1};
    }
    const bool inb = !shadowMemory(arr);
    Decision all;
    unsigned hits = 0, skips = 0;
    bool done = true; // every leaf value is normal (an mset hit)
    auto leaf = [&](const Expr &ld, const Expr &lj, const Expr &,
                    bool) -> Expr {
      Decision dl;
      mpz_class kv = -1;
      Range rg = decideInPair(lj, ld, haveN, nlo, nhi, dl, &kv, inb, r);
      if (rg == Range::Unknown)
        return Expr();
      if (dl.premises & PremAInb)
        checkInbPremise(lj, r, ld, 1); // as wideRelation
      all.add(dl);
      if (rg == Range::Out) {
        ++skips;
        done = false;
        return below;
      }
      ++hits;
      if (isOpX<MEM_SET>(arr)) {
        Expr v = arr->arg(3), res = v;
        for (uint64_t b = 1; b < r; ++b)
          res = op::bv::concat(v, res);
        return res;
      }
      done = false;
      if (isOpX<MEM_COPY>(arr)) {
        Expr k = kv >= mpz_class(0UL) ? num(kv, widthOf(ld))
                                      : mk<PTR_DIFF>(lj, ld); // S1.5
        return op::array::wselect(arr->arg(1), copySource(arr->arg(3), k), sort,
                                  1);
      }
      return op::array::wselect(arr->arg(1), j, sort, al);
    };
    Expr res = leafTree(d, j, leaf);
    if (!res)
      return {exp, RWStatus::RW_DONE};
    const bool split = hits + skips > 1;
    if (split) {
      m_stats.count("wide.memop-split");
      if (hits)
        m_stats.rules["wide.memop-leaf-hit"] += hits;
      if (skips)
        m_stats.rules["wide.memop-leaf-skip"] += skips;
    } else {
      m_stats.count(hits ? "wide.memop-hit" : "wide.memop-skip");
    }
    countReasons(all);
    countPremises(all.premises);
    if (!split && hits && done)
      return {res, RWStatus::RW_DONE};
    return {res, split ? RWStatus::RW_FULL : RWStatus::RW_1};
  }

  /// A wselect of r bytes at j over store-map(a0, b, ocl), per leaf of j
  /// (leafTree; i = b, not an ite), with the access context:
  ///   lj = b + o, o a numeral: the r bytes at o .. o + r - 1 (mod 2^n),
  ///     each the entry at its offset or else that byte of wselect(a0, j)
  ///     (no entry: the read moves below, wide.smap-miss; else
  ///     wide.smap-hit). Exact, no premise (other numeral offsets are other
  ///     addresses, as R-HIT / R-MISS).
  ///   lj in another object than b (decideInPair without a length: cmp.diff-
  ///     base, arm.skip, cmp.size0-vacuous): wselect(a0, j)
  ///     (wide.smap-skip). uses: A-INB (the read span and the entries in
  ///     their objects). Not in a shadow memory.
  ///   else undecided: no rewrite (the byte fallback, R-ABORT per byte).
  /// One leaf: its value; an ite j: the ite of the leaf values
  /// (wide.smap-split, wide.smap-leaf-*), not the concat of byte reads of
  /// the ite address. RW_FULL (new reads below).
  std::optional<RewriteResult> rowWideSmap(const Expr &exp, const Expr &arr) {
    const Expr a0 = arr->arg(0), b = arr->arg(1), ocl = arr->arg(2);
    const Expr j = exp->arg(1), sort = exp->arg(2);
    const uint64_t r = op::bv::width(sort) / 8;
    const bool shadow = shadowMemory(arr);
    const Expr below =
        op::array::wselect(a0, j, sort, op::array::promisedAlign(exp));
    Decision all;
    unsigned hits = 0, misses = 0, skips = 0;
    auto leaf = [&](const Expr &, const Expr &lj, const Expr &, bool) -> Expr {
      std::optional<PtrView> vj = view(lj);
      unsigned nw;
      if (vj && vj->base == b && op::bv::isBvNum(vj->off, nw)) {
        mpz_class o = numVal(vj->off);
        Expr res;
        bool any = false;
        for (uint64_t k = 0; k < r; ++k) {
          Expr e =
              m_smaps.find(ocl, modPow2(o + mpz_class((unsigned long)k), nw));
          any = any || e;
          Expr byte =
              e ? e
                : op::bv::extract(unsigned(8 * k + 7), unsigned(8 * k), below);
          res = k == 0 ? byte : op::bv::concat(byte, res);
        }
        if (!any) {
          ++misses;
          return below;
        }
        ++hits;
        return res;
      }
      Decision d;
      mpz_class none;
      if (!shadow && decideInPair(lj, b, /*haveN=*/false, none, none, d,
                                  nullptr, /*inb=*/true) == Range::Out) {
        ++skips;
        if ((d.premises & PremAInb) && m_opts.checkInbPremise)
          for (const SMapEntry &e : StoreMapTable::entries(ocl))
            checkInbPremise(lj, r, ptrAt(b, e.offset), 1); // as wideRelation
        all.add(d);
        return below;
      }
      return Expr();
    };
    Expr res = leafTree(b, j, leaf);
    if (!res)
      return std::nullopt;
    const bool split = hits + misses + skips > 1;
    if (split)
      m_stats.count("wide.smap-split");
    auto count = [&](const char *one, const char *many, unsigned c) {
      if (c)
        m_stats.rules[split ? many : one] += split ? c : 1;
    };
    count("wide.smap-hit", "wide.smap-leaf-hit", hits);
    count("wide.smap-miss", "wide.smap-leaf-miss", misses);
    count("wide.smap-skip", "wide.smap-leaf-skip", skips);
    countReasons(all);
    countPremises(all.premises);
    return RewriteResult{res, RWStatus::RW_FULL};
  }

  /// One step of a read of \p r bytes (a select: 1) at the address \p a, not
  /// an ite, at the top write of \p mem, decided as the rule the read meets
  /// there decides it: decideEq (row), wideRelation and the congruent
  /// decideEq (rowWide), a numeral offset of the map's object or another
  /// object (rowStoreMap, rowWideSmap), decideInPair (rowMemOp,
  /// rowWideMemOp). Cover: the write gives the read (ROW's hit, W-HIT,
  /// W-SUB, a store-map entry, an mset / mhavoc range); Below: the read
  /// goes on in \p next (decided apart, a store-map miss, a narrower write
  /// inside a wide read: the other bytes below); Base: a memory without
  /// writes (an initial memory, a const array); Ite: an ite memory;
  /// Undecided: anything else (an mcopy hit included). No term is built.
  /// Mixed (leafWalk only): an ite memory with a side Cover and a side Base.
  enum class LeafStep { Undecided, Cover, Below, Base, Ite, Mixed };
  LeafStep leafStep(const Expr &mem, const Expr &a, uint64_t r, Expr &next) {
    const bool shadow = shadowMemory(mem);
    if (isOpX<STORE>(mem) || isOpX<W_STORE>(mem)) {
      const Expr i = mem->arg(1);
      const uint64_t w = std::max<uint64_t>(1, widthOf(mem->arg(2)) / 8);
      next = mem->arg(0);
      Decision d;
      if (r == 1 && isOpX<STORE>(mem)) {
        Tri t = decideEq(i, a, d, /*diffBase=*/!shadow, shadow);
        return t == Tri::Yes  ? LeafStep::Cover
               : t == Tri::No ? LeafStep::Below
                              : LeafStep::Undecided;
      }
      uint64_t k = 0;
      WRel rel = wideRelation(i, w, a, r, k, d, shadow);
      if (rel == WRel::Unknown && isOpX<W_STORE>(mem) && r == w &&
          congruentStarts(i, a, w)) {
        Tri t = decideEq(i, a, d, /*diffBase=*/!shadow, shadow);
        rel = t == Tri::Yes  ? WRel::Hit
              : t == Tri::No ? WRel::Skip
                             : WRel::Unknown;
      }
      if (rel == WRel::Unknown)
        return LeafStep::Undecided;
      // Skip, or Super (the write inside the read: the other bytes below)
      return rel == WRel::Hit || rel == WRel::Sub ? LeafStep::Cover
                                                  : LeafStep::Below;
    }
    if (isOpX<STORE_MAP>(mem)) {
      next = mem->arg(0);
      std::optional<PtrView> v = view(a);
      if (!v)
        return LeafStep::Undecided;
      if (v->base == mem->arg(1)) {
        if (!op::bv::isBvNum(v->off))
          return LeafStep::Undecided;
        // a cell read of an entry: the entry; a miss, or a wide read (the
        // entries and the word below): the memory below too
        return r == 1 && m_smaps.find(mem->arg(2), numVal(v->off))
                   ? LeafStep::Cover
                   : LeafStep::Below;
      }
      return !shadow && distinctObjects(v->base, mem->arg(1))
                 ? LeafStep::Below
                 : LeafStep::Undecided;
    }
    if (isMemOpTerm(mem)) {
      next = memOpBase(mem);
      mpz_class nlo, nhi;
      const bool haveN = lenRange(memOpLen(mem), nlo, nhi);
      llvm::SmallVector<std::pair<Expr, Expr>, 8> pairs;
      if (!eqPairs(a, memOpDst(mem), pairs, m_opts.maxDecideLeaves))
        return LeafStep::Undecided;
      bool allIn = true, allOut = true;
      for (auto &p : pairs) {
        Decision dp;
        Range rg = decideInPair(p.first, p.second, haveN, nlo, nhi, dp, nullptr,
                                !shadow, r);
        allIn = allIn && rg == Range::In;
        allOut = allOut && rg == Range::Out;
      }
      if (allOut)
        return LeafStep::Below;
      return allIn && !isOpX<MEM_COPY>(mem) ? LeafStep::Cover
                                            : LeafStep::Undecided;
    }
    if (isOpX<ITE>(mem))
      return LeafStep::Ite;
    // no write: an initial memory (a constant), a const array; any other
    // memory term is undecided
    return mem->arity() == 0 || bind::isFapp(mem) || isOpX<CONST_ARRAY>(mem)
               ? LeafStep::Base
               : LeafStep::Undecided;
  }

  /// The walk of a read of \p r bytes at \p a (not an ite) down \p mem0 by
  /// leafStep: Cover (a write gives every byte), Base (it reaches a memory
  /// without writes: every write on the way decided apart, or inside a
  /// wide read), Undecided (a write on the way is not decided). An ite
  /// memory: Undecided if a side is, else the sides' answer if they agree,
  /// else Mixed (a side Cover, a side Base). A lookahead for leafSplit only:
  /// nothing is rewritten here, so the premises of the decisions are not this
  /// firing's (each leaf read is rewritten in its own firing, with its own).
  /// Memoized per (memory, address, r).
  LeafStep leafWalk(const Expr &mem0, const Expr &a, uint64_t r) {
    std::vector<Expr> path;
    Expr mem = mem0;
    LeafStep res = LeafStep::Undecided;
    while (true) {
      auto it = m_leafMemo.find(std::make_tuple(mem.get(), a.get(), r));
      if (it != m_leafMemo.end()) {
        res = it->second;
        break;
      }
      path.push_back(mem);
      Expr next;
      LeafStep s = leafStep(mem, a, r, next);
      if (s == LeafStep::Below) {
        mem = next;
        continue;
      }
      if (s == LeafStep::Ite) {
        LeafStep t = leafWalk(mem->arg(1), a, r);
        LeafStep e = leafWalk(mem->arg(2), a, r);
        res = t == LeafStep::Undecided || e == LeafStep::Undecided
                  ? LeafStep::Undecided
              : t == e ? t
                       : LeafStep::Mixed;
      } else {
        res = s;
      }
      break;
    }
    for (const Expr &m : path) {
      m_leafMemo[std::make_tuple(m.get(), a.get(), r)] = res;
      m_leafPin.push_back(m);
    }
    if (!path.empty())
      m_leafPin.push_back(a);
    return res;
  }

  /// R-LEAVES: a read at an ite address is the ite of the reads at its
  /// leaves (exact, no premise: on the paths that select a leaf, the address
  /// is that leaf):
  ///   read(M, ite(c, a1, a2)) ~> ite(c, read(M, a1), read(M, a2))
  /// when every leaf read is decided down the whole memory (leafWalk: none
  /// Undecided, so the split leaves no read at an ite address and no
  /// compare open), every leaf is given by a write (Cover, on every side of
  /// an ite memory), and some leaf is not decided apart from the top write
  /// (otherwise ROW skips the whole read first and the split waits for the
  /// write that gives a leaf). A leaf that reaches the memory without
  /// writes refuses the split (counter row.leaf-split-refused, when some
  /// leaf is given; one per refused attempt, per byte read and memory
  /// level: a read refused at an ite memory may still split lower down): the
  /// split pays by removing the read of that memory, and a leaf read there is a
  /// new read next to the read at the whole address that other reads keep (vcc
  /// hash_table_init_bounded: Ackermann pairs 1596 -> 6328 with one such leaf
  /// of two; SeaM-mixed-width.md, "R-LEAVES guard"). A general split over an
  /// ite index was measured against keeping it (SeaM-memops.md, "ite
  /// indices"): it grows the DAG when leaves stay open; here none does. A
  /// pointer or word load then reads the stored value per leaf. RW_FULL (new
  /// reads).
  ///
  /// The address is an ite, or a node N(ite(c, b1, b2), x...) over an ite
  /// base (the node's leaves N(b_k, x...): ptr-index of an ite is the ite of
  /// the ptr-index per leaf, as ptrindex.pull-ite, which stops at
  /// maxPullLeaves; counter row.leaf-split-node). The ite is a DAG: it is
  /// walked once per ite node, the bound is on its distinct leaves (at most
  /// maxDecideLeaves), each distinct leaf is decided once and the result has
  /// the ite's DAG shape. The address's own ites are the conditions.
  ///
  /// R-LEAVES-PARTIAL (leafSplitPartial): when some leaf reads are
  /// Undecided, the others decided and given by a write (none reaching the
  /// memory without writes), the decided leaves split and the undecided ones
  /// stay together as one read at the residual address U, the ite pruned to
  /// its undecided leaves (a sub-ite with no undecided leaf is dropped for
  /// its sibling). Exact: on a path that selects an undecided leaf u, U
  /// selects u too (the conditions are the same and only subtrees without
  /// undecided leaves are dropped), so read(M, U) = read(M, u); a sub-ite
  /// with only undecided leaves is read(M, U). Counters
  /// row.leaf-split-partial, row.leaf-split-residual-leaves.
  std::optional<RewriteResult> leafSplit(const Expr &exp) {
    if (!m_opts.leafSplit)
      return std::nullopt;
    const Expr mem = exp->arg(0), j = exp->arg(1);
    // the address's ite: j, or the base of a node over it
    Expr it = j, node;
    if (op::ptr::isPtrIndex(j) && isOpX<ITE>(op::ptr::base(j))) {
      node = j;
      it = op::ptr::base(j);
    }
    if (!isOpX<ITE>(it) || m_bases.lookup(it))
      return std::nullopt;
    if (!isOpX<STORE>(mem) && !isOpX<W_STORE>(mem) && !isOpX<STORE_MAP>(mem) &&
        !isMemOpTerm(mem))
      return std::nullopt;
    const bool wideRead = isOpX<W_SELECT>(exp);
    const uint64_t r = wideRead ? op::bv::width(exp->arg(2)) / 8 : 1;
    auto isIte = [&](const Expr &e) {
      return isOpX<ITE>(e) && !m_bases.lookup(e);
    };
    // the address of a leaf (the node over it, for a node)
    auto leafAddr = [&](const Expr &l) -> Expr {
      if (!node)
        return l;
      ExprVector kids(node->args_begin(), node->args_end());
      kids[0] = l;
      return node->efac().mkNary(node->op(), kids);
    };
    auto readAt = [&](const Expr &a) -> Expr {
      return wideRead ? op::array::wselect(mem, a, exp->arg(2),
                                           op::array::promisedAlign(exp))
                      : op::array::select(mem, a);
    };
    // the distinct ite nodes and leaves (iterative: the DAG may be deep)
    std::unordered_set<ENode *> ites;
    ExprVector leafList;
    {
      std::unordered_set<ENode *> seenLeaf;
      ExprVector stack{it};
      while (!stack.empty()) {
        Expr e = stack.back();
        stack.pop_back();
        if (isIte(e)) {
          if (ites.insert(e.get()).second) {
            stack.push_back(e->arg(2));
            stack.push_back(e->arg(1));
          }
          continue;
        }
        if (seenLeaf.insert(e.get()).second) {
          leafList.push_back(e);
          if (leafList.size() > m_opts.maxDecideLeaves)
            return std::nullopt;
        }
      }
    }
    // each distinct leaf decided once: Undecided leaves are the residual's
    std::unordered_map<ENode *, bool> undecided;
    bool cover = false, topOpen = false, below = false;
    unsigned nUndecided = 0;
    for (const Expr &l : leafList) {
      const Expr a = op::ptr::stripObservation(leafAddr(l));
      LeafStep s = leafWalk(mem, a, r);
      Expr next;
      topOpen = topOpen || leafStep(mem, a, r, next) != LeafStep::Below;
      if (s == LeafStep::Undecided) {
        if (!m_opts.leafSplitPartial)
          return std::nullopt;
        undecided[l.get()] = true;
        ++nUndecided;
        continue;
      }
      undecided[l.get()] = false;
      cover = cover || s != LeafStep::Base;
      below = below || s != LeafStep::Cover;
    }
    // nothing decided, or nothing given by a write: no split
    if (nUndecided == leafList.size() || !cover || !topOpen)
      return std::nullopt;
    if (below) {
      m_stats.count("row.leaf-split-refused");
      return std::nullopt;
    }
    // per ite node: does it have an undecided / a decided leaf (post-order,
    // memoized)
    std::unordered_map<ENode *, std::pair<bool, bool>> has;
    std::function<std::pair<bool, bool>(const Expr &)> hasOf =
        [&](const Expr &e) -> std::pair<bool, bool> {
      if (!isIte(e)) {
        bool u = undecided.find(e.get())->second;
        return {u, !u};
      }
      auto f = has.find(e.get());
      if (f != has.end())
        return f->second;
      auto t = hasOf(e->arg(1)), el = hasOf(e->arg(2));
      std::pair<bool, bool> res{t.first || el.first, t.second || el.second};
      has.emplace(e.get(), res);
      return res;
    };
    hasOf(it);
    // the residual address: the ite pruned to its undecided leaves
    std::unordered_map<ENode *, Expr> prunedMemo;
    std::function<Expr(const Expr &)> pruned = [&](const Expr &e) -> Expr {
      if (!isIte(e))
        return e;
      auto f = prunedMemo.find(e.get());
      if (f != prunedMemo.end())
        return f->second;
      const bool inThen = hasOf(e->arg(1)).first,
                 inElse = hasOf(e->arg(2)).first;
      Expr res =
          inThen && inElse
              ? Expr(mk<ITE>(e->arg(0), pruned(e->arg(1)), pruned(e->arg(2))))
          : inThen ? pruned(e->arg(1))
                   : pruned(e->arg(2));
      prunedMemo.emplace(e.get(), res);
      return res;
    };
    Expr residual;
    if (nUndecided)
      residual = readAt(leafAddr(pruned(it)));
    ExprVector conds;
    std::unordered_map<ENode *, Expr> valMemo;
    std::function<Expr(const Expr &)> val = [&](const Expr &e) -> Expr {
      if (!isIte(e))
        return undecided.find(e.get())->second ? residual : readAt(leafAddr(e));
      auto f = valMemo.find(e.get());
      if (f != valMemo.end())
        return f->second;
      Expr res;
      if (!hasOf(e).second) {
        res = residual;
      } else {
        conds.push_back(e->arg(0));
        res = mk<ITE>(e->arg(0), val(e->arg(1)), val(e->arg(2)));
      }
      valMemo.emplace(e.get(), res);
      return res;
    };
    Expr res = val(it);
    m_stats.count("row.leaf-split");
    m_stats.rules["row.leaf-split-leaves"] += leafList.size();
    if (node)
      m_stats.count("row.leaf-split-node");
    if (nUndecided) {
      m_stats.count("row.leaf-split-partial");
      m_stats.rules["row.leaf-split-residual-leaves"] += nUndecided;
    }
    for (const Expr &c : conds)
      m_leafConds.insert(c);
    return RewriteResult{res, RWStatus::RW_FULL};
  }

  /// A wselect: over a store / wstore (rowWide), through an ite (exact),
  /// across a store-map of another object (A-INB), over a memop term
  /// (rowWideMemOp); undecided over any of these: the byte fallback; over
  /// an initial memory or a const array it stays (the boundary makes the
  /// same byte reads).
  RewriteResult rowW(const Expr &exp) {
    const Expr arr = exp->arg(0), j = exp->arg(1), sort = exp->arg(2);
    if (isOpX<W_STORE>(arr) || isOpX<STORE>(arr))
      return rowWide(exp, arr);
    if (isOpX<STORE_MAP>(arr) || isMemOpTerm(arr))
      if (std::optional<RewriteResult> res = leafSplit(exp))
        return *res;
    if (isMemOpTerm(arr)) {
      RewriteResult res = rowWideMemOp(exp, arr);
      return res.exp == exp ? byteFallback(exp) : res;
    }
    if (isOpX<ITE>(arr)) {
      m_stats.count("wide.ite");
      const uint64_t al = op::array::promisedAlign(exp);
      return {mk<ITE>(arr->arg(0), op::array::wselect(arr->arg(1), j, sort, al),
                      op::array::wselect(arr->arg(2), j, sort, al)),
              RWStatus::RW_1};
    }
    if (isOpX<STORE_MAP>(arr)) {
      if (std::optional<RewriteResult> res = rowWideSmap(exp, arr))
        return *res;
      return byteFallback(exp);
    }
    // an initial memory, a const array: the wselect stays (the boundary
    // expands it into the same byte reads)
    return {exp, RWStatus::RW_DONE};
  }

  /// For the memory-operation terms (MemOps.hh): the destination
  /// ranges (d, n) a rule at \p lhs relies on when it uses A-INB: the term
  /// directly below a select (the cross-object skip, row.<op>-skip with
  /// cmp.diff-base, needs every cell of [d, d + n) in d's object).
  void accessRanges(const Expr &lhs,
                    llvm::SmallVectorImpl<std::pair<Expr, Expr>> &out) const {
    if ((isOpX<SELECT>(lhs) || isOpX<W_SELECT>(lhs)) &&
        isMemOpTerm(lhs->arg(0)))
      out.emplace_back(memOpDst(lhs->arg(0)), memOpLen(lhs->arg(0)));
    // a firing at the term itself (addr.prune-size0 over ranges): its own
    // ranges, as they were before the rewrite (the lhs is the original term)
    if (isMemOpTerm(lhs)) {
      out.emplace_back(memOpDst(lhs), memOpLen(lhs));
      if (isOpX<MEM_COPY>(lhs))
        out.emplace_back(lhs->arg(3), memOpLen(lhs));
    }
  }

  /// A-INB for a range access of \p n units at \p d (a memset / memmove /
  /// memhavoc destination, a memmove source), per leaf of an ite: for a leaf
  /// that is a pointer into object k with offset `off`,
  ///   n = 0  or  (n <=u size_k  and  off <=u size_k - n)
  /// (every cell of [d, d + n) is in object k, no wrap; n = 0 is no access);
  /// true for a leaf that is not a pointer (A-INB says nothing about it).
  /// For an object of size 0 (null) the condition is n = 0: a range of length
  /// 0 accesses nothing, even through null (SeaM-null.md, point 6). A leaf
  /// where a numeral n > 0 does not fit (n > size, or a numeral offset with
  /// off + n > size) is `false`; guarded aggregation as inBounds: \p
  /// violated is set iff the whole condition is `false`.
  Expr rangeInBounds(const Expr &d, const Expr &n, bool &violated) const {
    unsigned w = widthOf(n);
    mpz_class nv;
    unsigned nw;
    const bool nNum = op::bv::isBvNum(n, nw);
    if (nNum) {
      nv = numVal(n);
      if (nv == 0)
        return mk<TRUE>(m_efac);
    }
    auto leaf = [&](const Expr &p) -> Expr {
      std::optional<PtrView> v = view(p);
      if (!v)
        return mk<TRUE>(m_efac);
      const ObjectBase *o = m_bases.lookup(v->base);
      if (!o || o->size.getBitWidth() > 64)
        return mk<TRUE>(m_efac);
      mpz_class size = static_cast<unsigned long>(o->size.getZExtValue());
      unsigned ow;
      if (nNum) {
        if (nv > size)
          return mk<FALSE>(m_efac);
        if (op::bv::isBvNum(v->off, ow))
          return numVal(v->off) + nv <= size ? mk<TRUE>(m_efac)
                                             : mk<FALSE>(m_efac);
        return mk<BULE>(offSum(v->off), num(size - nv, w));
      }
      if (size == 0)
        return mk<EQ>(n, num(0, w));
      return mk<OR>(
          mk<EQ>(n, num(0, w)),
          mk<AND>(mk<BULE>(n, num(size, w)),
                  mk<BULE>(offSum(v->off), mk<BSUB>(num(size, w), n))));
    };
    std::unordered_map<Expr, Expr> memo;
    llvm::SmallVector<std::pair<Expr, bool>, 32> stack;
    stack.push_back({d, false});
    while (!stack.empty()) {
      auto [p, expanded] = stack.back();
      stack.pop_back();
      if (memo.count(p))
        continue;
      if (!isOpX<ITE>(p)) {
        memo.emplace(p, leaf(p));
        continue;
      }
      if (!expanded) {
        stack.push_back({p, true});
        for (unsigned i : {2u, 1u})
          if (!memo.count(p->arg(i)))
            stack.push_back({p->arg(i), false});
        continue;
      }
      const Expr &t = memo.find(p->arg(1))->second;
      const Expr &e = memo.find(p->arg(2))->second;
      memo.emplace(p, iteCond(p->arg(0), t, e));
    }
    Expr res = memo.find(d)->second;
    if (isOpX<FALSE>(res))
      violated = true;
    return res;
  }

  //===------------------------------------------------------------------===//
  // compare
  //===------------------------------------------------------------------===//

  /// Fig. 4.2 / 2.2 on two terms that are not ites: Yes, No or Unknown, and
  /// the rule that decided it (a counter name).
  ///   cmp.refl       a = a                              no assumption
  ///   cmp.bvnum      numerals, mod 2^w                  no assumption
  ///   cmp.bool       true = false                       no assumption
  ///   cmp.same-base  ptr(b, o1) = ptr(b, o2) iff o1 = o2, numeral offsets;
  ///                  bvadd(b, o1) = bvadd(b, o2) <=> o1 = o2 holds for any
  ///                  bit-vectors (addition is injective)  no assumption
  ///   cmp.diff-base  ptr(b1, o1) != ptr(b2, o2), b1 != b2, both objects of
  ///                  nonzero size (a size-0 object shares its base with a
  ///                  neighbour in the SeaHorn layout), only when
  ///                  \p diffBase (the compare is the one of a ROW step
  ///                  between two access addresses): uses A-INB
  ///   cmp.size0-vacuous  one side is a pointer into an object of size 0
  ///                  (null, a zero-sized allocation), only when \p diffBase:
  ///                  an access of positive width is not in that object, so
  ///                  A-INB of that access is false and the pair is
  ///                  vacuous; decided different (SeaM-null.md, point 3).
  ///                  uses: A-INB. Never for a program compare (null + k may
  ///                  equal obj.A + j numerically).
  ///   cmp.cancel     a + x = a + y => x = y numerals     no assumption
  ///   arm.skip       the address range maps are disjoint: no assumption
  ///                  for one base or numbers, A-INB (\p d records it) when
  ///                  the decision separates two bases
  ///   shadow.distinct-base  only when \p shadow (the compare of a ROW step
  ///                  in a shadow memory; \p diffBase is then false: no
  ///                  A-INB rule, no ARM): obj.a != obj.b, two registered
  ///                  bases, both offsets the numeral 0, whose placed
  ///                  numerals differ. A shadow cell is keyed by object
  ///                  identity (sea-lower-tracking: sea.ptr.base), and
  ///                  placement gives every object its own base (size 0
  ///                  included: sea-place-static), so after the layout
  ///                  equations obj.a = n_a, obj.b = n_b it is n_a != n_b,
  ///                  two different numerals. uses: LAYOUT (a fact of the
  ///                  VC, not an assumption; SeaM-null.md, "Shadow
  ///                  memories"). Two bases with equal numerals (a hand-
  ///                  placed program) stay Unknown.
  /// When decided, \p d gets the rule and its premises.
  Tri decidePair(const Expr &a0, const Expr &b0, Decision &d, bool diffBase,
                 bool shadow = false) const {
    const Expr a = op::ptr::stripObservation(a0),
               b = op::ptr::stripObservation(b0);
    // memo (PtrSort rev 5, S0.6): a pure function of (a, b, diffBase,
    // shadow). The base registry only grows when a segment op runs, before
    // any term mentions that object, so an answer never goes stale (as for
    // ARM's cache). A data access and its keyed companions at the same
    // address ask the same pairs: decided once. Counted
    // decide.pairs / decide.memo-hits.
    auto key = std::make_tuple(a.get(), b.get(), diffBase, shadow);
    auto it = m_pairMemo.find(key);
    if (it != m_pairMemo.end()) {
      ++m_stats.rules["decide.memo-hits"]; // bookkeeping: not in the trail
      d.add(it->second.d);
      return it->second.t;
    }
    ++m_stats.rules["decide.pairs"];
    Decision dp;
    Tri t = decidePairUncached(a, b, dp, diffBase, shadow);
    m_pairPin.push_back(a);
    m_pairPin.push_back(b);
    m_pairMemo.emplace(key, PairMemo{t, dp});
    d.add(dp);
    return t;
  }

  /// decidePair on \p a, \p b without their observation markers.
  Tri decidePairUncached(const Expr &a, const Expr &b, Decision &d,
                         bool diffBase, bool shadow) const {
    if (sameAddr(a, b)) {
      d.add("cmp.refl");
      return Tri::Yes;
    }
    mpz_class va, vb;
    if (ptrNumerals(a, b, va, vb)) {
      d.add("cmp.bvnum");
      return va == vb ? Tri::Yes : Tri::No;
    }
    if ((isOpX<TRUE>(a) && isOpX<FALSE>(b)) ||
        (isOpX<FALSE>(a) && isOpX<TRUE>(b))) {
      d.add("cmp.bool");
      return Tri::No;
    }
    std::optional<PtrView> pa = view(a), pb = view(b);
    if (pa && pb) {
      if (pa->base == pb->base) {
        if (sameOff(pa->off, pb->off)) {
          d.add("cmp.same-base");
          return Tri::Yes;
        }
        if (numerals(pa->off, pb->off, va, vb)) {
          d.add("cmp.same-base");
          return va == vb ? Tri::Yes : Tri::No;
        }
      } else if (diffBase && m_opts.assumeInBounds) {
        const ObjectBase *oa = m_bases.lookup(pa->base);
        const ObjectBase *ob = m_bases.lookup(pb->base);
        if (oa && ob && !oa->size.isZero() && !ob->size.isZero()) {
          d.add("cmp.diff-base", PremAInb); // uses: A-INB
          checkInbPremise(a, 1, b, 1);
          return Tri::No;
        }
      } else if (shadow && numVal0(pa->off) && numVal0(pb->off)) {
        const ObjectBase *oa = m_bases.lookup(pa->base);
        const ObjectBase *ob = m_bases.lookup(pb->base);
        if (oa && ob && oa->base.getBitWidth() == ob->base.getBitWidth() &&
            oa->base != ob->base) {
          d.add("shadow.distinct-base", PremLayout); // uses: LAYOUT
          return Tri::No;
        }
      } else if (std::optional<mpz_class> v =
                     diffBase ? layoutDiff(a, b) : std::nullopt) {
        d.add("cmp.layout", PremLayout); // uses: LAYOUT
        return modPow2(*v, widthOf(a)) == 0 ? Tri::Yes : Tri::No;
      }
    }
    // after the premise-free same-base decisions: a pair of equal terms or
    // of numeral offsets of one base is decided without A-INB
    if (diffBase && m_opts.assumeInBounds &&
        ((pa && size0(pa->base)) || (pb && size0(pb->base)))) {
      d.add("cmp.size0-vacuous", PremAInb); // uses: A-INB
      return Tri::No;
    }
    mpz_class dv;
    if (cancelDiff(a, b, dv)) {
      d.add("cmp.cancel");
      return dv == 0 ? Tri::Yes : Tri::No;
    }
    if (m_opts.congruence && incongruent(a, b)) {
      d.add("cmp.congruence");
      return Tri::No;
    }
    if (armDisjoint(a, b, diffBase, d))
      return Tri::No;
    return Tri::Unknown;
  }

  /// cmp.congruence (no premise): a and b differ for every assignment if
  /// the difference of their linear forms (as congruentStarts: the erased
  /// terms, Ackermann.hh LinearForms) is sum c_k x_k + d with 2^t dividing
  /// every c_k and not d (t = the least trailing-zero count of the c_k, at
  /// most the width): a - b = d mod 2^t != 0. Same-base pointers b + 8x + 1
  /// and b + 8y (the base cancels); slots of a table read a byte apart.
  bool incongruent(const Expr &a0, const Expr &b0) const {
    const Expr a = erase(a0), b = erase(b0);
    unsigned n = widthOf(a);
    if (n == 0 || widthOf(b) != n)
      return false;
    ackermann_detail::LinearForms lf(n);
    const ackermann_detail::Linear &la = lf.of(a.get());
    const ackermann_detail::Linear &lb = lf.of(b.get());
    auto tz = [&](const mpz_class &x) { return LeafDecider::tz(x, n); };
    unsigned t = n;
    bool any = false;
    auto ia = la.coef.begin(), ib = lb.coef.begin();
    while (ia != la.coef.end() || ib != lb.coef.end()) {
      mpz_class c;
      if (ib == lb.coef.end() || (ia != la.coef.end() && ia->first < ib->first))
        c = (ia++)->second;
      else if (ia == la.coef.end() || ib->first < ia->first)
        c = mpz_class(0L) - (ib++)->second;
      else
        c = (ia++)->second - (ib++)->second;
      unsigned z = tz(c);
      if (z < n)
        any = true;
      t = std::min(t, z);
    }
    // no variable part: cancelDiff decides it
    if (!any || t == 0)
      return false;
    return tz(mpz_class(la.c - lb.c)) < t;
  }

  /// ARM (thesis Thm 4): alpha(a) and alpha(b) have no pointer in common, so
  /// a != b. Only for the compare of a ROW step (\p rowCompare: two access
  /// addresses); never for a program compare. Same-base and absolute
  /// intervals that do not meet need no assumption (AddrRangeMap.hh: no
  /// A-NOWRAP, overflow gives top); different bases use A-INB (uses: A-INB,
  /// only with assumeInBounds, through the access conditions of
  /// Rules::inBounds).
  /// When true, \p d gets `arm.skip`, with PremAInb if the answer is true
  /// because of A-INB (the maps are disjoint only across different bases),
  /// and `cmp.size0-vacuous` (A-INB) if it dropped the provenance of an
  /// object of size 0 (AddrRangeMap::disjoint).
  bool armDisjoint(const Expr &a, const Expr &b, bool rowCompare,
                   Decision &d) const {
    if (!rowCompare || !m_opts.arm)
      return false;
    bool cross = false, zero = false;
    if (!m_arm.disjoint(a, b, m_opts.assumeInBounds, &cross, &zero))
      return false;
    d.add("arm.skip", cross ? PremAInb : PremNone);
    if (zero)
      d.add("cmp.size0-vacuous", PremAInb); // uses: A-INB
    return true;
  }

  /// The leaf pairs of `l = r` with ites split (ITE-EQ-1, ITE-EQ-2: two ites
  /// with the same condition are paired branch by branch); false if there are
  /// more than \p cap.
  bool eqPairs(const Expr &l0, const Expr &r0,
               llvm::SmallVectorImpl<std::pair<Expr, Expr>> &out,
               unsigned cap) const {
    const Expr l = op::ptr::stripObservation(l0),
               r = op::ptr::stripObservation(r0);
    if (isOpX<ITE>(l) && isOpX<ITE>(r) && l->arg(0) == r->arg(0))
      return eqPairs(l->arg(1), r->arg(1), out, cap) &&
             eqPairs(l->arg(2), r->arg(2), out, cap);
    if (isOpX<ITE>(l))
      return eqPairs(l->arg(1), r, out, cap) && eqPairs(l->arg(2), r, out, cap);
    if (isOpX<ITE>(r))
      return eqPairs(l, r->arg(1), out, cap) && eqPairs(l, r->arg(2), out, cap);
    out.emplace_back(l, r);
    return out.size() <= cap;
  }

  /// Does `a = b` hold on every leaf pair (Yes), fail on every one (No), or
  /// neither / too many pairs (Unknown)? No term is built. \p d gets the
  /// reasons of *every* leaf pair the answer rests on, with their premises (a
  /// later pair can need A-INB although the first is a same-base decision).
  /// uses: A-INB (cmp.diff-base, arm.skip across bases) when \p diffBase;
  /// LAYOUT (shadow.distinct-base) when \p shadow.
  Tri decideEq(const Expr &a0, const Expr &b0, Decision &d, bool diffBase,
               bool shadow = false) const {
    const Expr a = op::ptr::stripObservation(a0),
               b = op::ptr::stripObservation(b0);
    if (sameAddr(a, b)) {
      d.add("cmp.refl");
      return Tri::Yes;
    }
    llvm::SmallVector<std::pair<Expr, Expr>, 8> pairs;
    if (!eqPairs(a, b, pairs, m_opts.maxDecideLeaves)) {
      // too many leaf pairs to look at one by one: the ARM of the whole terms
      if (armDisjoint(a, b, diffBase, d))
        return Tri::No;
      return Tri::Unknown;
    }
    bool allYes = true, allNo = true;
    Decision all;
    for (auto &p : pairs) {
      Decision dp;
      Tri t = decidePair(p.first, p.second, dp, diffBase, shadow);
      allYes = allYes && t == Tri::Yes;
      allNo = allNo && t == Tri::No;
      if (!allYes && !allNo)
        return Tri::Unknown;
      all.add(dp);
    }
    d.add(all);
    return allYes ? Tri::Yes : Tri::No;
  }

  /// Two numeral pointers: numerals, or constructors / observations of
  /// numerals (from_bits(c), from_int(c): a constructor has no provenance,
  /// but its address is the numeral; PtrSort.md, pointer differences). Exact.
  static bool ptrNumerals(const Expr &a, const Expr &b, mpz_class &va,
                          mpz_class &vb) {
    auto strip = [](Expr e) {
      while (op::ptr::isMarker(e))
        e = e->arg(0);
      return e;
    };
    return numerals(strip(a), strip(b), va, vb);
  }

  /// The summands of an address term, as the deciders read it (PtrSort
  /// S2a): a numeral is added to \p sum, a bvadd's arguments and an
  /// observation marker's argument are read on, a node gives its base's
  /// summands, one x_k * s_k per index (normalized as the boundary's sum)
  /// and c + K; any other term is an atom. No term of the VC is built.
  void deciderSummands(const Expr &e0, llvm::SmallVectorImpl<Expr> &terms,
                       mpz_class &sum, unsigned &w) const {
    const Expr e = op::ptr::stripObservation(e0);
    unsigned tw;
    if (op::bv::isBvNum(e, tw)) {
      w = tw;
      sum = sum + numVal(e);
      return;
    }
    if (isOpX<BADD>(e)) {
      for (size_t i = 0; i < e->arity(); ++i)
        deciderSummands(e->arg(i), terms, sum, w);
      return;
    }
    if (!op::ptr::isPtrIndex(e)) {
      terms.push_back(e);
      return;
    }
    unsigned n = widthOf(op::ptr::base(e));
    w = n;
    deciderSummands(op::ptr::base(e), terms, sum, w);
    for (size_t k = 0; k < op::ptr::numIndices(e); ++k)
      deciderSummands(indexSummand(op::ptr::index(e, k), op::ptr::stride(e, k),
                                   n, &m_scratchBv),
                      terms, sum, w);
    sum = sum + op::ptr::constant(e) + op::ptr::byteOffset(e);
  }

  /// The OFFSET cancellation (rule cmp.cancel; PtrSort.md, pointer
  /// differences): the summands of \p a and \p b (bvadds, nodes:
  /// deciderSummands) are one multiset up to their numerals, and something
  /// cancels: a - b is the numeral \p d (mod 2^w). Valid over bit-vectors
  /// (addition mod 2^w is cancellative). Replaces cancel() on the flat forms.
  /// The A-INB premise check: a cmp.diff-base decision rests on A-INB of both
  /// addresses, which the run states only for the accesses OpSem recorded
  /// (accessAddr: the address in normal form and the access's units). Both
  /// addresses of a decision must lie in a recorded access: the same term,
  /// or p + k with 0 <= k and k + span <= units (cancelDiff; the byte
  /// fallback's p + k of a wide access). Counted (premise.inb-checked /
  /// premise.inb-unrecorded); sea-vc warns vc.inb-premise-unrecorded and
  /// prints the first few unrecorded accesses.
  void noteInbAccess(const Expr &p0, uint64_t units) const {
    const Expr p = op::ptr::stripObservation(p0);
    auto [it, fresh] = m_inbRec.try_emplace(p.get(), InbRec{p, units});
    if (!fresh)
      it->second.units = std::max(it->second.units, units);
    std::optional<PtrView> v = view(p);
    if (fresh && v)
      m_inbByBase[v->base.get()].push_back(p.get());
  }
  bool inbRecorded(const Expr &x0, uint64_t span) const {
    const Expr x = op::ptr::stripObservation(x0);
    auto it = m_inbRec.find(x.get());
    if (it != m_inbRec.end() && it->second.units >= span)
      return true;
    std::optional<PtrView> v = view(x);
    if (!v)
      return false;
    auto bt = m_inbByBase.find(v->base.get());
    if (bt == m_inbByBase.end())
      return false;
    for (ENode *pn : bt->second) {
      const InbRec &r = m_inbRec.at(pn);
      mpz_class dv;
      if (cancelDiff(x, r.p, dv) && dv >= mpz_class(0L) &&
          dv + mpz_class((unsigned long)span) <=
              mpz_class((unsigned long)r.units))
        return true;
    }
    return false;
  }
  void checkInbPremise(const Expr &a, uint64_t sa, const Expr &b,
                       uint64_t sb) const {
    if (!m_opts.checkInbPremise || m_inbRec.empty())
      return;
    ++m_stats.rules["premise.inb-checked"]; // bookkeeping: not in the trail
    for (auto [x, sp] : {std::pair<const Expr &, uint64_t>{a, sa}, {b, sb}})
      if (!inbRecorded(x, sp)) {
        ++m_stats.rules["premise.inb-unrecorded"];
        if (m_inbUnrecorded.size() < 4) {
          std::string t;
          llvm::raw_string_ostream os(t);
          os << *x;
          m_inbUnrecorded.push_back(os.str().substr(0, 200));
        }
      }
  }
  const std::vector<std::string> &inbUnrecorded() const {
    return m_inbUnrecorded;
  }

  bool cancelDiff(const Expr &a0, const Expr &b0, mpz_class &d) const {
    const Expr a = op::ptr::stripObservation(a0),
               b = op::ptr::stripObservation(b0);
    if (!isOpX<BADD>(a) && !isOpX<BADD>(b) && !op::ptr::isPtrIndex(a) &&
        !op::ptr::isPtrIndex(b))
      return false;
    llvm::SmallVector<Expr, 8> sa, sb;
    mpz_class ca = 0, cb = 0;
    unsigned w = 0;
    deciderSummands(a, sa, ca, w);
    deciderSummands(b, sb, cb, w);
    unsigned cancelled = 0;
    for (size_t i = 0; i < sa.size();) {
      auto it = std::find(sb.begin(), sb.end(), sa[i]);
      if (it == sb.end())
        return false; // a term of a only: not a numeral difference
      sb.erase(it);
      sa.erase(sa.begin() + i);
      ++cancelled;
    }
    if (!sb.empty())
      return false;
    if (ca == cb && ca != 0)
      ++cancelled;
    if (!cancelled)
      return false;
    if (!w)
      w = widthOf(a);
    d = modPow2(ca - cb, w);
    return true;
  }

  /// Cancellation: bvadd(S1, c1) = bvadd(S2, c2) with a term in both S1 and
  /// S2 (a multiset difference) becomes l = r, with the common terms
  /// removed and the numerals moved to the left. Valid over bit-vectors
  /// (addition mod 2^w is cancellative). False if nothing cancels.
  bool cancel(const Expr &a, const Expr &b, Expr &l, Expr &r) const {
    if (!isOpX<BADD>(a) && !isOpX<BADD>(b))
      return false;
    llvm::SmallVector<Expr, 4> sa, sb;
    mpz_class ca = 0, cb = 0;
    unsigned w = 0;
    auto collect = [&](const Expr &e, llvm::SmallVectorImpl<Expr> &terms,
                       mpz_class &c) {
      auto one = [&](const Expr &t) {
        unsigned tw;
        if (op::bv::isBvNum(t, tw)) {
          w = tw;
          c = c + numVal(t);
        } else {
          terms.push_back(t);
        }
      };
      if (isOpX<BADD>(e))
        for (size_t i = 0; i < e->arity(); ++i)
          one(e->arg(i));
      else
        one(e);
    };
    collect(a, sa, ca);
    collect(b, sb, cb);
    unsigned cancelled = 0;
    for (size_t i = 0; i < sa.size();) {
      auto it = std::find(sb.begin(), sb.end(), sa[i]);
      if (it == sb.end()) {
        ++i;
        continue;
      }
      sb.erase(it);
      sa.erase(sa.begin() + i);
      ++cancelled;
    }
    // equal nonzero numerals cancel as well (x + 4 = y + 4)
    if (ca == cb && ca != 0)
      ++cancelled;
    if (!cancelled)
      return false;
    if (!w)
      w = widthOf(a);
    mpz_class d = modPow2(ca - cb, w);
    if (d != 0)
      sa.push_back(num(d, w));
    l = sumOf(sa, w);
    r = sumOf(sb, w);
    return true;
  }

  /// Build `l = r` over the leaves of two ite trees (ITE-EQ). Leaves that
  /// decidePair resolves become true / false, leaves that reduce (same base,
  /// cancellation) become the reduced compare; \p depth is the depth of the
  /// resulting tree (0 for a single compare), \p resolved the number of
  /// resolved or reduced leaves, \p reduced whether a leaf became a new
  /// compare of sums (same base, cancellation): the kids of that compare are
  /// new terms, one level below the leaves. \p premises collects the premises
  /// of every resolved leaf (Premise bits).
  Expr buildEq(const Expr &l, const Expr &r, unsigned &depth,
               unsigned &resolved, bool diffBase, bool &reduced,
               unsigned &premises, bool shadow = false, bool ptrLeaf = false) {
    auto branches = [&](const Expr &c, const Expr &lt, const Expr &rt,
                        const Expr &le, const Expr &re) {
      unsigned d1 = 0, d2 = 0;
      Expr t = buildEq(lt, rt, d1, resolved, diffBase, reduced, premises,
                       shadow, ptrLeaf);
      Expr e = buildEq(le, re, d2, resolved, diffBase, reduced, premises,
                       shadow, ptrLeaf);
      depth = 1 + std::max(d1, d2);
      return mk<ITE>(c, t, e);
    };
    if (isOpX<ITE>(l) && isOpX<ITE>(r) && l->arg(0) == r->arg(0))
      return branches(l->arg(0), l->arg(1), r->arg(1), l->arg(2), r->arg(2));
    if (isOpX<ITE>(l))
      return branches(l->arg(0), l->arg(1), r, l->arg(2), r);
    if (isOpX<ITE>(r))
      return branches(r->arg(0), l, r->arg(1), l, r->arg(2));
    depth = 0;
    Decision d;
    Tri t = decidePair(l, r, d, diffBase, shadow);
    if (t != Tri::Unknown) {
      ++resolved;
      countReasons(d);
      premises |= d.premises;
      return t == Tri::Yes ? mk<TRUE>(m_efac) : mk<FALSE>(m_efac);
    }
    // not decided, but simpler: the same-base and cancellation reductions of
    // eq() (their results are rewritten by the depth of the tree: see eq())
    std::optional<PtrView> pl = view(l), pr = view(r);
    if (pl && pr && pl->base == pr->base) {
      ++resolved;
      reduced = true;
      m_stats.count("cmp.same-base");
      return mk<EQ>(offSum(pl->off), offSum(pr->off));
    }
    Expr cl, cr;
    if (cancel(l, r, cl, cr)) {
      ++resolved;
      reduced = true;
      m_stats.count("cmp.cancel");
      return mk<EQ>(cl, cr);
    }
    // an undecided leaf of a pointer compare stays a pointer compare (S2d)
    return ptrLeaf ? mk<PTR_EQ>(l, r) : mk<EQ>(l, r);
  }

  /// Fig. 2.2, 4.2: `a = b`. Kids are normal. In order:
  ///  - decideEq: Yes / No (cmp.*): RW_DONE, true / false.
  ///  - same base, offsets unresolved: ptr(b, o1) = ptr(b, o2) ~> o1 = o2
  ///    (cmp.same-base). No assumption. RW_1: the new `=` and, below it, the
  ///    offsets, which are normal (parts of a normalized bvadd).
  ///  - ITE-EQ (Fig. 2.1) when at most maxEqLeaves leaf pairs and one of them
  ///    resolves (cmp.ite-push): ite(c, x = w, y = w) with the resolved
  ///    leaves already true / false. No assumption beyond those of the
  ///    resolved leaves. RW_d, d the depth of the tree built: the ites are at
  ///    levels 0..d-1 and the compares at level d; their kids are kids of the
  ///    input, normal. A leaf reduced to a compare of new sums (cmp.same-base,
  ///    cmp.cancel) makes it RW_(d+1): the sums are at level d+1 and may be
  ///    redexes (a sum that lost an ite factor can fall under maxPullLeaves).
  ///  - cancellation (cmp.cancel): a + x = a + y ~> x = y, valid for any
  ///    bit-vectors (Fig. 2.2 first rule, with +). RW_1: the new `=` and its
  ///    sums (sub-sequences of normalized sums).
  RewriteResult eq(const Expr &exp, bool ptrLeaf = false) {
    const Expr a = exp->left(), b = exp->right();
    Decision d;
    Tri t = decideEq(a, b, d, /*diffBase=*/false);
    if (t != Tri::Unknown) {
      // a program compare: no premise (diffBase is false)
      countReasons(d);
      return {t == Tri::Yes ? mk<TRUE>(m_efac) : mk<FALSE>(m_efac),
              RWStatus::RW_DONE};
    }
    // an observation marker over an ite is read as the ite of the marked
    // leaves (S1.6c: to_int(ite(c, p, q)) = 0 pushes into the ite as
    // ite(c, p, q) = 0 did; the markers are erased at the boundary)
    const Expr ai = obsIte(a, m_opts.maxEqLeaves),
               bi = obsIte(b, m_opts.maxEqLeaves);
    if (!isOpX<ITE>(ai) && !isOpX<ITE>(bi)) {
      std::optional<PtrView> pa = view(a), pb = view(b);
      if (pa && pb && pa->base == pb->base) {
        m_stats.count("cmp.same-base");
        return {mk<EQ>(offSum(pa->off), offSum(pb->off)), RWStatus::RW_1};
      }
    } else if (m_opts.pushEqIte) {
      const Expr a = ai, b = bi;
      llvm::SmallVector<std::pair<Expr, Expr>, 4> pairs;
      if (eqPairs(a, b, pairs, m_opts.maxEqLeaves)) {
        unsigned depth = 0, resolved = 0, premises = PremNone;
        bool reduced = false;
        // resolved leaves are counted by buildEq; roll nothing back if none
        // resolves: the built term is dropped
        RewriteStats scratch;
        std::swap(scratch, m_stats);
        Expr res = buildEq(a, b, depth, resolved, /*diffBase=*/false, reduced,
                           premises, /*shadow=*/false, ptrLeaf);
        std::swap(scratch, m_stats);
        if (resolved) {
          m_stats.merge(scratch);
          m_stats.count("cmp.ite-push");
          countPremises(premises); // none: a program compare
          return {res, levels(depth + (reduced ? 1 : 0))};
        }
      }
    }
    Expr l, r;
    if (cancel(a, b, l, r)) {
      m_stats.count("cmp.cancel");
      return {mk<EQ>(l, r), RWStatus::RW_1};
    }
    return {exp, RWStatus::RW_DONE};
  }

  /// \p e with its observation markers pushed into the ite below them:
  /// M(ite(c, x, y)) as ite(c, M(x), M(y)) (an identity per leaf, sound
  /// under the sort: the ite is at the integer sort), also at the leaves of
  /// an ite tree of at most maxEqLeaves leaves; \p e otherwise.
  Expr obsIte(const Expr &e, unsigned cap) const {
    if (isOpX<ITE>(e)) {
      if (leafCount(e, cap) > cap)
        return e;
      Expr t = obsIte(e->arg(1), cap), f = obsIte(e->arg(2), cap);
      if (t == e->arg(1) && f == e->arg(2))
        return e;
      return mk<ITE>(e->arg(0), t, f);
    }
    if (!op::ptr::isObservation(e))
      return e;
    Expr x = obsIte(e->arg(0), cap);
    if (!isOpX<ITE>(x) || leafCount(x, cap) > cap)
      return e;
    return mk<ITE>(x->arg(0), obsIte(m_efac.mkUnary(e->op(), x->arg(1)), cap),
                   obsIte(m_efac.mkUnary(e->op(), x->arg(2)), cap));
  }

  /// `a != b ~> !(a = b)` (Fig. 2.2). RW_1: the new `!` and, below it, the
  /// `=`; its kids are the normal kids of the input.
  RewriteResult neq(const Expr &exp) {
    m_stats.count("cmp.neq");
    return {mk<NEG>(mk<EQ>(exp->left(), exp->right())), RWStatus::RW_1};
  }

  /// PTR_EQ(p, q), pointer equality (PtrSort S2d): the rules of eq() (the
  /// decisions, cmp.same-base to an integer compare of the offsets, the ite
  /// push with pointer leaves, the cancellation); an undecided compare
  /// stays PTR_EQ, with \p order its sides ordered by id (ptr.eq.order, as
  /// bv.eq.order orders an EQ; symmetric, no assumption). The other
  /// bit-vector compare rules (bv.eq.*) do not apply: the sides are
  /// pointers, not bit-vectors.
  /// IS_OVERLAP(p, 1, q, 1) (D13, S4): decided as PTR_EQ(p, q) (ptrEq's
  /// rules); what stays open stays an alias question (markAlias). Wider
  /// overlaps are left to the solver.
  RewriteResult overlap(const Expr &exp, bool order) {
    if (!op::ptr::isUnitOverlap(exp))
      return {exp, RWStatus::RW_DONE};
    const Expr e = mk<PTR_EQ>(exp->arg(0), exp->arg(2));
    RewriteResult r = ptrEq(e, order);
    if (r.exp == e)
      return {exp, RWStatus::RW_DONE};
    Expr res = markAlias(r.exp);
    // an undecided same-object question becomes a compare of the offsets
    // (no pointer left to mark): recorded for the census
    // (seam.alias.open.same-object), the query unchanged
    if (!isOpX<TRUE>(res) && !isOpX<FALSE>(res) && !isOpX<IS_OVERLAP>(res)) {
      auto va = view(exp->arg(0)), vb = view(exp->arg(2));
      if (va && vb && va->base == vb->base)
        m_aliasReduced.insert(res);
    }
    return {res, r.status};
  }

  /// the offset compares same-object alias questions were reduced to
  /// (overlap() on one object, the W-BYTE-IN guard (j - i) <u w); the
  /// census counts their normal forms in the query (D13)
  const ExprSet &aliasReduced() const { return m_aliasReduced; }
  const ExprSet &memOpOpen() const { return m_memOpOpen; }

  RewriteResult ptrEq(const Expr &exp, bool order) {
    const Expr e = mk<EQ>(exp->arg(0), exp->arg(1));
    RewriteResult r = eq(e, /*ptrLeaf=*/true);
    if (r.exp != e)
      return r;
    if (order && bvnorm::Rules::idLess(exp->arg(1), exp->arg(0))) {
      m_stats.count("ptr.eq.order");
      return {mk<PTR_EQ>(exp->arg(1), exp->arg(0)), RWStatus::RW_1};
    }
    return {exp, RWStatus::RW_DONE};
  }

  //===------------------------------------------------------------------===//
  // normalize: addition
  //===------------------------------------------------------------------===//

  /// Fig. 2.4 ADD-FLATTEN, ADD-COMPRESS; branch ArithmeticRule. bvadd is
  /// flattened one level (the kids are normal, hence flat), its numerals are
  /// folded mod 2^w into one, put last (zero dropped), the registered bases
  /// put first. No assumption: it is bit-vector arithmetic mod 2^w (so no
  /// A-NOWRAP either). RW_DONE: kids are normal and the result is a flat sum.
  ///
  /// Then, if the sum is a pointer term (an argument is a base or an ite with
  /// a base) and one argument is an ite: pull the sum through the first ite
  /// (ITE-PUSH, Fig. 2.1), bounded by maxPullLeaves leaf pairs:
  ///   bvadd(ite(c, x, y), r...) ~> ite(c, bvadd(x, r...), bvadd(y, r...))
  /// No assumption. RW_1: the ite at level 0, the new sums at level 1 (they
  /// are flattened by this rule; a further ite among their arguments is
  /// pulled by that call, which returns its own RW_1).
  RewriteResult add(const Expr &exp) {
    llvm::SmallVector<Expr, 8> terms;
    mpz_class sum = 0;
    unsigned w = 0;
    auto take = [&](const Expr &t) {
      unsigned tw;
      if (op::bv::isBvNum(t, tw)) {
        w = tw;
        sum = sum + numVal(t);
      } else {
        terms.push_back(t);
      }
    };
    for (size_t i = 0; i < exp->arity(); ++i) {
      // the address of a sum / numeral is that sum / numeral (an observation
      // marker over it is read through, S1.6c: to_int(p + k) + j flattens
      // as p + k + j did)
      const Expr &a0 = exp->arg(i);
      const Expr a = op::ptr::stripObservation(a0);
      unsigned aw;
      if (isOpX<BADD>(a)) {
        for (size_t j = 0; j < a->arity(); ++j)
          take(a->arg(j));
      } else {
        // to_int(node) is an atom of the integer sum (S2c)
        if (a0 != a && op::ptr::isPtrIndex(a))
          m_stats.rules["ptr.to-int-atom"]++; // census, not a rewrite
        take(op::bv::isBvNum(a, aw) ? a : a0);
      }
    }
    if (!w)
      w = widthOf(exp);
    sum = modPow2(sum, w);

    // bases first, numeral last; with simplify the others sorted by id
    // (BvRules.hh), else in the order they came (stable)
    if (m_opts.simplify)
      m_bv.sortSum(terms);
    else
      std::stable_partition(terms.begin(), terms.end(),
                            [&](const Expr &t) { return m_bases.isBase(t); });
    llvm::SmallVector<Expr, 8> sumTerms(terms.begin(), terms.end());
    if (sum != 0)
      sumTerms.push_back(num(sum, w));
    Expr flat = sumOf(sumTerms, w);

    if (m_opts.pullIte && sumTerms.size() > 1) {
      // an observation marker over an ite is the ite of the marked leaves
      // (S1.6c, see eq)
      for (Expr &t : sumTerms)
        t = obsIte(t, m_opts.maxPullLeaves);
      unsigned prod = 1;
      size_t k = sumTerms.size();
      for (size_t i = 0; i < sumTerms.size(); ++i)
        if (isOpX<ITE>(sumTerms[i])) {
          if (k == sumTerms.size())
            k = i;
          prod *= leafCount(sumTerms[i], m_opts.maxPullLeaves);
          if (prod > m_opts.maxPullLeaves)
            break;
        }
      if (k != sumTerms.size() && prod <= m_opts.maxPullLeaves) {
        int budget = 16;
        bool pointer = false;
        for (const Expr &t : sumTerms)
          pointer = pointer || ptrish(t, budget);
        if (pointer) {
          const Expr ite = sumTerms[k];
          llvm::SmallVector<Expr, 8> t1(sumTerms.begin(), sumTerms.end());
          llvm::SmallVector<Expr, 8> t2(sumTerms.begin(), sumTerms.end());
          t1[k] = ite->arg(1);
          t2[k] = ite->arg(2);
          m_stats.count("add.pull-ite");
          return {mk<ITE>(ite->arg(0), mknary<BADD>(t1.begin(), t1.end()),
                          mknary<BADD>(t2.begin(), t2.end())),
                  RWStatus::RW_1};
        }
      }
    }
    if (flat != exp)
      m_stats.count("add.normalize");
    return {flat, RWStatus::RW_DONE};
  }

  /// PTR_DIFF(p, q), the difference of two pointers (PtrSort.md 1.6, S1.5):
  ///   p - p ~> 0                         diff.self            no assumption
  ///   (not when widthOf(p) is 0: no rewrite)
  ///   ptr(b, o1) - ptr(b, o2) ~> o1 - o2 diff.same-base       no assumption
  ///   ite(c, P1, P2) - Q ~> ite(c, P1 - Q, P2 - Q) (and mirrored), every
  ///   leaf with Q's base             diff.ite-same-base   no assumption
  ///   otherwise its bvsub (diff.erase), where sub's rules apply; a numeral
  ///   q goes there first, as sub.const comes before sub.same-base in sub.
  /// The same decisions as sub on the bvsub, in the same order: under bv
  /// typing the query is the same (the S1 gate). Under the sort no bvsub
  /// has a pointer argument: sub's base cases read addresses (to_int(p)).
  RewriteResult diff(const Expr &exp) {
    const Expr a = exp->arg(0), b = exp->arg(1);
    if (a == b) {
      // no width (an ill-sorted a): no rewrite, never a width-0 numeral
      const unsigned n = widthOf(a);
      if (n == 0)
        return {exp, RWStatus::RW_DONE};
      m_stats.count("diff.self");
      return {num(0, n), RWStatus::RW_DONE};
    }
    unsigned w;
    if (!op::bv::isBvNum(b, w)) {
      std::optional<PtrView> pa = view(a), pb = view(b);
      if (pa && pb && pa->base == pb->base) {
        m_stats.count("diff.same-base");
        return {mk<BSUB>(offSum(pa->off), offSum(pb->off)), RWStatus::RW_1};
      }
      if (isOpX<ITE>(a) != isOpX<ITE>(b)) {
        const Expr it = isOpX<ITE>(a) ? a : b, other = isOpX<ITE>(a) ? b : a;
        std::optional<PtrView> po = isOpX<ITE>(a) ? pb : pa;
        if (po && !m_bases.lookup(it) && iteLeavesWithBase(it, po->base)) {
          m_stats.count("diff.ite-same-base");
          auto d = [&](const Expr &leaf) {
            return isOpX<ITE>(a) ? mk<PTR_DIFF>(leaf, other)
                                 : mk<PTR_DIFF>(other, leaf);
          };
          return {mk<ITE>(it->arg(0), d(it->arg(1)), d(it->arg(2))),
                  RWStatus::RW_2};
        }
      }
    }
    m_stats.count("diff.erase");
    // the difference of the addresses (ADDR, S2d; a numeral q stays bare
    // for sub.const until the sort)
    return {mk<BSUB>(mk<ADDR>(a), op::bv::isBvNum(b, w) ? b : mk<ADDR>(b)),
            RWStatus::RW_1};
  }

  /// PTR_FROM_BITS(x), a loaded pointer (PtrSort.md 1.7, S1.6b): its
  /// provenance is the stored pointer's (ptr.load-resolve, address-exact:
  /// the result has the loaded address, and the provenance is taken from the
  /// syntactic store; from_bits(to_bits(p)) is p's address, not p's identity
  /// (PtrSort D3; pointer identity through
  /// memory is the companion's, --ptr-companion / A-PTRINT)):
  ///   from_bits(to_bits(p)) ~> p
  ///   from_bits(c), c a numeral, is kept (S2e: from_bits(c) ~> c is
  ///   ill-typed; decidePair compares numerals by value)
  ///   from_bits(ite(g, x, y)) ~> ite(g, from_bits(x), from_bits(y)) when a
  ///   leaf resolves (at most maxPullLeaves leaves; leaves that do not keep
  ///   their from_bits)
  /// No assumption. Otherwise unchanged (no view: a loaded pointer whose
  /// store is unknown).
  RewriteResult loadResolve(const Expr &exp) {
    const Expr x = exp->arg(0);
    unsigned w;
    if (isOpX<PTR_TO_BITS>(x)) {
      m_stats.count("ptr.load-resolve");
      return {x->arg(0), RWStatus::RW_DONE};
    }
    // a loaded numeral stays from_bits(c): a pointer built from a number
    // (S2e: from_bits(c) ~> c is ill-typed; decidePair compares numeral
    // constructors by value, ARM reads them as the absolute point c)
    if (op::bv::isBvNum(x, w))
      return {exp, RWStatus::RW_DONE};
    if (m_opts.leafSplit && isOpX<BCONCAT>(x))
      if (auto r = loadResolveLeaves(exp))
        return {r, RWStatus::RW_FULL};
    if (m_opts.leafSplit && isOpX<ITE>(x) && !m_bases.lookup(x))
      if (auto r = loadResolveLeafIte(exp))
        return {r, RWStatus::RW_FULL};
    if (m_opts.pullConcatIte && isOpX<BCONCAT>(x))
      if (auto r = loadResolveConcat(exp))
        return {r, RWStatus::RW_FULL};
    if (m_opts.resolveIteNodes && isOpX<ITE>(x) && !m_bases.lookup(x) &&
        leafCount(x, m_opts.maxPullLeaves) > m_opts.maxPullLeaves)
      return loadResolveDag(exp);
    if (!isOpX<ITE>(x) || m_bases.lookup(x) ||
        leafCount(x, m_opts.maxPullLeaves) > m_opts.maxPullLeaves)
      return {exp, RWStatus::RW_DONE};
    unsigned resolved = 0, depth = 0;
    std::function<Expr(const Expr &, unsigned)> push = [&](const Expr &e,
                                                           unsigned d) -> Expr {
      if (isOpX<ITE>(e) && !m_bases.lookup(e)) {
        depth = std::max(depth, d + 1);
        return mk<ITE>(e->arg(0), push(e->arg(1), d + 1),
                       push(e->arg(2), d + 1));
      }
      if (isOpX<PTR_TO_BITS>(e)) {
        ++resolved;
        return e->arg(0);
      }
      return mk<PTR_FROM_BITS>(e);
    };
    Expr res = push(x, 0);
    if (resolved == 0)
      return {exp, RWStatus::RW_DONE};
    m_stats.count("ptr.load-resolve-ite");
    return {res, levels(depth + 1)};
  }

  /// The pointer p whose bits \p bytes (msb first) are: to_bits(p) whole,
  /// or its contiguous extracts from the lsb up, of the loaded width \p w
  /// (a to_bits of another width is not the loaded pointer). Null if none.
  Expr storedPointer(llvm::ArrayRef<Expr> bytes, unsigned w) {
    Expr p;
    unsigned next = 0; // the next expected low bit, from the lsb
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
      const Expr &b = *it;
      Expr arg;
      unsigned hi, lo;
      if (isOpX<PTR_TO_BITS>(b)) {
        arg = b;
        lo = 0;
        hi = w - 1;
      } else if (isOpX<BEXTRACT>(b) && isOpX<PTR_TO_BITS>(op::bv::earg(b))) {
        arg = op::bv::earg(b);
        hi = op::bv::high(b);
        lo = op::bv::low(b);
      } else {
        return Expr();
      }
      if (widthOf(arg) != w || lo != next || (p && arg != p))
        return Expr();
      p = arg;
      next = hi + 1;
    }
    if (!p || next != w)
      return Expr();
    return p->arg(0);
  }

  /// ptr.load-resolve through a split load whose bytes share one ite spine
  /// (R-LEAVES' result for the bytes of one load: each byte read at the
  /// leaves of the same address ite): concat(b_1, ..., b_n) (msb first,
  /// nested concats flattened) is split on c when every byte whose top is
  /// an ite on a condition of an address leafSplit split is ite(c, ., .) (a
  /// byte with another top goes to both sides: a value, or the residual read
  /// of a partial split; other shared conditions, a join's, are
  /// --seam-pull-concat's), until no byte has one at its top: the ite of
  /// from_bits(concat(leaf bytes)), and
  /// a leaf whose bytes are the contiguous extracts of one to_bits(p) is p.
  /// All or nothing: at most maxDecideLeaves distinct leaves (the bytes are
  /// ite DAGs: each byte tuple is split once, memoized), every split on a
  /// condition all ite bytes share at their top, some leaf a stored
  /// pointer. Address-exact, no assumption (counter
  /// ptr.load-resolve-leaves).
  Expr loadResolveLeaves(const Expr &exp) {
    const Expr x = exp->arg(0);
    using Bytes = llvm::SmallVector<Expr, 8>;
    std::function<void(const Expr &, Bytes &)> flat = [&](const Expr &e,
                                                          Bytes &out) {
      if (isOpX<BCONCAT>(e)) {
        for (size_t i = 0; i < e->arity(); ++i)
          flat(e->arg(i), out);
      } else {
        out.push_back(e);
      }
    };
    Bytes bytes;
    flat(x, bytes);
    if (llvm::none_of(bytes, [](const Expr &b) { return isOpX<ITE>(b); }))
      return Expr();
    const unsigned w = widthOf(x);
    unsigned leaves = 0, resolved = 0;
    bool ok = true;
    std::map<std::vector<ENode *>, Expr> memo;
    std::function<Expr(const Bytes &)> split = [&](const Bytes &bs) -> Expr {
      if (!ok)
        return Expr();
      std::vector<ENode *> key;
      for (const Expr &b : bs)
        key.push_back(b.get());
      auto f = memo.find(key);
      if (f != memo.end())
        return f->second;
      // the split condition: the leaf condition at the top of the bytes
      // that have one (a byte with another top, e.g. the residual read of a
      // partial split, goes to both sides)
      Expr c;
      for (const Expr &b : bs)
        if (isOpX<ITE>(b) && m_leafConds.count(b->arg(0))) {
          if (c && b->arg(0) != c) {
            ok = false; // the bytes do not share one spine
            return Expr();
          }
          c = b->arg(0);
        }
      Expr res;
      if (!c) {
        if (++leaves > m_opts.maxDecideLeaves) {
          ok = false;
          return Expr();
        }
        if (Expr p = storedPointer(bs, w)) {
          ++resolved;
          res = p;
        } else {
          res = mk<PTR_FROM_BITS>(
              bs.size() == 1 ? bs[0] : mknary<BCONCAT>(bs.begin(), bs.end()));
        }
      } else {
        Bytes t, e;
        for (const Expr &b : bs) {
          const bool at = isOpX<ITE>(b) && b->arg(0) == c;
          t.push_back(at ? b->arg(1) : b);
          e.push_back(at ? b->arg(2) : b);
        }
        Expr l = split(t);
        Expr r = split(e);
        if (!ok)
          return Expr();
        res = mk<ITE>(c, l, r);
      }
      memo.emplace(std::move(key), res);
      return res;
    };
    Expr res = split(bytes);
    if (!ok || !res || !resolved)
      return Expr();
    m_stats.count("ptr.load-resolve-leaves");
    return res;
  }

  /// ptr.load-resolve through a whole load at an address R-LEAVES split
  /// (a wide read: the ite of the leaf values): from_bits(ite(c, x, y)) ~>
  /// ite(c, from_bits(x), from_bits(y)) over the ite's DAG (memoized), when
  /// every ite node's condition is a condition of an address leafSplit
  /// split, it has at most maxDecideLeaves distinct leaves and some leaf is a
  /// stored pointer's to_bits (that leaf is the pointer). The loadResolve
  /// leaf bound (maxPullLeaves tree leaves) does not apply: the ite is the
  /// split address's shape. Address-exact, no assumption (counter
  /// ptr.load-resolve-leaves).
  Expr loadResolveLeafIte(const Expr &exp) {
    const Expr x = exp->arg(0);
    const unsigned w = widthOf(x);
    std::unordered_set<ENode *> ites, leaves;
    bool resolved = false;
    ExprVector stack{x};
    while (!stack.empty()) {
      Expr e = stack.back();
      stack.pop_back();
      if (isOpX<ITE>(e) && !m_bases.lookup(e)) {
        if (!ites.insert(e.get()).second)
          continue;
        if (!m_leafConds.count(e->arg(0)))
          return Expr();
        stack.push_back(e->arg(1));
        stack.push_back(e->arg(2));
        continue;
      }
      if (leaves.insert(e.get()).second &&
          leaves.size() > m_opts.maxDecideLeaves)
        return Expr();
      resolved = resolved || (isOpX<PTR_TO_BITS>(e) && widthOf(e) == w);
    }
    if (!resolved)
      return Expr();
    std::unordered_map<ENode *, Expr> memo;
    std::function<Expr(const Expr &)> map = [&](const Expr &e) -> Expr {
      if (ites.count(e.get())) {
        auto f = memo.find(e.get());
        if (f != memo.end())
          return f->second;
        Expr r = mk<ITE>(e->arg(0), map(e->arg(1)), map(e->arg(2)));
        memo.emplace(e.get(), r);
        return r;
      }
      if (isOpX<PTR_TO_BITS>(e) && widthOf(e) == w)
        return e->arg(0);
      return mk<PTR_FROM_BITS>(e);
    };
    Expr res = map(x);
    m_stats.count("ptr.load-resolve-leaves");
    return res;
  }

  /// ptr.load-resolve through a split load (pullConcatIte, S6 probe): the
  /// bytes of x = concat(b_1, ..., b_n) (msb first, nested concats
  /// flattened) are split on the condition c of the first byte that is an
  /// ite: ite(c, from(then-bytes), from(else-bytes)), where each byte is
  /// taken under c = true / false (every ite(c, t, e) along its ite spine
  /// is t / e; the stores of one pointer under a join repeat c inside the
  /// branches) and other bytes go to both sides; recursively, at
  /// most pullConcatIte splits. A side whose bytes are the contiguous
  /// extracts of one to_bits(p) (msb to lsb, the full width) is p. Kept
  /// only if some side is; the other sides are from_bits(concat(...)) and
  /// are not split again (m_concatTried). Address-exact, no assumption.
  Expr loadResolveConcat(const Expr &exp) {
    const Expr x = exp->arg(0);
    if (m_concatTried.count(x.get()))
      return Expr();
    m_concatTried.emplace(x.get(), x);
    using Bytes = llvm::SmallVector<Expr, 8>;
    std::function<void(const Expr &, Bytes &)> flat = [&](const Expr &e,
                                                          Bytes &out) {
      if (isOpX<BCONCAT>(e)) {
        for (size_t i = 0; i < e->arity(); ++i)
          flat(e->arg(i), out);
      } else {
        out.push_back(e);
      }
    };
    Bytes bytes;
    flat(x, bytes);
    const unsigned w = widthOf(x);
    unsigned pulls = 0, resolved = 0;
    std::function<Expr(const Bytes &)> split = [&](const Bytes &bs) -> Expr {
      if (Expr p = storedPointer(bs, w)) {
        ++resolved;
        return p;
      }
      Expr c;
      for (const Expr &b : bs)
        if (isOpX<ITE>(b)) {
          c = b->arg(0);
          break;
        }
      if (!c || pulls >= m_opts.pullConcatIte) {
        Expr leaf =
            bs.size() == 1 ? bs[0] : mknary<BCONCAT>(bs.begin(), bs.end());
        m_concatTried.emplace(leaf.get(), leaf);
        return mk<PTR_FROM_BITS>(leaf);
      }
      ++pulls;
      // a byte under c = val: every ite(c, ., .) along its ite spine
      // (through ites only) takes its side
      std::unordered_map<ENode *, Expr> memo[2];
      std::function<Expr(const Expr &, bool)> under = [&](const Expr &b,
                                                          bool val) -> Expr {
        if (!isOpX<ITE>(b))
          return b;
        auto it = memo[val].find(b.get());
        if (it != memo[val].end())
          return it->second;
        Expr r = b->arg(0) == c ? under(b->arg(val ? 1 : 2), val)
                                : Expr(mk<ITE>(b->arg(0), under(b->arg(1), val),
                                               under(b->arg(2), val)));
        memo[val].emplace(b.get(), r);
        return r;
      };
      Bytes t, e;
      for (const Expr &b : bs) {
        t.push_back(under(b, true));
        e.push_back(under(b, false));
      }
      return mk<ITE>(c, split(t), split(e));
    };
    Expr res = split(bytes);
    if (!resolved)
      return Expr();
    m_stats.count("ptr.load-resolve-concat");
    m_stats.rules["ptr.load-resolve-concat.pulls"] += pulls;
    return res;
  }

  /// ptr.load-resolve through an ite over its DAG (resolveIteNodes): the
  /// same map as loadResolve's (ite(g, x, y) -> ite(g, map x, map y),
  /// to_bits(p) -> p, any other leaf -> from_bits(leaf)), memoized per ite
  /// node, when the ite has at most resolveIteNodes distinct ite nodes and a
  /// leaf resolves. Address-exact, no assumption.
  RewriteResult loadResolveDag(const Expr &exp) {
    const Expr x = exp->arg(0);
    // the distinct ite nodes, bounded
    std::unordered_set<ENode *> ites;
    std::vector<ENode *> stack{x.get()};
    bool anyResolved = false;
    while (!stack.empty()) {
      ENode *e = stack.back();
      stack.pop_back();
      if (isOpX<ITE>(e) && !m_bases.lookup(Expr(e))) {
        if (!ites.insert(e).second)
          continue;
        if (ites.size() > m_opts.resolveIteNodes)
          return {exp, RWStatus::RW_DONE};
        stack.push_back(e->arg(1));
        stack.push_back(e->arg(2));
      } else if (isOpX<PTR_TO_BITS>(e)) {
        anyResolved = true;
      }
    }
    if (!anyResolved)
      return {exp, RWStatus::RW_DONE};
    std::unordered_map<ENode *, Expr> memo;
    std::function<Expr(const Expr &)> map = [&](const Expr &e) -> Expr {
      if (isOpX<ITE>(e) && ites.count(e.get())) {
        auto it = memo.find(e.get());
        if (it != memo.end())
          return it->second;
        Expr r = mk<ITE>(e->arg(0), map(e->arg(1)), map(e->arg(2)));
        memo.emplace(e.get(), r);
        return r;
      }
      if (isOpX<PTR_TO_BITS>(e))
        return e->arg(0);
      return mk<PTR_FROM_BITS>(e);
    };
    Expr res = map(x);
    m_stats.count("ptr.load-resolve-ite-dag");
    // the new ites (their rules: ite.same, the compare push) and the new
    // from_bits leaves (normal: loadResolve leaves them)
    return {res, RWStatus::RW_FULL};
  }

  /// bvsub (Fig. 4.3 -p; Fig. 2.2 for the numeral case), arity 2:
  ///   a - a ~> 0                      sub.self        no assumption (not
  ///                                   when widthOf(a) is 0: no rewrite)
  ///   a - c ~> a + (-c), c numeral    sub.const       no assumption (mod 2^w)
  ///   ptr(b, o1) - ptr(b, o2) ~> o1 - o2   sub.same-base   no assumption
  /// RW_1 for the last two: the new bvadd / bvsub is at level 0 and its kids
  /// (normal kids of the input) need no rewrite.
  RewriteResult sub(const Expr &exp) {
    if (exp->arity() != 2)
      return {exp, RWStatus::RW_DONE};
    const Expr a = exp->arg(0), b = exp->arg(1);
    if (a == b) {
      // no width (an ill-sorted a): no rewrite, never a width-0 numeral
      const unsigned n = widthOf(a);
      if (n == 0)
        return {exp, RWStatus::RW_DONE};
      m_stats.count("sub.self");
      return {num(0, n), RWStatus::RW_DONE};
    }
    unsigned w;
    if (op::bv::isBvNum(b, w)) {
      m_stats.count("sub.const");
      if (m_opts.plantBug ==
          SeaMOptions::PlantBug::SubConst) // test hook: wrong
        return {mk<BADD>(a, num(numVal(b), w)), RWStatus::RW_1};
      // a bvsub is integer arithmetic (S2e): its result is a sum, never a
      // node, also over an address (to_int(p) - c is to_int(p) + (-c))
      return {mk<BADD>(a, num(mpz_class(0UL) - numVal(b), w)), RWStatus::RW_1};
    }
    std::optional<PtrView> pa = view(a), pb = view(b);
    if (pa && pb && pa->base == pb->base) {
      m_stats.count("sub.same-base");
      return {mk<BSUB>(offSum(pa->off), offSum(pb->off)), RWStatus::RW_1};
    }
    // sub.ite-same-base: a difference of pointers into one object, one side
    // an ite of such pointers (every leaf, at most maxPullLeaves, with the
    // other side's base): pushed through the ite, so that each leaf cancels
    // the base (sub.same-base) and the difference is base-free:
    //   ite(c, A + x, A + y) - (A + z)  ~>  ite(c, (A + x) - (A + z), ...)
    // Exact (bvsub distributes over ite), no premise. The translated address
    // of a read through a copy, s + (j - d), keeps its object this way
    // (docs/design/SeaM-provenance.md, "Where provenance is lost").
    if (isOpX<ITE>(a) != isOpX<ITE>(b)) {
      const Expr it = isOpX<ITE>(a) ? a : b, other = isOpX<ITE>(a) ? b : a;
      std::optional<PtrView> po = isOpX<ITE>(a) ? pb : pa;
      if (po && !m_bases.lookup(it) && iteLeavesWithBase(it, po->base)) {
        m_stats.count("sub.ite-same-base");
        const Expr c = it->arg(0);
        Expr t = isOpX<ITE>(a) ? mk<BSUB>(it->arg(1), other)
                               : mk<BSUB>(other, it->arg(1));
        Expr e = isOpX<ITE>(a) ? mk<BSUB>(it->arg(2), other)
                               : mk<BSUB>(other, it->arg(2));
        return {mk<ITE>(c, t, e), RWStatus::RW_2};
      }
    }
    return {exp, RWStatus::RW_DONE};
  }

  /// Is every leaf of the ite tree \p it (ites that are not registered
  /// bases; at most maxPullLeaves leaves) a pointer into \p base?
  bool iteLeavesWithBase(const Expr &it, const Expr &base) const {
    llvm::SmallVector<Expr, 8> stack{it};
    unsigned leaves = 0;
    while (!stack.empty()) {
      Expr p = stack.pop_back_val();
      if (isOpX<ITE>(p) && !m_bases.lookup(p)) {
        stack.push_back(p->arg(1));
        stack.push_back(p->arg(2));
        continue;
      }
      if (++leaves > m_opts.maxPullLeaves)
        return false;
      std::optional<PtrView> v = view(p);
      if (!v || v->base != base)
        return false;
    }
    return true;
  }

  //===------------------------------------------------------------------===//
  // ite and Boolean rules
  //===------------------------------------------------------------------===//

  static bool areNegations(const Expr &x, const Expr &y) {
    return (isOpX<NEG>(x) && x->left() == y) ||
           (isOpX<NEG>(y) && y->left() == x);
  }

  /// Fig. 2.1 (and the branch's ITERewriteRule). No assumption.
  ///   ite(true, t, e) ~> t, ite(false, t, e) ~> e, ite(c, a, a) ~> a:
  ///     RW_DONE (the result is a kid, normal).
  ///   ite(c, true, false) ~> c: RW_DONE.
  ///   ite(c, false, true) ~> !c: RW_1: `!` at level 0 over the normal kid c
  ///     (!!x is removed by the NEG rule at the root).
  ///   ite(a, b, ite(!a, c, d)) ~> ite(a, b, c): RW_1: the new ite at level
  ///     0 over normal kids; its rules (the cases above) apply at the root.
  RewriteResult ite(const Expr &exp) {
    const Expr c = exp->arg(0), t = exp->arg(1), e = exp->arg(2);
    if (isOpX<TRUE>(c)) {
      m_stats.count("ite.true");
      return {t, RWStatus::RW_DONE};
    }
    if (isOpX<FALSE>(c)) {
      m_stats.count("ite.false");
      return {e, RWStatus::RW_DONE};
    }
    if (sameAddr(t, e)) {
      m_stats.count("ite.same");
      return {t, RWStatus::RW_DONE};
    }
    if (isOpX<TRUE>(t) && isOpX<FALSE>(e)) {
      m_stats.count("ite.bool");
      return {c, RWStatus::RW_DONE};
    }
    if (isOpX<FALSE>(t) && isOpX<TRUE>(e)) {
      m_stats.count("ite.bool");
      return {mk<NEG>(c), RWStatus::RW_1};
    }
    if (isOpX<ITE>(e) && areNegations(c, e->arg(0))) {
      m_stats.count("ite.neg-cond");
      return {mk<ITE>(c, t, e->arg(1)), RWStatus::RW_1};
    }
    return {exp, RWStatus::RW_DONE};
  }

  /// Fig. 2.3. No assumption.
  ///   !true ~> false, !false ~> true, !!a ~> a: RW_DONE.
  ///   NEG-PUSH !ite(c, x, y) ~> ite(c, !x, !y), only when a branch is a
  ///     Boolean constant or a negation (so that the push simplifies; the
  ///     thesis pushes always): RW_1: the ite at level 0, the `!` at level 1
  ///     over normal kids; a further push is that call's RW_1.
  RewriteResult neg(const Expr &exp) {
    const Expr a = exp->left();
    if (isOpX<TRUE>(a)) {
      m_stats.count("bool.neg");
      return {mk<FALSE>(m_efac), RWStatus::RW_DONE};
    }
    if (isOpX<FALSE>(a)) {
      m_stats.count("bool.neg");
      return {mk<TRUE>(m_efac), RWStatus::RW_DONE};
    }
    if (isOpX<NEG>(a)) {
      m_stats.count("bool.neg");
      return {a->left(), RWStatus::RW_DONE};
    }
    if (isOpX<ITE>(a)) {
      auto simple = [](const Expr &x) {
        return isOpX<TRUE>(x) || isOpX<FALSE>(x) || isOpX<NEG>(x);
      };
      if (simple(a->arg(1)) || simple(a->arg(2))) {
        m_stats.count("bool.neg-ite");
        return {mk<ITE>(a->arg(0), mk<NEG>(a->arg(1)), mk<NEG>(a->arg(2))),
                RWStatus::RW_1};
      }
    }
    return {exp, RWStatus::RW_DONE};
  }

  /// and / or (not in the thesis' figures; the branch has no rule): a unit
  /// (true for and, false for or) is dropped, a zero (false for and, true for
  /// or) is the result, duplicates are dropped. No assumption. RW_DONE: kids
  /// are normal.
  RewriteResult andOr(const Expr &exp, bool isAnd) {
    llvm::SmallVector<Expr, 4> kept;
    bool changed = false;
    for (size_t i = 0; i < exp->arity(); ++i) {
      const Expr &a = exp->arg(i);
      if (isAnd ? isOpX<FALSE>(a) : isOpX<TRUE>(a)) {
        m_stats.count(isAnd ? "bool.and" : "bool.or");
        return {a, RWStatus::RW_DONE};
      }
      if ((isAnd ? isOpX<TRUE>(a) : isOpX<FALSE>(a)) ||
          std::find(kept.begin(), kept.end(), a) != kept.end()) {
        changed = true;
        continue;
      }
      kept.push_back(a);
    }
    if (!changed)
      return {exp, RWStatus::RW_DONE};
    m_stats.count(isAnd ? "bool.and" : "bool.or");
    if (kept.empty())
      return {isAnd ? Expr(mk<TRUE>(m_efac)) : Expr(mk<FALSE>(m_efac)),
              RWStatus::RW_DONE};
    if (kept.size() == 1)
      return {kept.front(), RWStatus::RW_DONE};
    return {isAnd ? mknary<AND>(kept.begin(), kept.end())
                  : mknary<OR>(kept.begin(), kept.end()),
            RWStatus::RW_DONE};
  }

  //===------------------------------------------------------------------===//
  // write over write: Store-Map (thesis Ch. 6, Fig. 6.4)
  //===------------------------------------------------------------------===//

  /// Two pointers into different registered objects, under A-INB: the
  /// addresses of two accesses (of positive width) through them differ. Both
  /// objects of nonzero size: the test of cmp.diff-base; one of size 0 (null,
  /// a zero-sized allocation): no such access is in it, the pair is vacuous
  /// (cmp.size0-vacuous, SeaM-null.md; \p vacuous is set). uses: A-INB.
  bool distinctObjects(const Expr &b1, const Expr &b2,
                       bool *vacuous = nullptr) const {
    if (!m_opts.assumeInBounds || b1 == b2)
      return false;
    const ObjectBase *o1 = m_bases.lookup(b1);
    const ObjectBase *o2 = m_bases.lookup(b2);
    if (!o1 || !o2)
      return false;
    if (vacuous && (o1->size.isZero() || o2->size.isZero()))
      *vacuous = true;
    return true;
  }

  /// distinctObjects for a rule that counts its reasons itself: counts
  /// cmp.size0-vacuous when that decided it.
  bool distinctObjectsCounted(const Expr &b1, const Expr &b2) {
    bool vacuous = false;
    if (!distinctObjects(b1, b2, &vacuous))
      return false;
    if (vacuous)
      m_stats.count("cmp.size0-vacuous");
    return true;
  }

  /// The pointer base + offset, in the normal form of `add`.
  Expr ptrAt(const Expr &base, const Expr &offset) const {
    // a node (PtrSort rev 5, S1.2), read through its flat form
    return numVal(offset) == 0 ? base : op::ptr::addressPlus(base, offset);
  }

  /// WOW-COMMUTE / WOSMAP-COMMUTE need the write to go below one to a *later*
  /// allocated object: the head of a normal chain is the latest object, the
  /// order the thesis' `<t` (allocation order, from the registry).
  /// uses: A-INB.
  bool commutes(const Expr &outer, const Expr &inner) {
    return m_opts.assumeInBounds && outer != inner && m_bases.lookup(outer) &&
           m_bases.lookup(inner) &&
           m_bases.lookup(outer)->order < m_bases.lookup(inner)->order &&
           distinctObjectsCounted(outer, inner);
  }

  /// WOW-COMMUTE in a shadow memory (\p i, \p j: the two store addresses,
  /// pointer views \p vi, \p vj): no A-INB; both are object bases (offset
  /// 0) whose placed numerals differ (shadow.distinct-base, uses: LAYOUT),
  /// and the object of i is allocated earlier (the order of the chain).
  bool shadowCommutes(const PtrView &vi, const PtrView &vj) {
    if (!(m_bases.lookup(vi.base) && m_bases.lookup(vj.base) &&
          m_bases.lookup(vi.base)->order < m_bases.lookup(vj.base)->order))
      return false;
    Decision d;
    if (decidePair(vi.base, vj.base, d, /*diffBase=*/false, /*shadow=*/true) !=
            Tri::No ||
        !numVal0(vi.off) || !numVal0(vj.off))
      return false;
    countReasons(d);
    return true;
  }

  /// Fig. 6.4, at the encoding of a store `store(a, i, v)` (kids normal, so
  /// `a` is a normal memory). Needs a pointer view of `i`; without one (a
  /// loaded pointer, an ite of pointers) the store stays: it is a barrier, as
  /// in the thesis. In order:
  ///  - `a` = store(a0, j, w):
  ///    * i = j (the same term, so also equal numeral offsets of one base):
  ///      store(a0, i, v)                                       wow.collapse
  ///      No assumption. RW_1: the new store is at level 0 and may meet a
  ///      store or store-map below it (a0 is normal).
  ///    * same base, numeral offsets o1 (of i) != o2 (of j): SMAP-NEW
  ///      store-map(a0, base, {o2 -> w, o1 -> v})                 smap.new
  ///      No assumption (the offsets are distinct numerals). RW_DONE: a0 is
  ///      normal, and a normal `a` has a0 with no write to a later object
  ///      beneath it (that would have commuted), so the store-map is in order.
  ///    * different objects and the object of i allocated earlier than that of
  ///      j: WOW-COMMUTE
  ///      store(store(a0, i, v), j, w)                           wow.commute
  ///      uses: A-INB (i and j differ). RW_1: the root at level 0, the inner
  ///      store at level 1 (its own rules, and its own chain), a0, i, v normal.
  ///  - `a` = store-map(a0, b, ocl):
  ///    * i = b + o, o a numeral: SMAP-HIT, the entry inserted
  ///      store-map(a0, b, ocl + (o -> v))                      smap.insert
  ///      (smap.noop: a itself, when o already holds v). No assumption.
  ///      RW_DONE.
  ///    * different objects, the object of i earlier than b: WOSMAP-COMMUTE
  ///      store-map(store(a0, i, v), b, ocl)                    smap.commute
  ///      uses: A-INB (i is not any b + o). RW_1: the new store-map is the root
  ///      (not rewritten by shouldRewrite, but the follow-up frame is pushed),
  ///      its array, the store, at level 1; a0, i, v are normal. The ordered
  ///      map belongs to ocl, so it needs no transfer.
  ///  - otherwise the store is final (RW_DONE).
  /// In a shadow memory (shadowMemory: SeaM-null.md, "Shadow memories") no
  /// rule uses A-INB: WOW-COMMUTE needs two bases at offset 0
  /// (shadowCommutes, uses: LAYOUT), WOSMAP-COMMUTE is not applied.
  RewriteResult wow(const Expr &exp) {
    if (std::optional<RewriteResult> r = pruneAccess(exp))
      return *r;
    const Expr a = exp->arg(0), i = exp->arg(1), v = exp->arg(2);
    if (!m_opts.storeMap)
      return {exp, RWStatus::RW_DONE};
    const bool shadow = shadowMemory(a);
    if (isOpX<STORE>(a)) {
      const Expr a0 = a->arg(0), j = a->arg(1), w = a->arg(2);
      if (sameAddr(i, j)) {
        m_stats.count("wow.collapse");
        return {op::array::store(a0, i, v), RWStatus::RW_1};
      }
      std::optional<PtrView> vi = view(i), vj = view(j);
      if (!vi || !vj)
        return {exp, RWStatus::RW_DONE};
      if (vi->base == vj->base) {
        mpz_class x, y;
        if (!numerals(vi->off, vj->off, x, y))
          return {exp, RWStatus::RW_DONE}; // a symbolic offset: a barrier
        if (x == y) { // not reached for normal terms (i == j above)
          m_stats.count("wow.collapse");
          return {op::array::store(a0, i, v), RWStatus::RW_1};
        }
        m_stats.count("smap.new");
        return {op::array::storeMap(a0, vi->base,
                                    m_smaps.make2(vj->off, w, vi->off, v)),
                RWStatus::RW_DONE};
      }
      if (shadow) {
        if (shadowCommutes(*vi, *vj)) { // uses: LAYOUT
          m_stats.count("wow.commute");
          countPremises(PremLayout);
          return {op::array::store(op::array::store(a0, i, v), j, w),
                  RWStatus::RW_1};
        }
        return {exp, RWStatus::RW_DONE};
      }
      if (commutes(vi->base, vj->base)) { // uses: A-INB
        m_stats.count("wow.commute");
        countPremises(PremAInb);
        return {op::array::store(op::array::store(a0, i, v), j, w),
                RWStatus::RW_1};
      }
      return {exp, RWStatus::RW_DONE};
    }
    if (isOpX<STORE_MAP>(a)) {
      const Expr a0 = a->arg(0), b = a->arg(1), ocl = a->arg(2);
      std::optional<PtrView> vi = view(i);
      if (!vi)
        return {exp, RWStatus::RW_DONE};
      if (vi->base == b) {
        unsigned nw;
        if (!op::bv::isBvNum(vi->off, nw))
          return {exp, RWStatus::RW_DONE};
        Expr old = m_smaps.find(ocl, numVal(vi->off));
        if (old && old == v) {
          m_stats.count("smap.noop");
          return {a, RWStatus::RW_DONE};
        }
        m_stats.count("smap.insert");
        return {op::array::storeMap(a0, b, m_smaps.insert(ocl, vi->off, v)),
                RWStatus::RW_DONE};
      }
      if (!shadow && commutes(vi->base, b)) { // uses: A-INB
        m_stats.count("smap.commute");
        countPremises(PremAInb);
        return {op::array::storeMap(op::array::store(a0, i, v), b, ocl),
                RWStatus::RW_1};
      }
    }
    return {exp, RWStatus::RW_DONE};
  }

  //===------------------------------------------------------------------===//
  // read over write
  //===------------------------------------------------------------------===//

  /// The compare of a ROW step between the address \p i of a write and the
  /// read address \p idx: `i = idx`, or, when one is an ite, the compare tree
  /// over the leaves (buildEq: leaves that differ by A-INB are false there).
  /// \p depth / \p reduced as of buildEq (0 / false for the plain compare).
  /// \p premises gets the premises of the leaves it resolved (and the
  /// premise counter is bumped for them). uses: A-INB, through buildEq.
  /// \p shadow: the step is in a shadow memory: no A-INB, the leaves are
  /// decided by object identity (decidePair, shadow.distinct-base).
  Expr rowCompare(const Expr &i, const Expr &idx, unsigned &depth,
                  bool &reduced, unsigned &premises, bool shadow = false) {
    return markAlias(rowCompareEq(i, idx, depth, reduced, premises, shadow));
  }

  /// The pointer compares of a ROW compare as alias questions (D13, S4):
  /// every PTR_EQ leaf of its Boolean structure (ite, and, or, not) is
  /// IS_OVERLAP(p, 1, q, 1), the same value; the rest unchanged.
  Expr markAlias(const Expr &e) const {
    if (isOpX<PTR_EQ>(e))
      return op::ptr::overlap(e->arg(0), 1, e->arg(1), 1);
    if (isOpX<ITE>(e)) {
      Expr t = markAlias(e->arg(1)), f = markAlias(e->arg(2));
      return t == e->arg(1) && f == e->arg(2) ? e : mk<ITE>(e->arg(0), t, f);
    }
    if (isOpX<NEG>(e) || isOpX<AND>(e) || isOpX<OR>(e)) {
      ExprVector kids;
      bool changed = false;
      for (size_t k = 0; k < e->arity(); ++k) {
        kids.push_back(markAlias(e->arg(k)));
        changed |= kids.back() != e->arg(k);
      }
      return changed ? e->efac().mkNary(e->op(), kids) : e;
    }
    return e;
  }

  /// The EQ leaves of a ROW compare tree (ite, Boolean constants and
  /// PTR_EQ leaves aside) go to the census of same-object questions
  /// (aliasReduced).
  void noteReducedLeaves(const Expr &e) {
    if (isOpX<ITE>(e)) {
      noteReducedLeaves(e->arg(1));
      noteReducedLeaves(e->arg(2));
    } else if (isOpX<EQ>(e)) {
      m_aliasReduced.insert(e);
    }
  }

  /// rowCompare's compare with PTR_EQ leaves. The tree is built over as
  /// many leaf pairs as decideEq looks at (maxDecideLeaves, not the
  /// program compare's maxEqLeaves): a pair the access decision resolves is
  /// resolved in the tree, not left in one compare of the whole ites.
  Expr rowCompareEq(const Expr &i, const Expr &idx, unsigned &depth,
                    bool &reduced, unsigned &premises, bool shadow) {
    Expr cmp = mk<PTR_EQ>(i, idx);
    depth = 0;
    reduced = false;
    premises = PremNone;
    if (isOpX<ITE>(i) || isOpX<ITE>(idx)) {
      llvm::SmallVector<std::pair<Expr, Expr>, 8> pairs;
      if (eqPairs(i, idx, pairs, m_opts.maxDecideLeaves)) {
        unsigned resolved = 0;
        RewriteStats scratch;
        std::swap(scratch, m_stats);
        unsigned used = PremNone;
        Expr built = buildEq(i, idx, depth, resolved, /*diffBase=*/!shadow,
                             reduced, used, shadow, /*ptrLeaf=*/true);
        std::swap(scratch, m_stats);
        if (resolved) {
          m_stats.merge(scratch);
          cmp = built;
          premises = used;
          countPremises(used);
          // a leaf reduced to a compare of offsets (cmp.same-base,
          // cmp.cancel) has no pointer left to mark: an open alias question
          // in its reduced form, for the census (as overlap() records)
          if (reduced)
            noteReducedLeaves(built);
        } else {
          depth = 0;
          reduced = false;
        }
      }
    }
    return cmp;
  }

  /// Fig. 6.5, `select(store-map(a0, b, ocl), idx)` (idx normal):
  ///  - idx = b + o, o a numeral: R-HIT, the latest value written at o
  ///    (row.smap-hit, RW_DONE: the value is normal); R-MISS, none was
  ///    (row.smap-miss): select(a0, idx), RW_1 (the select at level 0,
  ///    a0 and idx normal). No assumption: other numeral offsets are other
  ///    addresses.
  ///  - idx a pointer into another registered object (and the objects are
  ///    distinct, nonzero): R-SKIP (row.smap-skip): select(a0, idx), RW_1.
  ///    uses: A-INB.
  ///  - otherwise R-ABORT (row.smap-abort): neither equality nor disequality
  ///    with the base is known (idx has no pointer view, or a symbolic offset
  ///    of b, or A-INB is off). The map is read as the store chain it stands
  ///    for, once: each entry (o, v), newest first, is a ROW step with the
  ///    compare of row() (decideEq: Yes ends the read with v, No skips the
  ///    entry, Unknown gives ite(b + o = idx, v, <rest>)), the bottom is
  ///    select(a0, idx). The thesis peels one entry per step
  ///    (read(write(store-map(a, b, m \ o1), ptr(b, o1), v1), i)): same terms,
  ///    quadratic in the list (a new list per step). Plain stores are never
  ///    built unless expandUnknownRow is off, and then they are final (RW_DONE;
  ///    a store chain visited by the rewriter would be merged into a store-map
  ///    again). The result is RW_FULL: the ite chain has one level per entry
  ///    and the select at its bottom starts its own chain (the shape that
  ///    needs chain-start caching: the same select(a0, idx) is reached from
  ///    every read of every version above it).
  RewriteResult rowStoreMap(const Expr &sm, const Expr &idx) {
    const Expr a0 = sm->arg(0), b = sm->arg(1), ocl = sm->arg(2);
    // a shadow memory: no R-SKIP (A-INB); R-ABORT decides the entries by
    // object identity
    const bool shadow = shadowMemory(sm);
    std::optional<PtrView> v = view(idx);
    if (v && v->base == b) {
      unsigned nw;
      if (op::bv::isBvNum(v->off, nw)) {
        Expr val = m_smaps.find(ocl, numVal(v->off));
        if (val) {
          m_stats.count("row.smap-hit");
          return {val, RWStatus::RW_DONE};
        }
        m_stats.count("row.smap-miss");
        return {op::array::select(a0, idx), RWStatus::RW_1};
      }
    } else if (v && !shadow &&
               distinctObjectsCounted(v->base, b)) { // uses: A-INB
      m_stats.count("row.smap-skip");
      countPremises(PremAInb);
      return {op::array::select(a0, idx), RWStatus::RW_1};
    }
    m_stats.count("row.smap-abort");
    struct Open {
      Expr ptr, value;
    };
    llvm::SmallVector<Open, 8> open; // newest first
    // The value under the open entries: select(a0, idx), or, when an older
    // entry is decided equal to idx, that entry's value (entries older than
    // it are shadowed). A Yes does not end the read while a newer entry is
    // still Unknown: those stay on top as ite levels.
    Expr bottom;
    std::vector<SMapEntry> ents = StoreMapTable::entries(ocl);
    for (auto it = ents.rbegin(); it != ents.rend(); ++it) {
      Expr p = ptrAt(b, it->offset);
      Decision d;
      Tri t = decideEq(p, idx, d, /*diffBase=*/!shadow,
                       shadow); // uses: A-INB, or LAYOUT in a shadow
      if (t == Tri::Yes) {
        m_stats.count("row.smap-abort-hit");
        countReasons(d);
        countPremises(d.premises);
        bottom = it->value;
        break;
      }
      if (t == Tri::No) {
        m_stats.count("row.smap-abort-skip");
        countReasons(d);
        countPremises(d.premises);
        continue;
      }
      unsigned sp = PremNone;
      if (m_opts.strideRule && !shadow &&
          strideApartEntry(b, numVal(it->offset), idx, sp)) {
        m_stats.count("row.smap-abort-stride");
        countReasons(Decision{{{"cmp.stride", sp}}, sp});
        countPremises(sp);
        continue;
      }
      open.push_back({p, it->value});
    }
    if (open.empty())
      return bottom ? RewriteResult{bottom, RWStatus::RW_DONE}
                    : RewriteResult{op::array::select(a0, idx), RWStatus::RW_1};
    if (!m_opts.expandUnknownRow) {
      // a store chain over a0 (after a Yes: over store(a0, idx, bottom),
      // which reads the same at idx)
      Expr chain = bottom ? Expr(op::array::store(a0, idx, bottom)) : a0;
      for (auto it = open.rbegin(); it != open.rend(); ++it)
        chain = op::array::store(chain, it->ptr, it->value);
      return {op::array::select(chain, idx), RWStatus::RW_DONE};
    }
    Expr res = bottom ? bottom : Expr(op::array::select(a0, idx));
    // oldest first; a run of consecutive entries with one value is one ite
    // over the disjunction of their compares (groupSameValueRow)
    ExprVector run;
    Expr runValue;
    auto flush = [&]() {
      if (run.empty())
        return;
      if (run.size() > 1)
        m_stats.rules["row.smap-group"] += run.size();
      // newest first, as the nested ites read
      std::reverse(run.begin(), run.end());
      Expr c =
          run.size() == 1 ? run.front() : mknary<OR>(run.begin(), run.end());
      res = mk<ITE>(c, runValue, res);
      run.clear();
    };
    for (auto it = open.rbegin(); it != open.rend(); ++it) {
      unsigned depth, premises;
      bool reduced;
      Expr cmp = rowCompare(it->ptr, idx, depth, reduced, premises, shadow);
      if (!m_opts.groupSameValueRow) {
        res = mk<ITE>(cmp, it->value, res);
        continue;
      }
      if (!run.empty() && it->value != runValue)
        flush();
      runValue = it->value;
      run.push_back(cmp);
    }
    flush();
    return {res, RWStatus::RW_FULL};
  }

  //===------------------------------------------------------------------===//
  // read over a memory-operation term (docs/design/SeaM-memops.md)
  //===------------------------------------------------------------------===//

  /// in(j, d, n) decided for a read: In (a hit), Out (a skip), Unknown.
  enum class Range { In, Out, Unknown };

  /// The unsigned values n can take, [lo, hi]: a numeral, or an absolute ARM
  /// interval with a nonnegative lower bound (ARM claims n = o mod 2^w for an
  /// integer o in it, so n is in it as an unsigned number). False: unknown.
  bool lenRange(const Expr &n, mpz_class &lo, mpz_class &hi) const {
    unsigned w;
    if (op::bv::isBvNum(n, w)) {
      lo = hi = numVal(n);
      return true;
    }
    if (!m_opts.arm)
      return false;
    const AddrRangeMap &m = m_arm.alpha(n);
    if (m.isTop() || m.size() != 1 ||
        m.entries()[0].first != AddrRangeMap::kAbs)
      return false;
    const AddrRange &r = m.entries()[0].second;
    if (!r.isRange() || r.lo().isNegative())
      return false;
    lo = expr::toMpz(r.lo());
    hi = expr::toMpz(r.hi());
    return true;
  }

  /// The difference x - y of the integer offsets of \p j and \p d (both not
  /// ites) as an integer interval [lo, hi], with k = (j - d) mod 2^w = (x - y)
  /// mod 2^w for the actual x, y. No assumption:
  ///   mem.in-exact  j and d differ by a numeral (the same term, numerals,
  ///                 or cancellation of common addends, e.g. one base)
  ///   arm.range     ARM: j and d each have one entry, of the same base (or
  ///                 both absolute), j = b + x, d = b + y with x, y integers in
  ///                 the intervals (AddrRangeMap.hh: no wrap in the claim)
  /// (a - b) as an integer when a and b are views over two different
  /// placed objects with numeral offsets: (n_a + o_a) - (n_b + o_b), n the
  /// placed numerals. Exact after the layout equations obj = n (a fact of
  /// the VC, not an assumption: uses LAYOUT). Bases with equal numerals (a
  /// hand-placed program) are not decided here.
  /// Only for the addresses of accesses (a ROW compare, a memop range) and
  /// only without A-INB: with A-INB, cmp.diff-base decides such pairs first;
  /// without it (--seam-inb=0, the vc.seam-inb-violated fallback) nothing
  /// else separates two objects, and every undecided memop row nests the
  /// next read (a read through n undecided mcopys is 2^n leaves). Program
  /// compares are never decided by the layout (SeaM-null.md, point 4).
  std::optional<mpz_class> layoutDiff(const Expr &a, const Expr &b) const {
    if (!m_opts.layoutDecide || m_opts.assumeInBounds)
      return std::nullopt;
    std::optional<PtrView> va = view(a), vb = view(b);
    if (!va || !vb || va->base == vb->base)
      return std::nullopt;
    unsigned w;
    if (!op::bv::isBvNum(va->off, w) || !op::bv::isBvNum(vb->off, w))
      return std::nullopt;
    const ObjectBase *oa = m_bases.lookup(va->base);
    const ObjectBase *ob = m_bases.lookup(vb->base);
    if (!oa || !ob || oa->base.getBitWidth() != ob->base.getBitWidth() ||
        oa->base == ob->base)
      return std::nullopt;
    // the layout numerals, unsigned (toMpz reads an APInt as signed)
    const unsigned n = oa->base.getBitWidth();
    mpz_class na = toMpz(oa->base.zext(n + 1)),
              nb = toMpz(ob->base.zext(n + 1));
    mpz_class oA = numVal(va->off), oB = numVal(vb->off);
    mpz_class r;
    mpz_add(r.get_mpz_t(), na.get_mpz_t(), oA.get_mpz_t());
    mpz_sub(r.get_mpz_t(), r.get_mpz_t(), nb.get_mpz_t());
    mpz_sub(r.get_mpz_t(), r.get_mpz_t(), oB.get_mpz_t());
    return r;
  }

  bool offsetDiff(const Expr &j0, const Expr &d0, mpz_class &lo, mpz_class &hi,
                  const char *&rule) const {
    const Expr j = op::ptr::stripObservation(j0),
               d = op::ptr::stripObservation(d0);
    unsigned w = widthOf(d);
    if (sameAddr(j, d)) {
      lo = hi = 0;
      rule = "mem.in-exact";
      return true;
    }
    mpz_class vj, vd;
    if (ptrNumerals(j, d, vj, vd)) {
      lo = hi = modPow2(vj - vd, w);
      rule = "mem.in-exact";
      return true;
    }
    mpz_class dv;
    if (cancelDiff(j, d, dv)) {
      lo = hi = modPow2(dv, w);
      rule = "mem.in-exact";
      return true;
    }
    if (std::optional<mpz_class> v = layoutDiff(j, d)) {
      lo = hi = modPow2(*v, w);
      rule = "mem.in-layout"; // uses: LAYOUT
      return true;
    }
    if (!m_opts.arm)
      return false;
    const AddrRangeMap &mj = m_arm.alpha(j);
    const AddrRangeMap &md = m_arm.alpha(d);
    if (mj.isTop() || md.isTop() || mj.size() != 1 || md.size() != 1)
      return false;
    const auto &ej = mj.entries()[0], &ed = md.entries()[0];
    if (ej.first != ed.first || !ej.second.isRange() || !ed.second.isRange())
      return false;
    lo = expr::toMpz(ej.second.lo()) - expr::toMpz(ed.second.hi());
    hi = expr::toMpz(ej.second.hi()) - expr::toMpz(ed.second.lo());
    rule = "arm.range";
    return true;
  }

  /// in(j, d, n) for k = (j - d) mod 2^w whose integer representative ranges
  /// over [lo, hi], n over [nlo, nhi] (unsigned): the circular test. The values
  /// of k are the residues of [lo, hi]: an interval of the circle, [l', l' +
  /// (hi - lo)] with l' = lo mod 2^w; if it does not pass 2^w - 1 (no wrap),
  /// In when its top is < nlo, Out when its bottom is >= nhi. A residue set
  /// that wraps holds both 2^w - 1 (never < n) and 0 (< n for n > 0):
  /// Unknown. Exact; linear intervals do not suffice.
  static Range circularIn(const mpz_class &lo, const mpz_class &hi,
                          const mpz_class &nlo, const mpz_class &nhi,
                          unsigned w) {
    mpz_class mod;
    mpz_ui_pow_ui(mod.get_mpz_t(), 2, w);
    mpz_class span = hi - lo;
    if (span < mpz_class(0UL) || span >= mod)
      return Range::Unknown;
    mpz_class l = modPow2(lo, w), h = l + span;
    if (h >= mod)
      return Range::Unknown;
    if (h < nlo)
      return Range::In;
    if (l >= nhi)
      return Range::Out;
    return Range::Unknown;
  }

  /// in(j, d, n) for one leaf pair (j, d not ites); the rule and its premises
  /// go to \p dec when decided. \p k gets the numeral k = j - d when exact.
  /// \p inb: A-INB may be used (false in a shadow memory). \p span: the
  /// read is of span bytes [j, j + span): In when all are in the range, Out
  /// when none is (the circular test of k .. k + span - 1).
  Range decideInPair(const Expr &j, const Expr &d, bool haveN,
                     const mpz_class &nlo, const mpz_class &nhi, Decision &dec,
                     mpz_class *k, bool inb = true, uint64_t span = 1) const {
    // different objects: no cell of [d, d + n) is j (uses: A-INB, for the
    // whole destination range and for j's access)
    std::optional<PtrView> vj = view(j), vd = view(d);
    // an object of size 0 (null): the read j (positive width) cannot be in
    // it, and a range [d, d + n) in it is empty (A-INB forces n = 0:
    // rangeInBounds): vacuous, no cell is read (uses: A-INB; SeaM-null.md)
    if (inb && m_opts.assumeInBounds &&
        ((vj && size0(vj->base)) || (vd && size0(vd->base)))) {
      dec.add("cmp.size0-vacuous", PremAInb); // uses: A-INB
      return Range::Out;
    }
    if (inb && vj && vd && vj->base != vd->base &&
        distinctObjects(vj->base, vd->base)) {
      dec.add("cmp.diff-base", PremAInb); // uses: A-INB
      return Range::Out;
    }
    // the same by ARM for a read address without a view: every provenance
    // of j (an ARM base entry; no absolute entry, which may be anywhere) is
    // another object than d's, all of nonzero size (uses: A-INB, the
    // per-provenance access condition of j, Rules::armInBounds, and d's
    // range, which has a view so rangeInBounds states it)
    if (inb && !vj && vd && m_opts.arm && m_opts.assumeInBounds) {
      const ObjectBase *od = m_bases.lookup(vd->base);
      const AddrRangeMap &mj = m_arm.alpha(j);
      if (od && !od->size.isZero() && !mj.isTop() && !mj.isBot() &&
          !mj.hasAbs()) {
        // a provenance in an object of size 0 is vacuous (j has positive
        // width): cmp.size0-vacuous
        bool apart = true, zero = false;
        for (const AddrRangeMap::Entry &e : mj.entries()) {
          if (e.first < m_bases.size() &&
              m_bases.objects()[e.first].size.isZero()) {
            zero = true;
            continue;
          }
          apart = apart && e.first != od->order && e.first < m_bases.size();
        }
        if (apart) {
          dec.add("arm.skip", PremAInb); // uses: A-INB
          if (zero)
            dec.add("cmp.size0-vacuous", PremAInb);
          return Range::Out;
        }
      }
    }
    if (!haveN)
      return Range::Unknown;
    mpz_class lo, hi;
    const char *rule = nullptr;
    if (!offsetDiff(j, d, lo, hi, rule))
      return Range::Unknown;
    Range r = circularIn(
        lo,
        span > 1 ? mpz_class(hi + mpz_class((unsigned long)(span - 1))) : hi,
        nlo, nhi, widthOf(d));
    if (r != Range::Unknown)
      dec.add(rule,
              std::strcmp(rule, "mem.in-layout") == 0 ? PremLayout : PremNone);
    if (k && lo == hi)
      *k = modPow2(lo, widthOf(d));
    return r;
  }

  /// in(idx, d, n) for a read at \p idx of a term with destination \p d and
  /// length \p n: over the leaf pairs of the ites of idx and d (as decideEq),
  /// In / Out when every pair says so, else Unknown. n = 0 (n's range is
  /// [0, 0]) is Out by mem.empty. \p k: the numeral k = idx - d when every
  /// pair has the same exact one (else null).
  Range decideIn(const Expr &idx, const Expr &d, const Expr &n, Decision &dec,
                 Expr &k, bool inb = true) const {
    mpz_class nlo, nhi;
    bool haveN = lenRange(n, nlo, nhi);
    if (haveN && nhi == 0) {
      dec.add("mem.empty");
      return Range::Out;
    }
    llvm::SmallVector<std::pair<Expr, Expr>, 8> pairs;
    if (!eqPairs(idx, d, pairs, m_opts.maxDecideLeaves)) {
      // too many leaf pairs: the whole terms (ARM looks through the ites)
      pairs.clear();
      pairs.emplace_back(idx, d);
    }
    bool allIn = true, allOut = true, sameK = true;
    std::optional<mpz_class> common;
    Decision all;
    for (auto &p : pairs) {
      Decision dp;
      mpz_class kv = -1;
      Range r = decideInPair(p.first, p.second, haveN, nlo, nhi, dp, &kv, inb);
      if (kv < mpz_class(0UL) || (common && *common != kv))
        sameK = false;
      else
        common = kv;
      allIn = allIn && r == Range::In;
      allOut = allOut && r == Range::Out;
      if (r != Range::Unknown)
        all.add(dp);
    }
    if (sameK && common)
      k = num(*common, widthOf(d));
    if (!allIn && !allOut)
      return Range::Unknown;
    dec.add(all);
    return allIn ? Range::In : Range::Out;
  }

  /// The in-test `(idx - d) <u n` of a read at \p idx of a memory-operation
  /// term with destination \p d and length \p n, over the leaf pairs of the
  /// ites of idx and d (as buildEq: ites with one condition paired branch by
  /// branch): a pair decideInPair resolves is true (In) / false (Out), with
  /// its reasons counted and its premises in \p premises; an open pair is
  /// its own `PTR_DIFF(j, d') <u n` (collected in \p open; the caller records
  /// them for the census, memOpOpen, when the tree is used).
  /// \p resolved counts the resolved pairs. On
  /// the path that selects a pair, idx and d are that pair, so the tree equals
  /// the in-test of the whole terms. uses: the premises of the resolved pairs
  /// (A-INB: cmp.diff-base, cmp.size0-vacuous, arm.skip; LAYOUT:
  /// mem.in-layout).
  Expr buildIn(const Expr &l, const Expr &r, const Expr &n, bool haveN,
               const mpz_class &nlo, const mpz_class &nhi, bool inb,
               unsigned &resolved, unsigned &premises,
               llvm::SmallVectorImpl<Expr> &open) {
    const Expr a = op::ptr::stripObservation(l),
               b = op::ptr::stripObservation(r);
    auto branches = [&](const Expr &c, const Expr &lt, const Expr &rt,
                        const Expr &le, const Expr &re) {
      Expr t =
          buildIn(lt, rt, n, haveN, nlo, nhi, inb, resolved, premises, open);
      Expr e =
          buildIn(le, re, n, haveN, nlo, nhi, inb, resolved, premises, open);
      return mk<ITE>(c, t, e);
    };
    if (isOpX<ITE>(a) && isOpX<ITE>(b) && a->arg(0) == b->arg(0))
      return branches(a->arg(0), a->arg(1), b->arg(1), a->arg(2), b->arg(2));
    if (isOpX<ITE>(a))
      return branches(a->arg(0), a->arg(1), b, a->arg(2), b);
    if (isOpX<ITE>(b))
      return branches(b->arg(0), a, b->arg(1), a, b->arg(2));
    Decision d;
    Range rg = decideInPair(a, b, haveN, nlo, nhi, d, nullptr, inb);
    if (rg != Range::Unknown) {
      ++resolved;
      countReasons(d);
      premises |= d.premises;
      return rg == Range::In ? mk<TRUE>(m_efac) : mk<FALSE>(m_efac);
    }
    Expr in = memOpIn(mk<PTR_DIFF>(a, b), n); // S1.5: a pointer difference
    open.push_back(in);
    return in;
  }

  /// select(T, idx) over a memory-operation term T (MemOps.hh), decided once
  /// per (term, read), not per cell:
  ///   skip (row.<op>-skip): in(idx, d, n) false: select(M, idx). RW_1 (the
  ///     select over normal kids, at level 0).
  ///     uses: A-INB when a leaf pair is in different objects (cmp.diff-base:
  ///     the whole destination range [d, d + n) is in d's object, idx in its
  ///     own; the premise is Rules::rangeInBounds and inBounds); none for
  ///     mem.empty (n = 0), mem.in-exact, arm.range (the circular test).
  ///   hit (row.<op>-hit): in(idx, d, n) true: mcopy -> select(Ms, s + k),
  ///     the read continues in the source (k = idx - d, a numeral when exact);
  ///     mset -> v (RW_DONE); mhavoc -> select(H, idx) (RW_1). No assumption
  ///     beyond the decision's.
  ///   undecided (row.<op>): ite(k <u n, <hit form>, select(M, idx)): one ite
  ///     with one compare (MemOps.hh memopRead); kept as it is when
  ///     expandUnknownRow is off (the boundary translates it). No assumption.
  ///     When idx or d is an ite, the compare is the tree of buildIn over
  ///     their leaf pairs (mem.in-push, at least one pair resolved): uses
  ///     the premises of the resolved pairs (A-INB across objects).
  ///   RW_FULL for the source read of mcopy and for the ite: the new selects
  ///   start their own chains below new sums (s + k, idx - d).
  RewriteResult rowMemOp(const Expr &exp, const Expr &t, const Expr &idx) {
    const char *name = isOpX<MEM_COPY>(t)  ? "mcopy"
                       : isOpX<MEM_SET>(t) ? "mset"
                                           : "mhavoc";
    auto counter = [&](const char *suffix) {
      m_stats.count(std::string("row.") + name + suffix);
    };
    Decision dec;
    Expr k;
    // no A-INB over a shadow memory (none is made by the VC generator: a
    // shadow has loads and stores only)
    Range r = decideIn(idx, memOpDst(t), memOpLen(t), dec, k,
                       /*inb=*/!shadowMemory(t));
    if (!k)
      k = mk<PTR_DIFF>(idx, memOpDst(t)); // S1.5: a pointer difference
    if (r == Range::Out) {
      counter("-skip");
      countReasons(dec);
      countPremises(dec.premises);
      return {op::array::select(memOpBase(t), idx), RWStatus::RW_1};
    }
    Expr hit = isOpX<MEM_COPY>(t)
                   ? op::array::select(t->arg(1), copySource(t->arg(3), k))
                   : memOpHit(t, idx, k);
    if (r == Range::In) {
      counter("-hit");
      countReasons(dec);
      countPremises(dec.premises);
      return {hit, isOpX<MEM_SET>(t)     ? RWStatus::RW_DONE
                   : isOpX<MEM_HAVOC>(t) ? RWStatus::RW_1
                                         : RWStatus::RW_FULL};
    }
    if (!m_opts.expandUnknownRow)
      return {exp, RWStatus::RW_DONE};
    counter("");
    // the in-test over the leaf pairs (as ROW's compare tree): the pairs
    // decided In / Out are true / false, the rest stay open
    Expr in = memOpIn(k, memOpLen(t));
    const Expr d = memOpDst(t);
    llvm::SmallVector<std::pair<Expr, Expr>, 8> pairs;
    if ((isOpX<ITE>(op::ptr::stripObservation(idx)) ||
         isOpX<ITE>(op::ptr::stripObservation(d))) &&
        eqPairs(idx, d, pairs, m_opts.maxDecideLeaves)) {
      mpz_class nlo, nhi;
      const bool haveN = lenRange(memOpLen(t), nlo, nhi);
      unsigned resolved = 0, premises = PremNone;
      llvm::SmallVector<Expr, 4> openIn;
      RewriteStats scratch;
      std::swap(scratch, m_stats);
      Expr built =
          buildIn(idx, d, memOpLen(t), haveN, nlo, nhi,
                  /*inb=*/!shadowMemory(t), resolved, premises, openIn);
      std::swap(scratch, m_stats);
      if (resolved) {
        m_stats.merge(scratch);
        m_stats.count("mem.in-push");
        countPremises(premises);
        in = built;
        for (const Expr &o : openIn)
          m_memOpOpen.insert(o);
      }
    }
    if (in == memOpIn(k, memOpLen(t)))
      m_memOpOpen.insert(in);
    return {mk<ITE>(in, hit, op::array::select(memOpBase(t), idx)),
            RWStatus::RW_FULL};
  }

  /// Fig. 4.6 / 2.6, with HIT and SKIP decided by the compare:
  ///   select(store(a, i, v), j) ~> v                  (decideEq: i = j)
  ///   select(store(a, i, v), j) ~> select(a, j)       (decideEq: i != j)
  ///   select(store(a, i, v), j) ~> ite(i = j, v, select(a, j))   otherwise
  /// uses: A-INB, through the compare (row.store-skip with cmp.diff-base).
  /// This is the only place that lets the compare use A-INB (diffBase): i
  /// and j are the addresses of two accesses. In the third form, when i or j
  /// is an ite, the compare is built over the leaves by buildEq (leaves
  /// that differ by A-INB are false there); otherwise it is the plain `=`
  /// (nothing more to decide), which the rewriter then visits as a program
  /// compare (no A-INB).
  /// RW_DONE for HIT (v is normal). RW_1 for SKIP: the new select is at level
  /// 0 and its kids are normal. RW_(d+1) for the ite (RW_1 with a plain
  /// compare): the ite at level 0, the select and the compare tree (depth d)
  /// from level 1, the compare leaves at level d + 1 (one more if a leaf
  /// reduced to new sums), their kids (i, j, a, v) are normal.
  ///
  /// Fig. 2.1 ITE-PUSH for reads:
  ///   select(ite(c, m1, m2), j) ~> ite(c, select(m1, j), select(m2, j))
  /// No assumption. RW_1: the ite at level 0, the selects at level 1, their
  /// kids normal; each select that is again over a store or an ite is that
  /// call's own chain.
  ///
  /// A store-map: rowStoreMap.
  /// addr.prune-size0 at a positive-width access (docs/design/SeaM-
  /// provenance.md):
  ///   select(M, a) ~> select(M, a'),  store(M, a, v) ~> store(M, a', v)
  /// with a' = pruneSize0(a), under the access's A-INB inBounds(a) (a size-0
  /// leaf is false there: a = a' on every execution it admits). Only the
  /// index argument changes. No rewrite when every leaf is size 0 (a definite
  /// violation: the static fallback) or the memory is a shadow memory
  /// (identity contract, no A-INB).
  std::optional<RewriteResult> pruneAccess(const Expr &exp) {
    if (!m_opts.assumeInBounds || !m_opts.pruneSize0 ||
        !isOpX<ITE>(exp->arg(1)))
      return std::nullopt;
    const Expr arr = exp->arg(0);
    Expr p = pruneSize0(exp->arg(1));
    if (!p || p == exp->arg(1) || shadowMemory(arr))
      return std::nullopt;
    m_stats.count("addr.prune-size0");
    countPremises(PremAInb);
    Expr res = isOpX<SELECT>(exp) ? op::array::select(arr, p)
                                  : op::array::store(arr, p, exp->arg(2));
    return RewriteResult{res, RWStatus::RW_1};
  }

  /// addr.prune-size0 at a memory-operation term (mset / mcopy / mhavoc,
  /// MemOps.hh; docs/design/SeaM-provenance.md, range pruning): an
  /// operation-level equivalence under the A-INB of its ranges (rangeInBounds
  /// of the destination, and of the source of an mcopy), not an address
  /// equality: a size-0 leaf there admits n = 0, where both operations are
  /// no-ops although the addresses differ.
  ///  - a range with a live leaf left: the range address pruned;
  ///  - a range whose every leaf is size 0: A-INB gives n = 0, the term is
  ///    its incoming memory (a no-op, not a violation);
  ///  - a range condition `false` (a numeral n > 0 that cannot fit: a
  ///    definite violation): no rewrite, the static fallback handles it.
  /// Never for a shadow memory.
  RewriteResult memOp(const Expr &exp) {
    if (!m_opts.assumeInBounds || !m_opts.pruneSize0)
      return {exp, RWStatus::RW_DONE};
    const bool copy = isOpX<MEM_COPY>(exp);
    const unsigned di = isOpX<MEM_SET>(exp) ? 1 : 2;
    const Expr n = memOpLen(exp);
    auto ranged = [&](unsigned i) -> bool { // an ite worth looking at
      return isOpX<ITE>(exp->arg(i));
    };
    if (!ranged(di) && !(copy && ranged(3)))
      return {exp, RWStatus::RW_DONE};
    if (shadowMemory(memOpBase(exp)) || (copy && shadowMemory(exp->arg(1))))
      return {exp, RWStatus::RW_DONE};
    llvm::SmallVector<unsigned, 2> pos{di};
    if (copy)
      pos.push_back(3);
    llvm::SmallVector<Expr, 6> kids(exp->args_begin(), exp->args_end());
    bool changed = false;
    for (unsigned i : pos) {
      const Expr a = exp->arg(i);
      Expr p = pruneSize0(a);
      if (p == a)
        continue;
      bool violated = false;
      (void)rangeInBounds(a, n, violated);
      if (violated)
        return {exp, RWStatus::RW_DONE};
      if (!p) { // every leaf size 0: A-INB gives n = 0
        m_stats.count("addr.prune-size0-noop");
        countPremises(PremAInb);
        return {memOpBase(exp), RWStatus::RW_DONE};
      }
      kids[i] = p;
      changed = true;
    }
    if (!changed)
      return {exp, RWStatus::RW_DONE};
    m_stats.count("addr.prune-size0-range");
    countPremises(PremAInb);
    return {exp->efac().mkNary(exp->op(), kids.begin(), kids.end()),
            RWStatus::RW_1};
  }

  RewriteResult row(const Expr &exp) {
    if (std::optional<RewriteResult> r = pruneAccess(exp))
      return *r;
    const Expr arr = exp->arg(0);
    const Expr idx = exp->arg(1);
    if (isOpX<W_STORE>(arr))
      return rowWide(exp, arr);
    if (isOpX<STORE>(arr)) {
      const Expr i = arr->arg(1);
      // a shadow memory (SeaM-null.md, "Shadow memories"): its accesses are
      // keyed by object identity, not program accesses: no A-INB rule
      // (cmp.diff-base, cmp.size0-vacuous, ARM); shadow.distinct-base
      const bool shadow = shadowMemory(arr);
      Decision d;
      Tri t = decideEq(i, idx, d, /*diffBase=*/!shadow, shadow);
      if (t == Tri::Yes) {
        m_stats.count("row.store-hit");
        countReasons(d);
        countPremises(d.premises);
        return {arr->arg(2), RWStatus::RW_DONE};
      }
      if (t == Tri::No &&
          m_opts.plantBug == SeaMOptions::PlantBug::RowStoreHit) {
        // test hook: a wrong rule (a hit on addresses decided different)
        m_stats.count("row.store-hit");
        countReasons(d);
        countPremises(d.premises);
        return {arr->arg(2), RWStatus::RW_DONE};
      }
      if (t == Tri::No) {
        m_stats.count("row.store-skip");
        countReasons(d);
        countPremises(d.premises);
        return {op::array::select(arr->arg(0), idx), RWStatus::RW_1};
      }
      if (std::optional<RewriteResult> res = leafSplit(exp))
        return *res;
      if (!m_opts.expandUnknownRow)
        return {exp, RWStatus::RW_DONE};
      m_stats.count("row.store");
      unsigned depth, premises;
      bool reduced;
      Expr cmp = rowCompare(i, idx, depth, reduced, premises, shadow);
      if (m_opts.groupSameValueRow) {
        // the writes below of the same value v whose compare is unknown
        // join this one (ite(c_1 | ... | c_k, v, below)); writes decided
        // apart are skipped; a write of v decided equal ends the read with
        // v. The walk stops at another value or another kind of memory.
        // A firing's premise covers the read and the top write only
        // (accessSpans): below the top, only premise-free decisions and
        // compares are used; one that needs a premise (A-INB of that
        // write, LAYOUT) stops the walk, and the one-step path takes it.
        const Expr v = arr->arg(2);
        ExprVector cmps{cmp};
        Expr cur = arr->arg(0);
        while (isOpX<STORE>(cur)) {
          Decision d2;
          Tri t2 = decideEq(cur->arg(1), idx, d2, /*diffBase=*/!shadow, shadow);
          if (d2.premises != PremNone)
            break;
          if (t2 == Tri::No) {
            m_stats.count("row.store-skip");
            countReasons(d2);
            countPremises(d2.premises);
            cur = cur->arg(0);
            continue;
          }
          if (cur->arg(2) != v)
            break;
          if (t2 == Tri::Yes) {
            // v on every path: the read is v
            m_stats.count("row.store-hit");
            countReasons(d2);
            countPremises(d2.premises);
            m_stats.rules["row.store-group"] += cmps.size();
            return {v, RWStatus::RW_DONE};
          }
          unsigned dp2, pr2;
          bool red2;
          RewriteStats scratch;
          std::swap(scratch, m_stats);
          Expr c2 = rowCompare(cur->arg(1), idx, dp2, red2, pr2, shadow);
          std::swap(scratch, m_stats);
          if (pr2 != PremNone)
            break; // its counts are dropped with it
          m_stats.merge(scratch);
          cmps.push_back(c2);
          cur = cur->arg(0);
        }
        if (cmps.size() > 1 || cur != arr->arg(0)) {
          if (cmps.size() > 1)
            m_stats.rules["row.store-group"] += cmps.size();
          Expr c = cmps.size() == 1 ? cmps.front()
                                    : mknary<OR>(cmps.begin(), cmps.end());
          return {mk<ITE>(c, v, op::array::select(cur, idx)),
                  RWStatus::RW_FULL};
        }
      }
      return {mk<ITE>(cmp, arr->arg(2), op::array::select(arr->arg(0), idx)),
              levels(depth + 1 + (reduced ? 1 : 0))};
    }
    if (isOpX<STORE_MAP>(arr) || isMemOpTerm(arr))
      if (std::optional<RewriteResult> res = leafSplit(exp))
        return *res;
    if (isOpX<STORE_MAP>(arr))
      return rowStoreMap(arr, idx);
    if (isMemOpTerm(arr))
      return rowMemOp(exp, arr, idx);
    if (isOpX<ITE>(arr)) {
      m_stats.count("row.ite");
      return {mk<ITE>(arr->arg(0), op::array::select(arr->arg(1), idx),
                      op::array::select(arr->arg(2), idx)),
              RWStatus::RW_1};
    }
    return {exp, RWStatus::RW_DONE};
  }

private:
  ExprFactory &m_efac;
  const BaseRegistry &m_bases;
  RewriteStats &m_stats;
  /// the ordered maps of the store-maps (a side table: Expr is immutable)
  StoreMapTable m_smaps;
  SeaMOptions m_opts;
  /// alpha per Expr node (the compares ask about the same terms again and
  /// again); a cache, so the rules stay logically const
  mutable AddrRangeAnalysis m_arm;
  /// the normalization rules of BvRules.hh
  bvnorm::Rules m_bv;
  /// offSum's normalizer: its counts go nowhere
  mutable RewriteStats m_scratchStats;
  mutable bvnorm::Rules m_scratchBv{m_efac, m_bases, m_scratchStats};
  /// widthOf's type checker (its typeOf cache is the memo)
  mutable std::unique_ptr<TypeChecker> m_tc;
  /// shadowMemory per memory term (a cache)
  mutable std::unordered_map<Expr, bool> m_shadowMemo;
  /// mentionsBase, per node
  mutable std::unordered_map<Expr, bool> m_mentionsMemo;
  /// flat / flatDeep per node, and the keys pinned
  mutable std::unordered_map<ENode *, Expr> m_flatMemo, m_flatDeepMemo;
  mutable std::vector<Expr> m_flatPin;
  /// erase(): the deep expansion, nodes into flat forms (memoized)
  mutable PtrIndexExpander m_old;
  /// sameAddr's eraser (same arithmetic as m_old, its own memo)
  mutable PtrIndexExpander m_cmpOld;
  /// canon (sameAddr), memoized per term; the pins keep the keys alive
  mutable std::unordered_map<ENode *, Expr> m_canonMemo;
  mutable ExprSet m_aliasReduced;
  /// the open in-tests of memory-operation reads (rowMemOp, buildIn), for
  /// the census (seam.alias.open-memop.<kind>)
  ExprSet m_memOpOpen;
  /// checkInbPremise: the recorded accesses (normal-form address, units),
  /// by node and by object base
  struct InbRec {
    Expr p;
    uint64_t units;
  };
  mutable std::unordered_map<ENode *, InbRec> m_inbRec;
  mutable std::unordered_map<ENode *, std::vector<ENode *>> m_inbByBase;
  mutable std::vector<std::string> m_inbUnrecorded;
  /// split loads loadResolveConcat looked at (not split again)
  std::unordered_map<ENode *, Expr> m_concatTried;
  /// leafWalk's memo: (memory, leaf address, read bytes) -> its answer; the
  /// keys pinned
  std::map<std::tuple<ENode *, ENode *, uint64_t>, LeafStep> m_leafMemo;
  std::vector<Expr> m_leafPin;
  /// the ite conditions of the addresses leafSplit split (the conditions
  /// loadResolveLeaves splits a load on)
  ExprSet m_leafConds;
  mutable std::vector<Expr> m_canonPin;
  /// decidePair's memo: (a, b, diffBase, shadow) -> answer and reasons;
  /// the keys pinned
  struct PairMemo {
    Tri t;
    Decision d;
  };
  mutable std::map<std::tuple<ENode *, ENode *, bool, bool>, PairMemo>
      m_pairMemo;
  mutable std::vector<Expr> m_pairPin;
};

} // end of namespace seam

/// \brief The SeaM rewrite config: every non-leaf term is visited (the
/// config must visit every operator on a path to a rewritable term, STORE
/// included, or the RW_n promises break), and the rules are dispatched by
/// operator.
class SeaMRewriteConfig : public ExprRewriterConfigBase {
  seam::Rules m_rules;

public:
  SeaMRewriteConfig(ExprFactory &efac, const BaseRegistry &bases,
                    RewriteStats &stats, SeaMOptions opts)
      : ExprRewriterConfigBase(efac, stats), m_rules(efac, bases, stats, opts) {
  }

  seam::Rules &rules() { return m_rules; }
  const seam::Rules &rules() const { return m_rules; }

  /// A store-map and its list are never rewritten: they are built from normal
  /// terms (rule comments of wow), and a rewrite of the node would only walk
  /// the list. A rule that builds a store-map over a term that is not normal
  /// (WOSMAP-COMMUTE) returns it as a follow-up: in the default (branch)
  /// mode its frame is processed whatever shouldRewrite says, with its kids
  /// visited. With RewriteOptions::followUpLookup the follow-up goes through
  /// visit(), which does not rewrite a term that shouldRewrite refuses: the
  /// store under the store-map is then left as it is (sound, not in normal
  /// form, cached as normal). sea-vc never sets followUpLookup.
  bool shouldRewrite(const Expr &exp) const {
    return !rules::isLeaf(exp) && !isOpX<STORE_MAP>(exp) &&
           !isOpX<SMAP_CONS>(exp);
  }

  /// With simplify, the rules of BvRules.hh: after the SeaM rule of the same
  /// operator when that found nothing (eq, ite: the SeaM rules decide pointer
  /// compares and resolve ite leaves, which must come first), instead of it
  /// for and / or (a superset of it), and alone for the operators SeaM has no
  /// rule for.
  RewriteResult doRewrite(const Expr &exp) {
    if (!m_rules.options().rules)
      return {exp, RWStatus::RW_DONE};
    const bool simp = m_rules.options().simplify;
    // the SeaM rule found nothing
    auto idle = [&](const RewriteResult &r) {
      return r.exp == exp && r.status == RWStatus::RW_DONE;
    };
    // PTR_INDEX (PtrIndex.md, step (a)): a node outside an address position
    // is its flat form; RW_1: the term at level 0 again, its kids (flat
    // forms of normal nodes, ites of them) at level 1
    if (op::ptr::isPtrIndex(exp))
      return m_rules.ptrIndex(exp);
    if (isOpX<PTR_FROM_BITS>(exp)) {
      RewriteResult r = m_rules.loadResolve(exp);
      if (r.exp != exp)
        return r;
    }
    // OFFSET (S1.4) lives inside a PtrView; one here leaked from a builder
    if (isOpX<OFFSET>(exp)) {
      m_rules.stats().count("offset.flat");
      return {m_rules.offSum(exp), RWStatus::RW_1};
    }
    if (isOpX<ITE>(exp)) {
      RewriteResult r = m_rules.ite(exp);
      return simp && idle(r) ? m_rules.bv().ite(exp) : r;
    }
    if (isOpX<EQ>(exp)) {
      RewriteResult r = m_rules.eq(exp);
      return simp && idle(r) ? m_rules.bv().eq(exp) : r;
    }
    if (isOpX<NEQ>(exp))
      return m_rules.neq(exp);
    if (isOpX<PTR_EQ>(exp))
      return m_rules.ptrEq(exp, simp);
    if (isOpX<IS_OVERLAP>(exp))
      return m_rules.overlap(exp, simp);
    if (isOpX<NEG>(exp))
      return m_rules.neg(exp);
    if (isOpX<AND>(exp))
      return simp ? m_rules.bv().andOr(exp, true) : m_rules.andOr(exp, true);
    if (isOpX<OR>(exp))
      return simp ? m_rules.bv().andOr(exp, false) : m_rules.andOr(exp, false);
    if (m_rules.options().memoryRules) {
      if (isOpX<SELECT>(exp))
        return m_rules.row(exp);
      if (isOpX<STORE>(exp))
        return m_rules.wow(exp);
      if (isMemOpTerm(exp))
        return m_rules.memOp(exp);
      if (isOpX<W_SELECT>(exp))
        return m_rules.rowW(exp);
    }
    if (isOpX<BADD>(exp))
      return m_rules.add(exp);
    if (isOpX<BSUB>(exp))
      return m_rules.sub(exp);
    if (isOpX<PTR_DIFF>(exp))
      return m_rules.diff(exp);
    if (simp) {
      if (isOpX<BEXTRACT>(exp))
        return m_rules.bv().extract(exp);
      if (isOpX<BCONCAT>(exp))
        return m_rules.bv().concat(exp);
      if (isOpX<BZEXT>(exp) || isOpX<BSEXT>(exp))
        return m_rules.bv().ext(exp);
      if (isOpX<BMUL>(exp) || isOpX<BAND>(exp) || isOpX<BOR>(exp) ||
          isOpX<BXOR>(exp))
        return m_rules.bv().ac(exp);
      if (isOpX<IMPL>(exp))
        return m_rules.bv().impl(exp);
    }
    // an operator without rules: kids are normal, the term is final
    return {exp, RWStatus::RW_DONE};
  }

  /// the branch's policy: select, store and ite always, others when shared;
  /// with simplify every term (the whole query goes through the rewriter,
  /// and a term used once is met again as the kid of the next one)
  bool shouldCache(const Expr &e) const {
    return m_rules.options().simplify || utils::shouldCache(e);
  }
};

/// \brief A SeaM rewriter with its own cache and statistics: one per VC
/// (docs/design/SeaM.md, "When it runs"). The cache keeps every term it has
/// seen alive; destroy it before the ExprFactory. \p bases must outlive it.
class SeaMRewriter {
  RewriteStats m_stats;
  RewriteCache m_cache;
  SeaMRewriteConfig m_config;
  ExprRewriter<SeaMRewriteConfig> m_rw;
  StoreMapExpander m_expander;
  MemOpTranslator m_memops;
  WideExpander m_wide;
  WideExpander m_wideKeep, m_wideRest; ///< solverTermWords
  bool m_memOpsMade = false;
  bool m_wideMade = false;
  AccessOrigins *m_record = nullptr;
  void recordReads(const Expr &root) {
    std::unordered_set<ENode *> seen;
    std::vector<ENode *> stack{root.get()};
    while (!stack.empty()) {
      ENode *n = stack.back();
      stack.pop_back();
      if (!seen.insert(n).second)
        continue;
      if (isOpX<SELECT>(n)) {
        Expr idx(n->arg(1));
        m_record->add(m_config.rules().erase(idx), idx);
      }
      for (size_t i = 0; i < n->arity(); ++i)
        stack.push_back(n->arg(i));
    }
  }
  /// the boundary's byte addresses with the offset pushed into ite leaves
  /// (op::ptr::byteAddress), totals over the three expanders
  void noteWideOffsets() {
    uint64_t pushed = 0, refused = 0;
    for (const WideExpander *w : {&m_wide, &m_wideKeep, &m_wideRest}) {
      pushed += w->offsetPushed();
      refused += w->offsetRefused();
    }
    if (pushed)
      m_stats.rules["wide.expanded-ite-offset"] = pushed;
    if (refused)
      m_stats.rules["wide.expanded-ite-offset-refused"] = refused;
  }
  /// time spent in rewrite(): the pre-processing of the thesis' Table 8.4
  double m_rewriteSeconds = 0;
  /// time spent in simplify(): the final pass over the VC
  double m_simplifySeconds = 0;

  Expr run(const Expr &e, double &seconds) {
    auto t0 = std::chrono::steady_clock::now();
    Expr res = m_rw.rewriteExpr(e);
    seconds +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    if (!m_rw.aborted() && m_config.shouldCache(res))
      m_cache.insert(res, res, kFullDepth);
    // ARM work, as totals (the analysis counts it, not the rules)
    if (m_config.rules().options().arm) {
      m_stats.rules["arm.nodes"] = m_config.rules().arm().nodes();
      m_stats.rules["arm.queries"] = m_config.rules().arm().queries();
    }
    return res;
  }

public:
  SeaMRewriter(ExprFactory &efac, const BaseRegistry &bases,
               SeaMOptions opts = SeaMOptions(),
               RewriteOptions ropts = RewriteOptions())
      : m_config(efac, bases, m_stats, opts),
        m_rw(m_config, m_cache, m_stats, ropts) {
    for (WideExpander *w : {&m_wide, &m_wideKeep, &m_wideRest})
      w->maxOffsetLeaves(opts.maxOffsetLeaves);
  }
  SeaMRewriter(const SeaMRewriter &) = delete;
  SeaMRewriter &operator=(const SeaMRewriter &) = delete;

  /// The normal form of \p e. The result is cached as its own normal form
  /// (a term the next call meets as a kid, such as the memory of the last
  /// store, is not walked again).
  Expr rewrite(const Expr &e) { return run(e, m_rewriteSeconds); }

  /// The normal form of a term of the VC that is not a memory access (a side
  /// condition, the query), with the same cache and rules as rewrite(): the
  /// final normalization pass. Time is counted apart (simplifySeconds()).
  Expr simplify(const Expr &e) { return run(e, m_simplifySeconds); }

  /// Seconds spent in rewrite() so far.
  double rewriteSeconds() const { return m_rewriteSeconds; }
  /// Seconds spent in simplify() so far.
  double simplifySeconds() const { return m_simplifySeconds; }

  /// True once a store-map was made (smap.new): terms that went through this
  /// rewriter may contain one.
  bool storeMapsMade() const { return m_stats.get("smap.new") > 0; }

  /// \p e with every store-map expanded into stores (StoreMap.hh): the form
  /// a solver, or a model evaluation, can take. \p e itself when no store-map
  /// was ever made. Counts smap.expanded (store-maps expanded; 0 unless one
  /// reached the boundary).
  Expr expandStoreMaps(const Expr &e) {
    Expr res = storeMapsMade() ? m_expander.expand(e) : e;
    m_stats.rules["smap.expanded"] = m_expander.expanded();
    return res;
  }

  /// A memory-operation term (MemOps.hh) was made: terms of the VC may hold
  /// one (the VC generator says so; the rules never make one).
  void noteMemOps() { m_memOpsMade = true; }
  bool memOpsMade() const { return m_memOpsMade; }
  /// A width term (wstore / wselect, MemOps.hh) was made.
  void noteWide() { m_wideMade = true; }
  bool wideMade() const { return m_wideMade; }

  /// \p e with every PTR_INDEX node replaced by its flat form (Rules::flat:
  /// the normalized lowered term; PtrIndex.md): the first step
  /// of every boundary. Counts ptrindex.expanded.
  Expr expandPtrIndex(const Expr &e) {
    PtrIndexExpander &x = m_config.rules().eraser();
    Expr res = x.expand(e);
    m_stats.rules["ptrindex.expanded"] = x.expanded();
    return res;
  }
  /// Does \p e hold a PTR_INDEX node (the backend guard)?
  bool hasPtrIndex(const Expr &e) { return m_config.rules().eraser().has(e); }

  /// The solver boundary (SeaOpSem::solverTerm): \p e with every store-map
  /// expanded into stores, then every memory-operation term eliminated by its
  /// read equations (MemOpTranslator: semantics only, no assumption). Null if
  /// a term is in a position that is not read (\p why says where). Counts
  /// smap.expanded and memop.translated (reads over a term translated: 0
  /// unless the rewriting left one).
  Expr solverTerm(const Expr &e, std::string &why) {
    Expr res = expandStoreMaps(e);
    // width terms first: their byte reads may be over memory-operation terms
    if (m_wideMade) {
      res = m_wide.expand(res);
      m_stats.rules["wide.expanded"] = m_wide.expanded();
      noteWideOffsets();
    }
    if (m_memOpsMade) {
      res = m_memops.translate(res, why);
      if (!res)
        return res;
      m_stats.rules["memop.translated"] = m_memops.translated();
    }
    // nodes last: every read of the query is there now, with the address it
    // has (the record of the stride rule); the translations above build
    // terms over a node that its expansion turns into the old ones
    if (m_record)
      recordReads(res);
    return expandPtrIndex(res);
  }

  /// The stride rule's record (PtrIndex.md section 5; Ackermann.hh
  /// AccessOrigins): while set, solverTerm adds, for each select of its
  /// result, the read's index (expanded) and the address it had (each leaf
  /// of an ite index is plain: the rule takes non-ite indices). Off (null)
  /// by default; sea-vc sets it for the query only.
  void recordAccesses(AccessOrigins *r) { m_record = r; }

  /// The solver boundary with initial words (docs/design/SeaM-initial-
  /// words.md rev 3, point 1), for the query only: solverTerm keeps the byte
  /// meaning for every other caller (--seam-check, replay, validation).
  /// Store-maps expanded; width terms byte-expanded except a wselect directly
  /// over an array constant; memory-operation terms translated; then \p iw
  /// analyzes the complete query (its map frozen) and translates the chosen
  /// arrays; the width terms left are byte-expanded. Null with \p why on a
  /// failure (a postcondition: a bug).
  Expr solverTermWords(const Expr &e, InitialWords &iw, std::string &why) {
    Expr res = expandStoreMaps(expandPtrIndex(e));
    if (m_wideMade) {
      m_wideKeep.keepOverArrayConstants(true);
      res = m_wideKeep.expand(res);
    }
    if (m_memOpsMade) {
      res = m_memops.translate(res, why);
      if (!res)
        return res;
      m_stats.rules["memop.translated"] = m_memops.translated();
    }
    // the builders above make nodes (PtrSort rev 5, S1.3); InitialWords
    // reads bit-vector addresses
    res = expandPtrIndex(res);
    iw.analyze(res);
    res = iw.translate(res, why);
    if (!res)
      return res;
    if (m_wideMade) {
      res = m_wideRest.expand(res);
      m_stats.rules["wide.expanded"] =
          m_wideKeep.expanded() + m_wideRest.expanded();
      noteWideOffsets();
      res = expandPtrIndex(res);
      if (m_wideRest.hasWide(res)) {
        why = "initial words: a width term is left in the query";
        return Expr();
      }
    }
    return res;
  }

  /// The rules (for the access check of the VC generator, which needs the
  /// pointer view of a normalized address).
  const seam::Rules &rules() const { return m_config.rules(); }

  const RewriteStats &stats() const { return m_stats; }
  RewriteStats &stats() { return m_stats; }
  RewriteCache &cache() { return m_cache; }
};

} // end of namespace expr
