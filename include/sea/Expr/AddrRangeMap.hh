// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Address Range Map (ARM), SeaM (docs/design/SeaM.md, "ARM"; thesis
// Ch. 5, Fig. 5.1-5.9, Theorems 1-4).
//
// ARM abstracts a pointer term by what it can be: for each object base, an
// interval of offsets. Two pointer terms whose ARMs share no pointer are
// different (Thm 4), whatever their ite conditions: the condition is ignored
// by the abstraction (the join of the branches). It decides the compares that
// the syntactic rules leave unknown (an ite with many leaves, a sum over a
// big ite) in time linear in the size of the terms, once per Expr node.
//
// This header has three layers:
//   AddrRange         an interval of offsets, with top and bottom (Fig. 5.2,
//                     5.3): join, meet, add, subset
//   AddrRangeMap      base -> AddrRange, with top and bottom (Fig. 5.8, 5.9):
//                     join, add, meet, subset, and the disjointness test
//   AddrRangeAnalysis alpha(term) for the terms of the VC, cached per Expr
//                     node (not per term: the thesis builds each ARM from
//                     scratch, O(n^2) per term)
//
// Soundness, and how this differs from the thesis.
//  - Offsets are signed w-bit integers. An interval [l, h] has l <= h in
//    [-2^(w-1), 2^(w-1) - 1]. The claim of an entry (b, [l, h]) for a term t
//    is: t = b + o (mod 2^w) for some integer o in [l, h] (the base b is the
//    value of its constant, obj.b). Top says nothing (every residue has a
//    representative in the signed range).
//  - A-NOWRAP (thesis Sec. 5.1: legal pointer arithmetic does not overflow)
//    is not assumed. Interval addition that leaves the signed range gives
//    top (checked with the overflow flags of APInt), and then the sum of the
//    two terms is covered by the claim above; with bounds inside the range,
//    the sum of the offsets as integers is the offset of the sum, no wrap. So
//    two intervals of one base that do not meet imply different values with
//    no assumption (|o1 - o2| < 2^w, so o1 != o2 mod 2^w).
//  - A-INB (SeaM.md "Soundness stance") is what separates two bases: a
//    pointer into b is not a pointer into b' (both objects of nonzero size).
//    In ARM form: the entry (b, I) of an accessed term also says the term is
//    in object b: it is the *provenance* of that leaf of the term (the base
//    of the bvadd under the ites that select it), checked per leaf by
//    Rules::inBounds like the view-based A-INB. Only the compare of a ROW
//    step (two accessed addresses) may use it: `disjoint(..., crossBase)`.
//  - A term the abstraction does not understand is "absolute": an entry under
//    the key kAbs (a number, like a numeral address, `ptr.from_int`, a loaded
//    value). An absolute entry may be anywhere: it never differs from an
//    entry of a base (no A-INB for it), only from another absolute interval
//    that does not meet. A term that mentions a registered base through an
//    operator other than bvadd / ite (bvsub of two bases, bvand, a ptr.to_int
//    round trip) is top. This is the mentionsBase rule: a pointer built
//    from integer casts is no pointer into a base.
//  - Differences from the figures: bottom is the identity of join and absorbs
//    add (Fig. 5.3 has [a, b] +^ bottom = [a, b], which is wrong for the sums
//    of sets, one of them empty); bottom is never produced by alpha, only by
//    meet; a map with only bottom entries is bottom (the thesis says so too).
//
// Test only: `arm_test::mutant` makes one lattice operation wrong
// (unittests/AddrRange), to check that the tests catch it. Zero in
// production.
#include "sea/Expr/BaseRegistry.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprAPInt.hh"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <unordered_map>
#include <utility>

