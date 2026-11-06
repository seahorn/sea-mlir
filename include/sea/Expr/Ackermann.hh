// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Ackermann lowering of read-only arrays (docs/design/Ackermann.md).
//
// An encoding step of the solver-facing query, after the solver boundary
// (SeaOpSem::solverTerm): not a program change, not a SeaM rewrite. With SeaM's
// memory-operation terms the query often has no store at all, only reads of
// array constants (initial memories nd.<site>, memhavoc arrays). Such an array
// A is only an uninterpreted function: every distinct read select(A, i_k)
// becomes a fresh constant r_{A,k} of the cell sort, with the functional-
// consistency constraints
//
//     i_k = i_l  ->  r_{A,k} = r_{A,l}        for k < l
//
// (Ackermann's reduction). The lowered formula is equisatisfiable with the
// original one; no constraint is dropped except where it is valid. Each index
// is put in a linear form i = S + c over w-bit bit-vectors (LinearForms:
// bvadd / bvsub / bvneg / bvmul by numerals, ite(b, S1 + c, S2 + c) =
// ite(b, S1, S2) + c; S a map atom -> coefficient, empty for a numeral; c a
// numeral modulo 2^w; exact identities, no assumption):
//  - two reads with the same S and c have equal indices: they share one
//    constant (one read); in particular the same index term (hash-consed: the
//    same node) and two numerals of the same value modulo 2^w;
//  - two reads with the same S and different c: S + c1 = S + c2 iff c1 = c2
//    (mod 2^w), so i_k = i_l is false and the constraint is true (counted:
//    numeral pairs when S is empty, offset pairs otherwise).
// The other drops are exact. One more (sea-vc default; off:
// --ack-stride=false): the stride rule (--ack-stride, docs/design/PtrIndex.md
// section 5): a pair of reads whose every address was a promised GEP node
// (inbounds / nusw / nuw) over one base, whose integer offsets can never be
// equal (the gcd of the stride differences does not divide the constant
// difference), or one at a node over an object of size 0. Dropping any
// constraint only weakens the lowered query: unsat stays sound with no premise.
// The promises (and, for a byte of a wide access or a size-0 base, A-INB) are
// what make the dropped constraints vacuous, so a sat model needs them: one
// that puts two such reads at one index breaks one, is detected
// (AckermannModel::strideBroken) before the result, and sea-vc reruns without
// the rule. Otherwise no pair is dropped (pointers into different objects are
// not taken to be different here). An array is lowered only if every use of it
// is the array operand of a select: no store over it, no equality, no ite, no
// other operator (then it is kept as it is, `kept` counts it). Indices of reads
// may contain reads (of the same or other arrays): they are lowered first
// (bottom-up).
//
// Model reconstruction (AckermannModel): the model of the lowered formula has
// values for i_k and r_k; each lowered array A becomes the finite function
// {value(i_k) -> value(r_k)} (consistent: by the constraints) with default 0
// elsewhere. AckermannModel is a solver::Model whose eval reads every
// select(A, j) through that function (j evaluated first) and replaces any
// other occurrence of A by the store chain over a constant 0 array, so every
// consumer of the model (replay, validation) sees the same array.
#include "sea/Expr/Expr.hh"
#include "sea/Expr/Smt/Model.hh"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace expr {

namespace ackermann_detail {
/// A bit-vector numeral's value reduced modulo 2^w (bvnum does not reduce;
/// a backend may return a signed numeral).
inline mpz_class reducedValue(const Expr &num) {
  unsigned w = op::bv::widthBvNum(num);
  mpz_class mod;
  mpz_ui_pow_ui(mod.get_mpz_t(), 2, w);
  mpz_class r;
  mpz_fdiv_r(r.get_mpz_t(), op::bv::toMpz(num).get_mpz_t(), mod.get_mpz_t());
  return r;
}

/// A constant (0-ary application) of an array sort.
inline bool isArrayConstant(ENode *n) {
  return op::bind::isArrayConst(Expr(n));
}

/// A node with no kids worth visiting: terminals, numerals and other binds,
/// constants.
inline bool isLeaf(ENode *n) {
  return n->arity() == 0 || isOpX<BIND>(n) || isOpX<FDECL>(n) ||
         (isOpX<FAPP>(n) && n->arity() == 1);
}

/// Bottom-up rewrite of the DAG of \p root, iterative (VC terms are deep):
/// \p fn(node, kids) gives the image of a non-leaf node from the images of its
/// kids; a leaf's image is \p leaf(node). Memoized in \p memo (node -> image;
/// \p keep holds the keys alive).
template <typename LeafFn, typename NodeFn>
Expr rewriteDag(const Expr &root, std::unordered_map<ENode *, Expr> &memo,
                std::vector<Expr> &keep, LeafFn leaf, NodeFn fn) {
  std::vector<std::pair<ENode *, bool>> stack{{root.get(), false}};
  while (!stack.empty()) {
    auto [n, expanded] = stack.back();
    if (memo.count(n)) {
      stack.pop_back();
      continue;
    }
    if (isLeaf(n)) {
      stack.pop_back();
      memo.emplace(n, leaf(n));
      keep.push_back(Expr(n));
      continue;
    }
    if (!expanded) {
      stack.back().second = true;
      for (size_t i = n->arity(); i-- > 0;)
        if (!memo.count(n->arg(i)))
          stack.push_back({n->arg(i), false});
      continue;
    }
    stack.pop_back();
    llvm::SmallVector<Expr, 4> kids;
    for (size_t i = 0; i < n->arity(); ++i)
      kids.push_back(memo.find(n->arg(i))->second);
    memo.emplace(n, fn(n, kids));
    keep.push_back(Expr(n));
  }
  return memo.find(root.get())->second;
}

/// \p n with kids \p kids (the node itself if they are its own).
inline Expr withKids(ENode *n, llvm::SmallVectorImpl<Expr> &kids) {
  bool changed = false;
  for (size_t i = 0; i < kids.size(); ++i)
    changed = changed || kids[i].get() != n->arg(i);
  return changed ? n->getFactory().mkNary(n->op(), kids.begin(), kids.end())
                 : Expr(n);
}
/// A linear form of a w-bit term: sum of coef(a) * a over atoms a, plus c,
/// modulo 2^w (coefficients nonzero, reduced).
struct Linear {
  std::map<ENode *, mpz_class> coef;
  mpz_class c;
};

/// The linear forms of index terms of one width w: bvadd, bvsub, bvneg, a
/// bvmul with one non-numeral factor, numerals; an ite whose two branches
/// have the same constant c is ite(cond, S1, S2) + c (an atom built from the
/// two symbolic parts, hash-consed, so equal structures meet); every other
/// term is an atom. Exact identities of w-bit arithmetic: the form is equal
/// to the term.
class LinearForms {
public:
  using Key = std::vector<std::pair<ENode *, mpz_class>>;