namespace expr {

namespace arm_test {
/// 0: correct. 1..7: a wrong lattice operation (see AddrRange.hh uses).
///  1 join takes the larger lower bound     2 join takes the smaller upper
///  bound 3 add wraps instead of giving top       4 meet: touching intervals
///  are empty 5 map join drops the right map's bases  6 map join ignores common
///  bases 7 disjoint ignores the absolute entry (the wildcard)
inline int mutant = 0;
} // namespace arm_test

/// An interval of signed w-bit offsets: bottom (no offset), [lo, hi], or top
/// (any offset). Immutable value.
class AddrRange {
public:
  enum class Kind : unsigned char { Bot, Rng, Top };

  AddrRange() = default; // bottom
  static AddrRange bot() { return AddrRange(); }
  static AddrRange top() {
    AddrRange r;
    r.m_kind = Kind::Top;
    return r;
  }
  /// {v}, v a signed offset
  static AddrRange point(const llvm::APInt &v) { return range(v, v); }
  /// [lo, hi]; requires lo <=s hi. The whole range is top.
  static AddrRange range(const llvm::APInt &lo, const llvm::APInt &hi) {
    assert(lo.getBitWidth() == hi.getBitWidth() && lo.sle(hi));
    AddrRange r;
    if (lo.isMinSignedValue() && hi.isMaxSignedValue()) {
      r.m_kind = Kind::Top;
      return r;
    }
    r.m_kind = Kind::Rng;
    r.m_lo = lo;
    r.m_hi = hi;
    return r;
  }

  Kind kind() const { return m_kind; }
  bool isBot() const { return m_kind == Kind::Bot; }
  bool isTop() const { return m_kind == Kind::Top; }
  bool isRange() const { return m_kind == Kind::Rng; }
  const llvm::APInt &lo() const { return m_lo; }
  const llvm::APInt &hi() const { return m_hi; }

  bool operator==(const AddrRange &o) const {
    return m_kind == o.m_kind &&
           (m_kind != Kind::Rng || (m_lo == o.m_lo && m_hi == o.m_hi));
  }
  bool operator!=(const AddrRange &o) const { return !(*this == o); }

  /// o in the interval (o signed, same width)
  bool contains(const llvm::APInt &o) const {
    switch (m_kind) {
    case Kind::Bot:
      return false;
    case Kind::Top:
      return true;
    case Kind::Rng:
      return m_lo.sle(o) && o.sle(m_hi);
    }
    return false;
  }

  /// Fig. 5.3 ⊔: [a, b] ⊔ [a', b'] = [min(a, a'), max(b, b')]; ⊔ top = top;
  /// ⊔ bottom is the identity. Soundness of ite (thesis Thm 1, (5.1)).
  AddrRange join(const AddrRange &o) const {
    if (isBot())
      return o;
    if (o.isBot())
      return *this;
    if (isTop() || o.isTop())
      return top();
    if (m_lo.getBitWidth() != o.m_lo.getBitWidth())
      return top();
    llvm::APInt lo = m_lo.slt(o.m_lo) ? m_lo : o.m_lo;
    llvm::APInt hi = m_hi.sgt(o.m_hi) ? m_hi : o.m_hi;
    if (arm_test::mutant == 1)
      lo = m_lo.sgt(o.m_lo) ? m_lo : o.m_lo;
    if (arm_test::mutant == 2)
      hi = m_hi.slt(o.m_hi) ? m_hi : o.m_hi;
    if (lo.sgt(hi)) // only a mutant gets here
      return bot();
    return range(lo, hi);
  }

  /// Fig. 5.3 ⊓: [max(a, a'), min(b, b')] if they share a point, else
  /// bottom; ⊓ top is the identity, ⊓ bottom is bottom.
  AddrRange meet(const AddrRange &o) const {
    if (isBot() || o.isBot())
      return bot();
    if (isTop())
      return o;
    if (o.isTop())
      return *this;
    if (m_lo.getBitWidth() != o.m_lo.getBitWidth())
      return *this; // different widths: nothing known
    llvm::APInt lo = m_lo.sgt(o.m_lo) ? m_lo : o.m_lo;
    llvm::APInt hi = m_hi.slt(o.m_hi) ? m_hi : o.m_hi;
    if (lo.sgt(hi) || (arm_test::mutant == 4 && lo == hi))
      return bot();
    return range(lo, hi);
  }

  /// Fig. 5.3 +^: [a + a', b + b']. A bound outside the signed range (the
  /// sum overflows) gives top, not a wrapped interval: see the header.
  /// top + x = top; bottom + x = bottom.
  AddrRange add(const AddrRange &o) const {
    if (isBot() || o.isBot())
      return bot();
    if (isTop() || o.isTop())
      return top();
    if (m_lo.getBitWidth() != o.m_lo.getBitWidth())
      return top();
    bool ovLo = false, ovHi = false;
    llvm::APInt lo = m_lo.sadd_ov(o.m_lo, ovLo);
    llvm::APInt hi = m_hi.sadd_ov(o.m_hi, ovHi);
    if (arm_test::mutant == 3) {
      ovLo = ovHi = false;
      if (lo.sgt(hi))
        return top();
    }
    if (ovLo || ovHi)
      return top();
    return range(lo, hi);
  }

  /// Fig. 5.3 ⊑
  bool subsetOf(const AddrRange &o) const {
    if (isBot() || o.isTop())
      return true;
    if (isTop() || o.isBot())
      return false;
    return o.m_lo.sle(m_lo) && m_hi.sle(o.m_hi);
  }

  void print(llvm::raw_ostream &os) const {
    switch (m_kind) {
    case Kind::Bot:
      os << "_|_";
      return;
    case Kind::Top:
      os << "T";
      return;
    case Kind::Rng:
      os << "[" << m_lo.getSExtValue() << ", " << m_hi.getSExtValue() << "]";
      return;
    }
  }

private:
  Kind m_kind = Kind::Bot;
  llvm::APInt m_lo, m_hi;
};

/// An Address Range Map (Fig. 5.8, 5.9): a set of pointer terms as, per
/// object base (the key: ObjectBase::order), the interval of the offsets
/// the pointer may have into it. Keys are in increasing order; no entry is
/// bottom. Top: any pointer. Bottom: no pointer. The key kAbs is the
/// absolute part: values that are not pointers into a registered base (see
/// the header).
class AddrRangeMap {
public:
  static constexpr unsigned kAbs = ~0u;
  using Entry = std::pair<unsigned, AddrRange>;

  AddrRangeMap() = default; // bottom
  static AddrRangeMap bot() { return AddrRangeMap(); }
  static AddrRangeMap top() {
    AddrRangeMap m;
    m.m_top = true;
    return m;
  }
  /// {key -> r}
  static AddrRangeMap of(unsigned key, const AddrRange &r) {
    AddrRangeMap m;
    if (!r.isBot())
      m.m_ents.emplace_back(key, r);
    return m;
  }
  static AddrRangeMap absolute(const AddrRange &r) { return of(kAbs, r); }

  bool isTop() const { return m_top; }
  bool isBot() const { return !m_top && m_ents.empty(); }
  const llvm::SmallVectorImpl<Entry> &entries() const { return m_ents; }
  size_t size() const { return m_ents.size(); }

  /// The interval of \p key, or null.
  const AddrRange *find(unsigned key) const {
    for (const Entry &e : m_ents)
      if (e.first == key)
        return &e.second;
    return nullptr;
  }
  bool hasAbs() const { return !m_top && find(kAbs); }
  /// Some entry is a registered base.
  bool hasBases() const {
    for (const Entry &e : m_ents)
      if (e.first != kAbs)
        return !m_top;
    return false;
  }

  bool operator==(const AddrRangeMap &o) const {
    return m_top == o.m_top && m_ents.size() == o.m_ents.size() &&
           std::equal(m_ents.begin(), m_ents.end(), o.m_ents.begin());
  }
  bool operator!=(const AddrRangeMap &o) const { return !(*this == o); }