  explicit LinearForms(unsigned w) : m_w(w) {
    mpz_ui_pow_ui(m_mod.get_mpz_t(), 2, w);
  }
  /// Constants with a known value (the layout: obj.k = <numeral>, a fact of
  /// every model of the query): their form is that numeral. Set before the
  /// first of(); null: none (the default, used for read sharing).
  void setValues(const std::unordered_map<ENode *, mpz_class> *v) {
    m_values = v;
  }
  /// acc += sign * l (sign 1 or -1)
  void add(Linear &acc, const Linear &l, bool negate) const {
    mpz_class k(1UL);
    if (negate)
      mpz_sub_ui(k.get_mpz_t(), m_mod.get_mpz_t(), 1);
    addScaled(acc, l, k);
  }
  unsigned width() const { return m_w; }

  const Linear &of(ENode *n) {
    auto it = m_memo.find(n);
    if (it != m_memo.end())
      return it->second;
    Linear l = compute(n);
    m_keep.push_back(Expr(n));
    return m_memo.emplace(n, std::move(l)).first->second;
  }

  /// The symbolic part as a key (atoms ordered, with coefficients).
  Key key(const Linear &l) const { return Key(l.coef.begin(), l.coef.end()); }

private:
  mpz_class norm(const mpz_class &x) const {
    mpz_class r;
    mpz_fdiv_r(r.get_mpz_t(), x.get_mpz_t(), m_mod.get_mpz_t());
    return r;
  }
  mpz_class mul(const mpz_class &x, const mpz_class &y) const {
    mpz_class r;
    mpz_mul(r.get_mpz_t(), x.get_mpz_t(), y.get_mpz_t());
    return norm(r);
  }
  /// acc += k * l
  void addScaled(Linear &acc, const Linear &l, const mpz_class &k) const {
    mpz_class t;
    mpz_addmul(t.get_mpz_t(), l.c.get_mpz_t(), k.get_mpz_t());
    mpz_add(t.get_mpz_t(), t.get_mpz_t(), acc.c.get_mpz_t());
    acc.c = norm(t);
    for (const auto &[a, v] : l.coef) {
      mpz_class s;
      mpz_mul(s.get_mpz_t(), v.get_mpz_t(), k.get_mpz_t());
      auto [p, fresh] = acc.coef.emplace(a, mpz_class(0UL));
      mpz_add(s.get_mpz_t(), s.get_mpz_t(), p->second.get_mpz_t());
      p->second = norm(s);
      if (p->second == mpz_class(0UL))
        acc.coef.erase(p);
    }
  }
  Linear atom(ENode *n) const {
    Linear l;
    l.coef.emplace(n, mpz_class(1UL));
    return l;
  }
  /// The symbolic part as a term (0 if empty).
  Expr rebuild(const Linear &l, ExprFactory &ef) {
    std::vector<Expr> terms;
    for (const auto &[a, v] : l.coef)
      terms.push_back(v == mpz_class(1UL)
                          ? Expr(a)
                          : mk<BMUL>(op::bv::bvnum(v, m_w, ef), Expr(a)));
    Expr r = terms.empty()       ? op::bv::bvnum(mpz_class(0UL), m_w, ef)
             : terms.size() == 1 ? terms[0]
                                 : mknary<BADD>(terms.begin(), terms.end());
    m_keep.push_back(r);
    return r;
  }
  Linear compute(ENode *n) {
    Expr e(n);
    if (m_values) {
      auto it = m_values->find(n);
      if (it != m_values->end()) {
        Linear l;
        l.c = norm(it->second);
        return l;
      }
    }
    if (op::bv::is_bvnum(e)) {
      Linear l;
      l.c = norm(op::bv::toMpz(e));
      return l;
    }
    const mpz_class one(1UL);
    mpz_class minusOne;
    mpz_sub_ui(minusOne.get_mpz_t(), m_mod.get_mpz_t(), 1);
    if (isOpX<BADD>(n) || isOpX<BSUB>(n)) {
      Linear l;
      for (size_t i = 0; i < n->arity(); ++i) {
        const Linear &k = of(n->arg(i));
        addScaled(l, k, (isOpX<BSUB>(n) && i > 0) ? minusOne : one);
      }
      return l;
    }
    if (isOpX<BNEG>(n) && n->arity() == 1) {
      Linear l;
      addScaled(l, of(n->arg(0)), minusOne);
      return l;
    }
    if (isOpX<BMUL>(n)) {
      mpz_class k(1UL);
      ENode *other = nullptr;
      size_t others = 0;
      for (size_t i = 0; i < n->arity(); ++i) {
        Expr kid(n->arg(i));
        if (op::bv::is_bvnum(kid))
          k = mul(k, op::bv::toMpz(kid));
        else {
          other = n->arg(i);
          ++others;
        }
      }
      Linear l;
      if (others == 0)
        l.c = k;
      else if (others == 1)
        addScaled(l, of(other), k);
      else
        return atom(n);
      return l;
    }
    if (isOpX<ITE>(n) && n->arity() == 3) {
      Linear t = of(n->arg(1)), f = of(n->arg(2));
      if (t.c != f.c)
        return atom(n);
      if (t.coef == f.coef)
        return t; // ite(c, x, x) = x
      Expr ite = mk<ITE>(Expr(n->arg(0)), rebuild(t, n->efac()),
                         rebuild(f, n->efac()));
      m_keep.push_back(ite);
      Linear l = atom(ite.get());
      l.c = t.c;
      return l;
    }
    return atom(n);
  }