  /// Fig. 5.9 ⊔: per key, the union of the keys, the join of the intervals of
  /// a common key. The condition of the ite is not looked at. top ⊔ x = top.
  AddrRangeMap join(const AddrRangeMap &o) const {
    if (m_top || o.m_top)
      return top();
    AddrRangeMap r;
    size_t i = 0, j = 0;
    while (i < m_ents.size() || j < o.m_ents.size()) {
      if (j == o.m_ents.size() ||
          (i < m_ents.size() && m_ents[i].first < o.m_ents[j].first)) {
        r.m_ents.push_back(m_ents[i++]);
      } else if (i == m_ents.size() || o.m_ents[j].first < m_ents[i].first) {
        if (arm_test::mutant != 5)
          r.m_ents.push_back(o.m_ents[j]);
        ++j;
      } else {
        AddrRange a = m_ents[i].second.join(o.m_ents[j].second);
        if (arm_test::mutant == 6)
          a = m_ents[i].second;
        r.m_ents.emplace_back(m_ents[i].first, a);
        ++i;
        ++j;
      }
    }
    return r;
  }

  /// +# of Fig. 5.9 for two maps (the thesis adds an interval to a map; the
  /// offset here is a term with its own map: a numeral or an ite of them has
  /// only the absolute entry). Entry pairs: absolute + base = base,
  /// absolute + absolute = absolute, interval sum per pair, joined per key;
  /// base + base is not a pointer into one object: top. top + x = top,
  /// bottom + x = bottom.
  AddrRangeMap add(const AddrRangeMap &o) const {
    if (m_top || o.m_top)
      return top();
    AddrRangeMap r;
    for (const Entry &a : m_ents)
      for (const Entry &b : o.m_ents) {
        unsigned key;
        if (a.first == kAbs)
          key = b.first;
        else if (b.first == kAbs)
          key = a.first;
        else
          return top();
        r.insert(key, a.second.add(b.second));
      }
    return r;
  }

  /// Fig. 5.9 ⊓: the common keys, intervals met; entries that meet to
  /// bottom are dropped (bottom if none is left). top ⊓ x = x. Theorem 4 for
  /// maps with no absolute entry: bottom means different pointers (with
  /// A-INB if they have different keys). With absolute entries, use
  /// disjoint().
  AddrRangeMap meet(const AddrRangeMap &o) const {
    if (m_top)
      return o;
    if (o.m_top)
      return *this;
    AddrRangeMap r;
    size_t i = 0, j = 0;
    while (i < m_ents.size() && j < o.m_ents.size()) {
      if (m_ents[i].first < o.m_ents[j].first) {
        ++i;
      } else if (o.m_ents[j].first < m_ents[i].first) {
        ++j;
      } else {
        AddrRange a = m_ents[i].second.meet(o.m_ents[j].second);
        if (!a.isBot())
          r.m_ents.emplace_back(m_ents[i].first, a);
        ++i;
        ++j;
      }
    }
    return r;
  }

  /// Fig. 5.9 ⊑: every key here is a key there, with a subset interval.
  bool subsetOf(const AddrRangeMap &o) const {
    if (o.m_top)
      return true;
    if (m_top)
      return false;
    for (const Entry &e : m_ents) {
      const AddrRange *r = o.find(e.first);
      if (!r || !e.second.subsetOf(*r))
        return false;
    }
    return true;
  }

  /// Theorem 4, made precise for the VC: do the pointers of \p p and of \p q
  /// have no value in common? (true: the two terms are different.)
  ///  - top and bottom: no information / vacuous.
  ///  - an absolute entry (a number that may be anywhere) is not different
  ///    from an entry of any base, whatever its interval; two absolute
  ///    entries are different if their intervals do not meet.
  ///  - a common base: its intervals must not meet (no assumption).
  ///  - different bases (A-INB, \p crossBase): the terms are in different
  ///    objects; needs every base of both maps to name an object of nonzero
  ///    size (\p nonZero(key)): objects of size 0 share their base with a
  ///    neighbour.
  ///  - an entry of an object of size 0 (null, a zero-sized allocation), only
  ///    with \p crossBase (the two terms are addresses of accesses of
  ///    positive width, A-INB; docs/design/SeaM-null.md): no such access is
  ///    in that object, so the leaves of that provenance are vacuous and the
  ///    entry is dropped before the tests above (\p usedSize0 is set if one
  ///    was; a map left bottom is vacuous: true).
  /// Only one base on each side, the same: no cross pair to take on trust.
  /// \p usedCrossBase (optional) is set iff the answer is true *because of*
  /// \p crossBase (A-INB): it stays false for every answer that holds
  /// without the assumption (provenance: SeaMRules.hh, Decision).
  template <class NonZero>
  static bool disjoint(const AddrRangeMap &p, const AddrRangeMap &q,
                       bool crossBase, NonZero nonZero,
                       bool *usedCrossBase = nullptr,
                       bool *usedSize0 = nullptr) {
    if (p.m_top || q.m_top)
      return false;
    if (p.isBot() || q.isBot())
      return true;
    if (crossBase) {
      // drop the entries of size-0 objects (vacuous under A-INB)
      auto drop = [&](const AddrRangeMap &m, AddrRangeMap &out) {
        bool any = false;
        for (const Entry &e : m.m_ents) {
          if (e.first != kAbs && !nonZero(e.first))
            any = true;
          else
            out.m_ents.push_back(e);
        }
        return any;
      };
      AddrRangeMap p0, q0;
      bool dp = drop(p, p0), dq = drop(q, q0);
      if (dp || dq) {
        bool res = p0.isBot() || q0.isBot() ||
                   disjoint(p0, q0, crossBase, nonZero, usedCrossBase);
        if (res && usedSize0)
          *usedSize0 = true;
        return res;
      }
    }
    bool pb = p.hasBases(), qb = q.hasBases();
    if (arm_test::mutant != 7 && ((p.hasAbs() && qb) || (q.hasAbs() && pb)))
      return false;
    if (!p.meet(q).isBot())
      return false;
    if (!pb || !qb)
      return true; // absolute against absolute, met above
    if (p.size() == 1 && q.size() == 1 &&
        p.m_ents[0].first == q.m_ents[0].first)
      return true;
    if (!crossBase)
      return false;
    for (const Entry &e : p.m_ents)
      if (e.first != kAbs && !nonZero(e.first))
        return false;
    for (const Entry &e : q.m_ents)
      if (e.first != kAbs && !nonZero(e.first))
        return false;
    if (usedCrossBase)
      *usedCrossBase = true;
    return true;
  }

  /// `{#k: [l, h], abs: [l, h]}`, `T`, `_|_`
  void print(llvm::raw_ostream &os) const {
    if (m_top) {
      os << "T";
      return;
    }
    if (m_ents.empty()) {
      os << "_|_";
      return;
    }
    os << "{";
    bool first = true;
    for (const Entry &e : m_ents) {
      if (!first)
        os << ", ";
      first = false;
      if (e.first == kAbs)
        os << "abs";
      else
        os << "#" << e.first;
      os << ": ";
      e.second.print(os);
    }
    os << "}";
  }

private:
  /// join \p r into the entry of \p key (keeps the order)
  void insert(unsigned key, const AddrRange &r) {
    if (r.isBot())
      return;
    auto it = m_ents.begin();
    while (it != m_ents.end() && it->first < key)
      ++it;
    if (it != m_ents.end() && it->first == key)
      it->second = it->second.join(r);
    else
      m_ents.insert(it, Entry(key, r));
  }

  bool m_top = false;
  llvm::SmallVector<Entry, 2> m_ents;
};

/// alpha of the VC's address terms (Fig. 5.4, 5.7), one value per Expr node,
/// computed once and cached. The terms: a numeral n is absolute [n, n]; a
/// registered base b is {b: [0, 0]}; bvadd adds the maps of its arguments
/// (interval add for every entry: a base plus an offset); ite joins its
/// branches (the condition is ignored); a select or any other term that
/// mentions no registered base is an unknown absolute value (top interval);
/// a term that does mention one, other than through bvadd / ite, is top.
class AddrRangeAnalysis {
public:
  explicit AddrRangeAnalysis(const BaseRegistry &bases) : m_bases(bases) {}