  unsigned m_w;
  mpz_class m_mod;
  const std::unordered_map<ENode *, mpz_class> *m_values = nullptr;
  std::unordered_map<ENode *, Linear> m_memo;
  std::vector<Expr> m_keep;
};
} // namespace ackermann_detail

/// Leaf-wise "never equal" for two index terms (the Ackermann pair test
/// beyond equal symbolic parts): each term as the set of linear forms of its
/// leaves (through ite, and bvadd / bvsub over ites: the forms of every
/// combination; at most kMaxLeaves, else the term is one leaf), with the
/// known values of constants substituted (the layout). The terms are never
/// equal if no leaf of one can equal a leaf of the other: equal symbolic
/// parts with different numerals, or a residue difference (every
/// coefficient of the difference a multiple of 2^t, the numeral difference
/// not). Exact over w-bit arithmetic given the values (true in every model
/// of the query when they are its unconditional facts).
class LeafDecider {
  using Linear = ackermann_detail::Linear;
  using LinearForms = ackermann_detail::LinearForms;

public:
  static constexpr size_t kMaxLeaves = 16;
  LeafDecider(unsigned w, const std::unordered_map<ENode *, mpz_class> *values)
      : m_lf(w) {
    m_lf.setValues(values);
  }

  bool neverEqual(ENode *a, ENode *b) {
    const std::vector<Linear> &la = leaves(a), &lb = leaves(b);
    for (const Linear &x : la)
      for (const Linear &y : lb)
        if (!apart(x, y))
          return false;
    return true;
  }

  /// Trailing zeros of x modulo 2^w (w for 0).
  static unsigned tz(const mpz_class &x, unsigned w) {
    mpz_class m(1UL);
    mpz_mul_2exp(m.get_mpz_t(), m.get_mpz_t(), w);
    mpz_class r;
    mpz_fdiv_r(r.get_mpz_t(), x.get_mpz_t(), m.get_mpz_t());
    if (r == 0)
      return w;
    return unsigned(mpz_scan1(r.get_mpz_t(), 0));
  }

private:
  /// Two linear forms that are never equal.
  bool apart(const Linear &x, const Linear &y) const {
    const unsigned w = m_lf.width();
    unsigned t = w;
    auto i = x.coef.begin(), j = y.coef.begin();
    while ((i != x.coef.end() || j != y.coef.end()) && t > 0) {
      if (j == y.coef.end() || (i != x.coef.end() && i->first < j->first)) {
        t = std::min(t, tz(i->second, w));
        ++i;
      } else if (i == x.coef.end() || j->first < i->first) {
        t = std::min(t, tz(j->second, w));
        ++j;
      } else {
        t = std::min(t, tz(mpz_class(i->second - j->second), w));
        ++i;
        ++j;
      }
    }
    // the difference of the symbolic parts is a multiple of 2^t (t = w: they
    // are equal); the numerals must differ below 2^t
    return tz(mpz_class(y.c - x.c), w) < t;
  }

  const std::vector<Linear> &leaves(ENode *n) {
    auto it = m_memo.find(n);
    if (it != m_memo.end())
      return it->second;
    std::vector<Linear> res;
    auto combine = [&](const std::vector<Linear> &xs,
                       const std::vector<Linear> &ys, bool negate) {
      std::vector<Linear> out;
      if (xs.size() * ys.size() > kMaxLeaves)
        return out; // empty: too many
      for (const Linear &x : xs)
        for (const Linear &y : ys) {
          Linear z = x;
          m_lf.add(z, y, negate);
          out.push_back(std::move(z));
        }
      return out;
    };
    if (isOpX<ITE>(n) && n->arity() == 3) {
      const std::vector<Linear> &t = leaves(n->arg(1));
      res = t;
      const std::vector<Linear> &f = leaves(n->arg(2));
      res.insert(res.end(), f.begin(), f.end());
      if (res.size() > kMaxLeaves)
        res.clear();
    } else if ((isOpX<BADD>(n) || isOpX<BSUB>(n)) && n->arity() >= 1) {
      res = leaves(n->arg(0));
      for (size_t i = 1; i < n->arity() && !res.empty(); ++i)
        res = combine(res, leaves(n->arg(i)), isOpX<BSUB>(n));
    }
    if (res.empty())
      res.push_back(m_lf.of(n));
    return m_memo.emplace(n, std::move(res)).first->second;
  }

  LinearForms m_lf;
  std::unordered_map<ENode *, std::vector<Linear>> m_memo;
};

/// The stride rule's input (docs/design/PtrIndex.md section 5):
/// for each read index of the query (the expanded term the read has), the
/// access addresses it was before the boundary expanded the PTR_INDEX nodes
/// (SeaMRewriter::recordAccesses): flagged nodes, or `plain` when one of
/// them was anything else (a bvadd, an unflagged node, an ite, a term a
/// boundary translation built). An index without an entry is plain.
struct AccessOrigins {
  struct Entry {
    Expr key;
    std::vector<Expr> nodes; ///< distinct flagged nodes
    bool plain = false;
  };
  std::unordered_map<ENode *, Entry> byIndex;
  /// \p key (expanded) was the read address \p origin
  void add(const Expr &key, const Expr &origin) {
    Entry &e = byIndex[key.get()];
    e.key = key;
    // an ite of promised nodes (a GEP over a phi): the index is one of its
    // leaves, each under its guard, so each leaf is an origin (the rule
    // must separate every pair of leaves)
    if (isOpX<ITE>(origin)) {
      std::vector<Expr> leaves, stack{origin};
      bool all = true;
      while (!stack.empty() && all) {
        Expr x = stack.back();
        stack.pop_back();
        if (isOpX<ITE>(x)) {
          stack.push_back(x->arg(1));
          stack.push_back(x->arg(2));
        } else if (op::ptr::isFlagged(x.get()) && leaves.size() < 64) {
          leaves.push_back(x);
        } else {
          all = false;
        }
      }
      if (all) {
        for (const Expr &l : leaves)
          if (std::find(e.nodes.begin(), e.nodes.end(), l) == e.nodes.end())
            e.nodes.push_back(l);
        return;
      }
    }
    if (!op::ptr::isFlagged(origin.get())) {
      e.plain = true;
      return;
    }
    if (std::find(e.nodes.begin(), e.nodes.end(), origin) == e.nodes.end())
      e.nodes.push_back(origin);
  }
};

/// The lowering (see the header). One instance per query; it keeps the table
/// of reads that AckermannModel needs: destroy it before the ExprFactory.
class Ackermannizer {
public:
  struct Stats {
    uint64_t arrays = 0;  ///< array constants in the query
    uint64_t lowered = 0; ///< of them only read, lowered
    uint64_t kept = 0;    ///< used otherwise (or not bv -> bv), kept
    uint64_t stores = 0;  ///< store nodes in the query
    uint64_t binders = 0; ///< quantifier / lambda nodes (none lowered)
    uint64_t reads = 0;   ///< distinct reads of lowered arrays
    uint64_t pairs = 0;   ///< read pairs of one array, n(n-1)/2 summed
    uint64_t numeralPairs =
        0;                    ///< of them two different numerals: no constraint
    uint64_t offsetPairs = 0; ///< same symbolic part, different offsets: none
    /// different symbolic parts whose difference is a multiple of 2^t
    /// (every coefficient of both), offsets differing below 2^t: none
    uint64_t residuePairs = 0;
    /// never equal leaf by leaf (through ites) or by the layout (constants
    /// with a known value): no constraint
    uint64_t leafPairs = 0;
    /// never equal by the stride rule (trusted GEP promises, PtrIndex.md
    /// section 5): no constraint
    uint64_t stridePairs = 0;
    /// why the stride rule left a constrained pair (with origins set): a
    /// read not eligible (some address not a promised node), different
    /// bases or operators (or K != 0), or the gcd divides the difference
    uint64_t strideNotEligible = 0, strideOtherBase = 0, strideDivides = 0;
    /// of stridePairs, those with a byte of a wide access (K != 0): they
    /// also rest on that access's A-INB (its span is in the space)
    uint64_t strideAInbPairs = 0;
    uint64_t constraints = 0; ///< constraints added (the other pairs)
  };
  /// One distinct read of a lowered array: its index (lowered: in terms of
  /// the fresh constants) and the fresh constant r standing for the cell.
  struct Read {
    Expr index;
    Expr value;
    size_t group;     ///< the symbolic part (0: none, a numeral index)
    mpz_class offset; ///< the numeral part, modulo 2^w
    /// the access addresses of every index term merged into this read
    /// (setAccessOrigins): flagged nodes, plain if any was not one
    std::vector<Expr> nodes;
    bool plain = true;
  };
  struct Array {
    Expr array; ///< the array constant A
    std::vector<Read> reads;
  };

  /// Scans \p root: counts stores and array constants, decides which arrays
  /// are only read. Called by lower() if not called before.
  void analyze(const Expr &root) {
    using namespace ackermann_detail;
    m_analyzed = true;
    std::unordered_set<ENode *> seen;
    std::unordered_set<ENode *> bad;
    std::vector<ENode *> stack{root.get()};
    if (isArrayConstant(root.get()))
      bad.insert(root.get());
    while (!stack.empty()) {
      ENode *n = stack.back();
      stack.pop_back();
      if (!seen.insert(n).second)
        continue;
      if (isArrayConstant(n)) {
        m_candidates.push_back(Expr(n));
        continue;
      }
      if (isLeaf(n))
        continue;
      if (isOpX<STORE>(n))
        ++m_stats.stores;
      if (isOpX<FORALL>(n) || isOpX<EXISTS>(n) || isOpX<LAMBDA>(n))
        ++m_stats.binders;
      for (size_t i = 0; i < n->arity(); ++i) {
        ENode *k = n->arg(i);
        if (isArrayConstant(k) && !(isOpX<SELECT>(n) && i == 0))
          bad.insert(k);
        stack.push_back(k);
      }
    }
    m_stats.arrays = m_candidates.size();
    for (const Expr &a : m_candidates) {
      Expr sort = op::bind::declaredSort(a);
      bool bvbv = isOpX<BVSORT>(sort::arrayIndexTy(sort)) &&
                  isOpX<BVSORT>(sort::arrayValTy(sort));
      // under a binder a read may depend on a bound variable: lower nothing
      if (bad.count(a.get()) || !bvbv || m_stats.binders) {
        ++m_stats.kept;
        continue;
      }
      ++m_stats.lowered;
      m_index.emplace(a.get(), m_arrays.size());
      m_arrays.push_back(Array{a, {}});
    }
  }

  /// Number of store nodes of the analyzed query.
  bool hasStore() const { return m_stats.stores != 0; }