  /// alpha(e) for a bit-vector term. Iterative (the ite chains of a VC are
  /// deep). The reference is valid until the analysis is destroyed.
  const AddrRangeMap &alpha(const Expr &root) {
    auto found = m_cache.find(root);
    if (found != m_cache.end())
      return found->second.map;
    struct Frame {
      Expr e;
      bool expanded;
    };
    llvm::SmallVector<Frame, 32> stack;
    stack.push_back({root, false});
    while (!stack.empty()) {
      Frame f = stack.back();
      stack.pop_back();
      if (m_cache.count(f.e))
        continue;
      if (!f.expanded) {
        if (isLeaf(f.e)) {
          m_cache.emplace(f.e, leaf(f.e));
          ++m_nodes;
          continue;
        }
        stack.push_back({f.e, true});
        // an ite's condition is not looked at
        size_t first = isOpX<ITE>(f.e) ? 1 : 0;
        for (size_t i = first; i < f.e->arity(); ++i)
          if (!m_cache.count(f.e->arg(i)))
            stack.push_back({f.e->arg(i), false});
        continue;
      }
      m_cache.emplace(f.e, node(f.e));
      ++m_nodes;
    }
    return m_cache.find(root)->second.map;
  }

  /// Does \p e mention a registered base other than as an object of the
  /// pointer arithmetic of its own (bvadd / ite) value, or hold one (the
  /// base itself)? Not looking into select / store, nor an ite's condition.
  bool mentionsBase(const Expr &e) {
    alpha(e);
    return m_cache.find(e)->second.mentionsBase;
  }

  /// Are \p a and \p b different pointers by their ARMs (Thm 4)? See
  /// AddrRangeMap::disjoint. \p crossBase: A-INB may separate two bases;
  /// \p usedCrossBase (optional): set iff a true answer needed that;
  /// \p usedSize0 (optional): set iff it dropped an entry of a size-0
  /// object (A-INB: no positive-width access is in it).
  bool disjoint(const Expr &a, const Expr &b, bool crossBase,
                bool *usedCrossBase = nullptr, bool *usedSize0 = nullptr) {
    ++m_queries;
    const AddrRangeMap &pa = alpha(a);
    const AddrRangeMap &pb = alpha(b);
    return AddrRangeMap::disjoint(
        pa, pb, crossBase,
        [&](unsigned key) {
          return key < m_bases.size() && !m_bases.objects()[key].size.isZero();
        },
        usedCrossBase, usedSize0);
  }

  uint64_t nodes() const { return m_nodes; }
  uint64_t queries() const { return m_queries; }

private:
  struct Info {
    AddrRangeMap map;
    bool mentionsBase = false;
  };

  static bool isLeaf(const Expr &e) {
    // numerals and declarations are BIND, constants a FAPP of a declaration
    // (as rules::isLeaf); a select / store is not looked into
    return e->arity() == 0 || isOpX<BIND>(e) ||
           (isOpX<FAPP>(e) && e->arity() == 1) || isOpX<SELECT>(e) ||
           isOpX<STORE>(e);
  }

  Info leaf(const Expr &e) const {
    unsigned w;
    if (op::bv::isBvNum(e, w)) {
      mpz_class v = op::bv::toMpz(e);
      mpz_class m, r;
      mpz_ui_pow_ui(m.get_mpz_t(), 2, w);
      mpz_fdiv_r(r.get_mpz_t(), v.get_mpz_t(), m.get_mpz_t());
      return {AddrRangeMap::absolute(AddrRange::point(toAPInt(w, r))), false};
    }
    if (const ObjectBase *o = m_bases.lookup(e))
      return {AddrRangeMap::of(o->order, AddrRange::point(llvm::APInt(
                                             o->base.getBitWidth(), 0))),
              true};
    // a select, a symbolic constant: some number, no base mentioned (what a
    // select holds is not a syntactic fact: A-INB covers an access address
    // that depends on it, as in the syntactic rules)
    return {AddrRangeMap::absolute(AddrRange::top()), false};
  }