  /// \p root with every read of a lowered array replaced by its constant, and
  /// the constraints conjoined.
  Expr lower(const Expr &root) {
    using namespace ackermann_detail;
    if (!m_analyzed)
      analyze(root);
    if (m_arrays.empty())
      return root;
    // symbolic parts -> group number; 0 is the empty one (numerals)
    std::map<LinearForms::Key, size_t> groups{{LinearForms::Key(), 0}};
    // per group: its symbolic part (atoms with coefficients, ordered)
    std::vector<const LinearForms::Key *> groupKey{&groups.begin()->first};
    // linear forms per index width
    std::map<unsigned, LinearForms> forms;
    // per array: (group, offset) -> read number
    std::vector<std::map<std::pair<size_t, mpz_class>, size_t>> byKey(
        m_arrays.size());
    std::unordered_map<ENode *, Expr> memo;
    std::vector<Expr> keep;
    Expr body = rewriteDag(
        root, memo, keep, [](ENode *n) { return Expr(n); },
        [&](ENode *n, llvm::SmallVectorImpl<Expr> &kids) -> Expr {
          if (!isOpX<SELECT>(n))
            return withKids(n, kids);
          auto it = m_index.find(n->arg(0));
          if (it == m_index.end())
            return withKids(n, kids);
          size_t a = it->second;
          Expr idx = kids[1];
          unsigned w = op::bv::width(
              sort::arrayIndexTy(op::bind::declaredSort(m_arrays[a].array)));
          LinearForms &lf = forms.try_emplace(w, w).first->second;
          const Linear &lin = lf.of(idx.get());
          mpz_class off = lin.c;
          auto g = groups.emplace(lf.key(lin), groups.size()).first;
          if (g->second == groupKey.size())
            groupKey.push_back(&g->first);
          auto [p, fresh] = byKey[a].emplace(std::make_pair(g->second, off),
                                             m_arrays[a].reads.size());
          if (!fresh) {
            noteOrigins(m_arrays[a].reads[p->second], n->arg(1), false);
            return m_arrays[a].reads[p->second].value;
          }
          Expr r = freshConstant(a);
          m_arrays[a].reads.push_back(Read{idx, r, g->second, off, {}, true});
          noteOrigins(m_arrays[a].reads.back(), n->arg(1), true);
          ++m_stats.reads;
          return r;
        });
    std::vector<Expr> conj{body};
    for (const Array &arr : m_arrays) {
      unsigned w =
          op::bv::width(sort::arrayIndexTy(op::bind::declaredSort(arr.array)));
      LeafDecider leaf(w, m_values.empty() ? nullptr : &m_values);
      const auto &rs = arr.reads;
      for (size_t k = 0; k < rs.size(); ++k)
        for (size_t l = k + 1; l < rs.size(); ++l) {
          ++m_stats.pairs;
          if (rs[k].group == rs[l].group) {
            // S + c_k = S + c_l is false: c_k != c_l (else one read)
            ++(rs[k].group == 0 ? m_stats.numeralPairs : m_stats.offsetPairs);
            continue;
          }
          // S_k + c_k = S_l + c_l needs S_k - S_l = c_l - c_k (mod 2^w); if
          // every coefficient of S_k - S_l is a multiple of 2^t and c_l - c_k
          // is not, the indices are never equal (an identity of w-bit
          // arithmetic, no assumption): no constraint
          {
            unsigned t = diffTrailingZeros(*groupKey[rs[k].group],
                                           *groupKey[rs[l].group], w);
            mpz_class dc = rs[l].offset - rs[k].offset;
            if (t > 0 && mpzTrailingZeros(dc, w) < t) {
              ++m_stats.residuePairs;
              continue;
            }
          }
          // leaf by leaf through ites, with the layout values
          if (leaf.neverEqual(rs[k].index.get(), rs[l].index.get())) {
            ++m_stats.leafPairs;
            continue;
          }
          // the stride rule (trusted GEP promises), after the exact tests
          if (bool aInb = false; strideApart(rs[k], rs[l], aInb)) {
            ++m_stats.stridePairs;
            if (aInb)
              ++m_stats.strideAInbPairs;
            continue;
          }
          ++m_stats.constraints;
          if (m_origins)
            countStrideMiss(rs[k], rs[l]);
          if (m_recordPairs)
            m_pairs.push_back({size_t(&arr - m_arrays.data()), k, l});
          conj.push_back(mk<OR>(mk<NEG>(mk<EQ>(rs[k].index, rs[l].index)),
                                mk<EQ>(rs[k].value, rs[l].value)));
        }
    }
    return conj.size() == 1 ? body : mknary<AND>(conj.begin(), conj.end());
  }

  const Stats &stats() const { return m_stats; }
  /// A pair of reads of one array that got a constraint (no reduction
  /// decided it): array position, read numbers k < l (arrays()[a].reads).
  struct Pair {
    size_t array, k, l;
  };
  /// Record the constrained pairs in lower() (for the equality audit,
  /// sea-vc --eq-audit); off by default (a pair per constraint).
  void recordPairs(bool b) { m_recordPairs = b; }
  /// Constants whose value every model of the query fixes (the layout,
  /// obj.k = <numeral>: unconditional facts of the query); used only to drop
  /// pairs that are never equal (leafPairs), not to share reads.
  void setValues(std::unordered_map<ENode *, mpz_class> v) {
    m_values = std::move(v);
  }
  const std::vector<Pair> &constrainedPairs() const { return m_pairs; }
  /// Did the stride rule drop the constraint of reads \p a and \p b?
  bool strideDropped(const Read &a, const Read &b) const {
    return strideApart(a, b);
  }
  /// Enables the stride rule (PtrIndex.md section 5) with the access
  /// addresses of the read indices; null (the default) disables it.
  void setAccessOrigins(const AccessOrigins *o) { m_origins = o; }
  /// The bases of objects of size 0 (the stride rule: a read at a node over
  /// one is vacuous under A-INB).
  void setSize0Bases(std::unordered_set<ENode *> b) { m_size0 = std::move(b); }
  /// Off: no stride firing that rests on A-INB (K != 0, size-0 bases).
  void setAssumeInBounds(bool b) { m_inb = b; }
  /// The lowered arrays with their reads (the table of model reconstruction).
  const std::vector<Array> &arrays() const { return m_arrays; }
  /// The position of \p a in arrays(), or -1 if it is not lowered.
  long arrayIndex(const Expr &a) const {
    auto it = m_index.find(a.get());
    return it == m_index.end() ? -1 : long(it->second);
  }

private:
  /// Merge the access addresses of index term \p idx into \p r (\p first:
  /// r's first index term).
  void noteOrigins(Read &r, ENode *idx, bool first) {
    if (!m_origins)
      return;
    auto it = m_origins->byIndex.find(idx);
    if (it == m_origins->byIndex.end() || it->second.plain ||
        it->second.nodes.empty()) {
      r.plain = true;
      r.nodes.clear();
      return;
    }
    if (first)
      r.plain = false;
    if (r.plain)
      return;
    for (const Expr &n : it->second.nodes)
      if (std::find(r.nodes.begin(), r.nodes.end(), n) == r.nodes.end())
        r.nodes.push_back(n);
  }

  /// The stride rule (PtrIndex.md section 5, the Ackermann case: cells,
  /// u = 1, K = 0; premise PremPtrIndex only). Reads a and b are never at
  /// one index if every pair of their access addresses (flagged nodes N_a,
  /// N_b) has:
  ///  - the same operator (NUSW or NUW: the indices read as the same
  ///    integers) and the same base term p, K = 0 on both;
  ///  - with O_j = sum x_k * s_k + c the integer offsets (x_k the signed
  ///    value of the index term for NUSW, unsigned for NUW): by P3 both
  ///    p + O_a and p + O_b are integers in [0, 2^n), so they are equal mod
  ///    2^n iff O_a = O_b; that is sum (coef_x * x) = c_b - c_a over the
  ///    distinct index terms x (coef: the strides of a minus those of b),
  ///    and it has no integer solution if g = gcd(coef) does not divide
  ///    D = c_b - c_a (no solution at all for g = 0, D != 0). Distinct index
  ///    terms are taken as independent: conservative.
  /// Every access address of a read must be such a node: an access of the
  /// same cell through a ptr.add (plain) makes the read ineligible, since
  /// the promise holds only where the GEP is used (PtrIndex.md).
  bool strideApart(const Read &a, const Read &b) const {
    bool aInb = false;
    return strideApart(a, b, aInb);
  }
  /// \p aInb: the answer also rests on A-INB (a byte of a wide access, K !=
  /// 0, or a node over an object of size 0).
  bool strideApart(const Read &a, const Read &b, bool &aInb) const {
    if (!m_origins || a.plain || b.plain || a.nodes.empty() || b.nodes.empty())
      return false;
    for (const Expr &na : a.nodes)
      for (const Expr &nb : b.nodes)
        if (!strideApartNodes(na, nb, aInb))
          return false;
    return true;
  }
  /// A node over an object of size 0 (null, a zero-sized allocation): no
  /// access of positive width is in it under A-INB (cmp.size0-vacuous), so
  /// a read at it never happens.
  bool size0Base(const Expr &n) const {
    return m_size0.count(op::ptr::base(n).get()) != 0;
  }
  void countStrideMiss(const Read &a, const Read &b) {
    if (a.plain || b.plain || a.nodes.empty() || b.nodes.empty()) {
      ++m_stats.strideNotEligible;
      return;
    }
    for (const Expr &na : a.nodes)
      for (const Expr &nb : b.nodes)
        if (isOpX<op::PTR_INDEX_NUSW>(na) != isOpX<op::PTR_INDEX_NUSW>(nb) ||
            op::ptr::base(na) != op::ptr::base(nb)) {
          ++m_stats.strideOtherBase;
          return;
        }
    ++m_stats.strideDivides;
  }
  bool strideApartNodes(const Expr &a, const Expr &b, bool &aInb) const {
    using namespace op;
    if (size0Base(a) || size0Base(b)) {
      aInb = true;
      return m_inb;
    }
    if (ptr::byteOffset(a) != mpz_class(0L) ||
        ptr::byteOffset(b) != mpz_class(0L)) {
      if (!m_inb)
        return false;
      aInb = true;
    }
    if (isOpX<PTR_INDEX_NUSW>(a) != isOpX<PTR_INDEX_NUSW>(b) ||
        ptr::base(a) != ptr::base(b))
      return false;
    std::map<ENode *, mpz_class> coef;
    for (size_t k = 0; k < ptr::numIndices(a); ++k) {
      mpz_class &c = coef[ptr::index(a, k).get()];
      c = c + ptr::stride(a, k);
    }
    for (size_t k = 0; k < ptr::numIndices(b); ++k) {
      mpz_class &c = coef[ptr::index(b, k).get()];
      c = c - ptr::stride(b, k);
    }
    mpz_class g = 0L;
    for (const auto &[x, c] : coef)
      mpz_gcd(g.get_mpz_t(), g.get_mpz_t(), c.get_mpz_t());
    mpz_class d = (ptr::constant(b) + ptr::byteOffset(b)) -
                  (ptr::constant(a) + ptr::byteOffset(a));
    if (g == mpz_class(0L))
      return d != mpz_class(0L);
    return !mpz_divisible_p(d.get_mpz_t(), g.get_mpz_t());
  }