  Info node(const Expr &e) const {
    if (isOpX<ITE>(e)) {
      const Info &t = m_cache.find(e->arg(1))->second;
      const Info &f = m_cache.find(e->arg(2))->second;
      return {t.map.join(f.map), t.mentionsBase || f.mentionsBase};
    }
    bool mb = false;
    for (size_t i = 0; i < e->arity(); ++i)
      mb = mb || m_cache.find(e->arg(i))->second.mentionsBase;
    if (isOpX<BADD>(e)) {
      AddrRangeMap acc = m_cache.find(e->arg(0))->second.map;
      for (size_t i = 1; i < e->arity() && !acc.isTop(); ++i)
        acc = acc.add(m_cache.find(e->arg(i))->second.map);
      return {std::move(acc), mb};
    }
    // an observation marker is the address of its argument; a constructor
    // (from_int, from_bits) has no provenance (D17): some number
    if (op::ptr::isObservation(e))
      return m_cache.find(e->arg(0))->second;
    if (isOpX<PTR_FROM_INT>(e) || isOpX<PTR_FROM_BITS>(e)) {
      // of a numeral: that absolute address (no base, no provenance claim)
      unsigned nw;
      if (op::bv::isBvNum(e->arg(0), nw))
        return {m_cache.find(e->arg(0))->second.map, false};
      return {AddrRangeMap::absolute(AddrRange::top()), false};
    }
    // a node (PtrSort S2a): the map of its value base + sum x_k * s_k + c +
    // K, as the sum it erases to: the base's map, per index the index's map
    // for stride 1, nothing for stride 0, else (a product) some number,
    // top if the index mentions a base; then the point c + K (mod 2^n)
    if (op::ptr::isPtrIndex(e)) {
      const Info &b = m_cache.find(op::ptr::base(e))->second;
      AddrRangeMap acc = b.map;
      if (acc.isTop())
        return {std::move(acc), mb};
      unsigned n = 0;
      for (const AddrRangeMap::Entry &en : acc.entries())
        if (en.second.isRange()) {
          n = en.second.lo().getBitWidth();
          break;
        }
      for (size_t k = 0; k < op::ptr::numIndices(e) && !acc.isTop(); ++k) {
        const Info &x = m_cache.find(op::ptr::index(e, k))->second;
        mpz_class s = op::ptr::stride(e, k);
        if (n) {
          mpz_class m, r;
          mpz_ui_pow_ui(m.get_mpz_t(), 2, n);
          mpz_fdiv_r(r.get_mpz_t(), s.get_mpz_t(), m.get_mpz_t());
          s = r;
        }
        if (s == 0)
          continue;
        if (s == 1)
          acc = acc.add(x.map);
        else
          acc = acc.add(x.mentionsBase
                            ? AddrRangeMap::top()
                            : AddrRangeMap::absolute(AddrRange::top()));
      }
      // without a width (no interval in the base's map: an unbounded
      // entry), the point does not change the map
      if (acc.isTop() || !n)
        return {std::move(acc), mb};
      mpz_class c = op::ptr::constant(e) + op::ptr::byteOffset(e), m, r;
      mpz_ui_pow_ui(m.get_mpz_t(), 2, n);
      mpz_fdiv_r(r.get_mpz_t(), c.get_mpz_t(), m.get_mpz_t());
      if (r != 0)
        acc = acc.add(AddrRangeMap::absolute(AddrRange::point(toAPInt(n, r))));
      return {std::move(acc), mb};
    }
    return {mb ? AddrRangeMap::top() : AddrRangeMap::absolute(AddrRange::top()),
            mb};
  }

  const BaseRegistry &m_bases;
  std::unordered_map<Expr, Info> m_cache;
  uint64_t m_nodes = 0, m_queries = 0;
};

} // namespace expr