  /// Trailing zeros of x modulo 2^w (w for 0).
  static unsigned mpzTrailingZeros(const mpz_class &x, unsigned w) {
    mpz_class m = mpz_class(1UL);
    mpz_mul_2exp(m.get_mpz_t(), m.get_mpz_t(), w);
    mpz_class r;
    mpz_fdiv_r(r.get_mpz_t(), x.get_mpz_t(), m.get_mpz_t());
    if (r == 0)
      return w;
    return unsigned(mpz_scan1(r.get_mpz_t(), 0));
  }
  /// The trailing zeros common to the coefficients of a - b (symbolic parts,
  /// atoms in the same order; w if they are equal).
  template <typename Key>
  static unsigned diffTrailingZeros(const Key &a, const Key &b, unsigned w) {
    unsigned t = w;
    auto i = a.begin(), j = b.begin();
    while ((i != a.end() || j != b.end()) && t > 0) {
      if (j == b.end() || (i != a.end() && i->first < j->first)) {
        t = std::min(t, mpzTrailingZeros(i->second, w));
        ++i;
      } else if (i == a.end() || j->first < i->first) {
        t = std::min(t, mpzTrailingZeros(j->second, w));
        ++j;
      } else {
        mpz_class d = i->second - j->second;
        t = std::min(t, mpzTrailingZeros(d, w));
        ++i;
        ++j;
      }
    }
    return t;
  }

  Expr freshConstant(size_t a) {
    const Array &arr = m_arrays[a];
    std::string name;
    llvm::raw_string_ostream os(name);
    os << "ack." << *op::bind::fname(arr.array->first()) << "."
       << arr.reads.size();
    Expr sort = sort::arrayValTy(op::bind::declaredSort(arr.array));
    return op::bind::mkConst(mkTerm<std::string>(os.str(), arr.array->efac()),
                             sort);
  }

  bool m_analyzed = false;
  bool m_recordPairs = false;
  std::unordered_map<ENode *, mpz_class> m_values;
  std::vector<Pair> m_pairs;
  const AccessOrigins *m_origins = nullptr;
  std::unordered_set<ENode *> m_size0;
  bool m_inb = true;
  Stats m_stats;
  std::vector<Expr> m_candidates;
  std::vector<Array> m_arrays;
  std::unordered_map<ENode *, size_t> m_index;
};

/// A model of the original formula from a model of the lowered one (see the
/// header). Wraps \p inner; \p ack must outlive it.
class AckermannModel : public sea::solver::Model {
public:
  AckermannModel(sea::solver::Model &inner, const Ackermannizer &ack)
      : m_inner(inner), m_ack(ack), m_funcs(ack.arrays().size()),
        m_built(ack.arrays().size(), false) {}

  /// The finite function of lowered array number \p a: index value -> cell
  /// value (numerals), built from the inner model on first use.
  const std::map<mpz_class, Expr> &function(size_t a) {
    build(a);
    return m_funcs[a];
  }

  /// Read pairs whose index values are equal but whose cell values differ in
  /// the inner model: 0 unless the constraints were not given to the solver
  /// (a bug).
  uint64_t inconsistent() const { return m_inconsistent; }
  /// Read pairs at one index value with different cells whose constraint
  /// the stride rule dropped: the model breaks a trusted GEP promise (a
  /// wrapping offset; poison, UB in LLVM). Not a lowering bug: sea-vc
  /// reruns without the rule (--ack-stride).
  uint64_t strideBroken() const { return m_strideBroken; }
  /// Reads (select(A, j) evaluated) at an index the VC did not read: default 0.
  uint64_t defaulted() const { return m_defaulted; }

  /// A read select(M, j) of a memory M that contains a lowered array is
  /// evaluated concretely: j to its value v, then M walked down (a store at an
  /// index of value v gives its value, an ite its chosen branch) to a lowered
  /// A, read as function(A)(v), 0 at an index not in it, or to a memory with
  /// no lowered array, read by the inner model. Any other occurrence of a
  /// lowered A (e.g. an equality of arrays) becomes the store chain of
  /// function(A) over a constant-0 array (Yices cannot evaluate those: its
  /// const arrays are lambdas). The rest is evaluated by the inner model. Null
  /// if an index or a condition has no value (then every later call too).
  Expr eval(Expr e, bool complete) override {
    using namespace ackermann_detail;
    if (m_broken)
      return Expr();
    Expr t = rewriteDag(
        e, m_memo, m_keep, [](ENode *n) { return Expr(n); },
        [&](ENode *n, llvm::SmallVectorImpl<Expr> &kids) -> Expr {
          if (m_broken || !isOpX<SELECT>(n) || !hasLowered(n->arg(0)))
            return withKids(n, kids);
          Expr j = m_inner.eval(kids[1], /*complete=*/true);
          if (!j || !op::bv::is_bvnum(j)) {
            m_broken = true;
            return withKids(n, kids);
          }
          Expr v =
              readMem(Expr(n->arg(0)), reducedValue(j), op::bv::widthBvNum(j));
          return v ? v : withKids(n, kids);
        });
    if (m_broken)
      return Expr();
    if (hasLowered(t.get()))
      t = rewriteDag(
          t, m_asArray, m_keep,
          [&](ENode *n) -> Expr {
            long a = isArrayConstant(n) ? m_ack.arrayIndex(Expr(n)) : -1;
            return a < 0 ? Expr(n) : asArray(size_t(a));
          },
          [](ENode *n, llvm::SmallVectorImpl<Expr> &kids) {
            return withKids(n, kids);
          });
    return m_inner.eval(t, complete);
  }

  /// A lowered array's cell is a default iff no read of the query has its
  /// index (cell() gives 0 there); other arrays: the inner model's answer.
  bool defaultCell(Expr arr, Expr idx) override {
    using namespace ackermann_detail;
    long a = arr && isArrayConstant(arr.get()) ? m_ack.arrayIndex(arr) : -1;
    if (a < 0)
      return m_inner.defaultCell(arr, idx);
    if (!idx || !op::bv::is_bvnum(idx))
      return false;
    build(size_t(a));
    return !m_funcs[a].count(reducedValue(idx));
  }

  void print(llvm::raw_ostream &o) const override {
    m_inner.print(o);
    const auto &arrs = m_ack.arrays();
    for (size_t a = 0; a < arrs.size(); ++a) {
      o << "; ackermann " << *op::bind::fname(arrs[a].array->first()) << ":";
      if (m_built[a])
        for (const auto &[i, v] : m_funcs[a])
          o << " " << i.to_string() << "->" << v;
      else
        o << " (not evaluated)";
      o << " else 0\n";
    }
  }

private:
  void build(size_t a) {
    using namespace ackermann_detail;
    if (m_built[a])
      return;
    m_built[a] = true;
    std::map<mpz_class, const Ackermannizer::Read *> first;
    for (const Ackermannizer::Read &rd : m_ack.arrays()[a].reads) {
      Expr i = m_inner.eval(rd.index, /*complete=*/true);
      Expr v = m_inner.eval(rd.value, /*complete=*/true);
      if (!i || !v || !op::bv::is_bvnum(i) || !op::bv::is_bvnum(v)) {
        ++m_inconsistent;
        continue;
      }
      unsigned w = op::bv::widthBvNum(v);
      Expr vn = op::bv::bvnum(reducedValue(v), w, v->efac());
      auto [it, fresh] = m_funcs[a].emplace(reducedValue(i), vn);
      if (fresh)
        first.emplace(reducedValue(i), &rd);
      else if (it->second != vn)
        ++(m_ack.strideDropped(*first.at(reducedValue(i)), rd)
               ? m_strideBroken
               : m_inconsistent);
    }
  }

  /// Does \p n contain a lowered array? Memoized, iterative.
  bool hasLowered(ENode *root) {
    using namespace ackermann_detail;
    std::vector<std::pair<ENode *, bool>> stack{{root, false}};
    while (!stack.empty()) {
      auto [n, expanded] = stack.back();
      if (m_has.count(n)) {
        stack.pop_back();
        continue;
      }
      if (isLeaf(n)) {
        stack.pop_back();
        m_has.emplace(n, isArrayConstant(n) && m_ack.arrayIndex(Expr(n)) >= 0);
        m_keep.push_back(Expr(n));
        continue;
      }
      if (!expanded) {
        stack.back().second = true;
        for (size_t i = 0; i < n->arity(); ++i)
          if (!m_has.count(n->arg(i)))
            stack.push_back({n->arg(i), false});
        continue;
      }
      stack.pop_back();
      bool h = false;
      for (size_t i = 0; i < n->arity() && !h; ++i)
        h = m_has.find(n->arg(i))->second;
      m_has.emplace(n, h);
      m_keep.push_back(Expr(n));
    }
    return m_has.find(root)->second;
  }

  /// The value of cell \p idx (of width \p w) of memory \p mem; null if a
  /// store index or an ite condition on the way has no value.
  Expr readMem(Expr mem, const mpz_class &idx, unsigned w) {
    using namespace ackermann_detail;
    while (true) {
      if (!hasLowered(mem.get()))
        return m_inner.eval(
            op::array::select(mem, op::bv::bvnum(idx, w, mem->efac())),
            /*complete=*/true);
      long a = isArrayConstant(mem.get()) ? m_ack.arrayIndex(mem) : -1;
      if (a >= 0)
        return cell(size_t(a), idx);
      if (isOpX<STORE>(mem)) {
        Expr i = eval(mem->arg(1), /*complete=*/true);
        if (!i || !op::bv::is_bvnum(i))
          return Expr();
        if (reducedValue(i) == idx)
          return eval(mem->arg(2), /*complete=*/true);
        mem = mem->arg(0);
        continue;
      }
      if (isOpX<ITE>(mem)) {
        Expr c = eval(mem->arg(0), /*complete=*/true);
        if (!c || !(isOpX<TRUE>(c) || isOpX<FALSE>(c)))
          return Expr();
        mem = isOpX<TRUE>(c) ? mem->arg(1) : mem->arg(2);
        continue;
      }
      // another memory term over a lowered array: as an array term
      return Expr();
    }
  }

  Expr zero(size_t a) {
    const Expr &arr = m_ack.arrays()[a].array;
    unsigned w = op::bv::width(sort::arrayValTy(op::bind::declaredSort(arr)));
    return op::bv::bvnum(mpz_class(0UL), w, arr->efac());
  }

  Expr cell(size_t a, const mpz_class &idx) {
    build(a);
    auto it = m_funcs[a].find(idx);
    if (it != m_funcs[a].end())
      return it->second;
    ++m_defaulted;
    return zero(a);
  }

  /// The lowered array as an array term: stores over const-array 0.
  Expr asArray(size_t a) {
    build(a);
    const Expr &arr = m_ack.arrays()[a].array;
    Expr sort = op::bind::declaredSort(arr);
    Expr isort = sort::arrayIndexTy(sort);
    unsigned iw = op::bv::width(isort);
    Expr res = op::array::constArray(isort, zero(a));
    for (const auto &[i, v] : m_funcs[a])
      res = op::array::store(res, op::bv::bvnum(i, iw, arr->efac()), v);
    return res;
  }

  sea::solver::Model &m_inner;
  const Ackermannizer &m_ack;
  std::vector<std::map<mpz_class, Expr>> m_funcs;
  std::vector<bool> m_built;
  std::unordered_map<ENode *, Expr> m_memo, m_asArray;
  std::unordered_map<ENode *, bool> m_has;
  std::vector<Expr> m_keep;
  bool m_broken = false;
  uint64_t m_inconsistent = 0;
  uint64_t m_strideBroken = 0;
  uint64_t m_defaulted = 0;
};

} // namespace expr
