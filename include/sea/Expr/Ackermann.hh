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
// The other drops are exact: residues, leaves, and the difference decider
// (DiffDecider.hh: intervals and congruences of the cancelled difference,
// signed representatives, the layout values; on the index compared, at its
// width). One more (sea-vc default; off:
// --ack-stride=false): the stride rule (--ack-stride, docs/design/PtrIndex.md
// section 5): a pair of reads whose every address was a promised GEP node
// (inbounds / nusw / nuw) over one base, whose integer offsets can never be
// equal (the gcd of the stride differences does not divide the constant
// difference), or one at a node over an object of size 0. Dropping any
// constraint only weakens the lowered query: unsat stays sound with no premise.
// The promises (and A-NOWRAP for a byte of a wide access, A-NULL for a size-0
// base: SeaMRules.hh, the named premises) are what make the dropped constraints
// vacuous, so a sat model needs them. A promise is a fact only at an access the
// run performs: a model that puts two such reads at one index with different
// cells breaks one only if both reads execute in it (Read::exec, their loads'
// block guards). Such a pair is counted (AckermannModel::strideBroken); the
// promises are trusted front-end facts, so sea-vc answers sat with that model,
// and a replay that breaks a promise at an access instead of confirming the
// answer is reported as such (--reach reruns without the rule). A pair with a
// read that does not execute is counted (strideIdle) and breaks nothing. One
// more, off by default (sea-vc --ack-own): the ownership drop, a pair of reads
// of two different objects (every address of each index a pointer into its
// object: a load's read or a select the boundary made for a memory operation),
// resting on A-INB of the access behind one read and A-OWN (no access through
// one object touches another object's cells, reads included); a sat model that
// puts two such reads at one index with different cells is counted as
// inconsistent (AckermannModel::ownBroken). Otherwise no pair is dropped. An
// array is lowered only if every use of it is the array operand of a select: no
// store over it, no equality, no ite, no other operator (then it is kept as it
// is, `kept` counts it). Indices of reads may contain reads (of the same or
// other arrays): they are lowered first (bottom-up).
//
// Model reconstruction (AckermannModel): the model of the lowered formula has
// values for i_k and r_k; each lowered array A becomes the finite function
// {value(i_k) -> value(r_k)} (consistent: by the constraints, but for the
// dropped pairs: of two values at one index an executed read's wins, also
// for the ownership drop's pairs) with default 0 elsewhere. AckermannModel is a
// solver::Model whose eval reads every select(A, j) through that function (j
// evaluated first) and replaces any other occurrence of A by the store chain
// over a constant 0 array, so every consumer of the model (replay, validation)
// sees the same array.
//
// The UF form (setUf, sea-vc --ro-arrays=uf with the lowering off): the same
// arrays (only read, bv -> bv) become an uninterpreted function uf.<A> with
// A's own index and cell sorts, and each select(A, i) the application
// uf.<A>(i); no constraint is added (functional consistency is the solver's).
// The reads (one per distinct index term) are recorded as for the eager
// lowering, with value uf.<A>(i_k), so AckermannModel rebuilds A the same way.
#include "sea/Expr/DiffDecider.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/LinearForms.hh"
#include "sea/Expr/LowBits.hh"
#include "sea/Expr/Smt/Model.hh"
#include "sea/Support/Stats.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace expr {

namespace ackermann_detail {
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
    // the atoms in the maps' order (by id)
    const std::less<ENode *> before;
    auto i = x.coef.begin(), j = y.coef.begin();
    while ((i != x.coef.end() || j != y.coef.end()) && t > 0) {
      if (j == y.coef.end() ||
          (i != x.coef.end() && before(i->first, j->first))) {
        t = std::min(t, tz(i->second, w));
        ++i;
      } else if (i == x.coef.end() || before(j->first, i->first)) {
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
    } else if ((isOpX<BADD>(n) && n->arity() >= 1) ||
               (isOpX<BSUB>(n) && n->arity() == 2)) {
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
  /// at most this many distinct addresses per index are kept in addrs
  static constexpr size_t kMaxAddrs = 8;
  struct Entry {
    Expr key;
    std::vector<Expr> nodes; ///< distinct flagged nodes
    bool plain = false;
    /// every distinct read address (pointer-sorted, as the select had it;
    /// --narrow-offsets); `many` when there were more than kMaxAddrs
    std::vector<Expr> addrs;
    bool many = false;
    /// some select at this index reads a shadow memory (keyed by object
    /// identity, not a program access)
    bool shadow = false;
    /// some select at this index is not an own read of a program load
    /// (seam::Rules::noteOwnReads), or also a read made elsewhere (a
    /// memmove's source read, the boundary's translations)
    bool notOwn = false;
    /// of notOwn: an own read that is also made elsewhere (one node)
    bool shared = false;
    /// when some select at this index executes: the execution conditions
    /// (block guards, erased) of the loads whose own reads they are, at
    /// most kMaxExec; `execAlways` when a select here is not an own read
    /// with recorded conditions (a read made elsewhere, a shared one), or
    /// there were more (the model check takes such a read as executed)
    static constexpr size_t kMaxExec = 16;
    std::vector<Expr> exec;
    bool execAlways = false;
  };
  std::unordered_map<ENode *, Entry> byIndex;
  /// indices whose execution conditions were more than kMaxExec (then
  /// execAlways)
  uint64_t execOverflow = 0;
  /// \p key (expanded) was the read address \p origin (of a shadow memory:
  /// \p shadow)
  /// \p exec: the execution conditions of the select (null: unknown, taken
  /// as always)
  void add(const Expr &key, const Expr &origin, bool shadow = false,
           bool notOwn = false, bool shared = false,
           const std::vector<Expr> *exec = nullptr) {
    Entry &e = byIndex[key.get()];
    e.key = key;
    e.shadow = e.shadow || shadow;
    e.notOwn = e.notOwn || notOwn;
    e.shared = e.shared || shared;
    if (!exec) {
      e.execAlways = true;
    } else if (!e.execAlways) {
      for (const Expr &g : *exec)
        if (std::find(e.exec.begin(), e.exec.end(), g) == e.exec.end())
          e.exec.push_back(g);
      if (e.exec.size() > Entry::kMaxExec) {
        e.execAlways = true;
        ++execOverflow;
      }
    }
    if (e.execAlways)
      e.exec.clear();
    if (std::find(e.addrs.begin(), e.addrs.end(), origin) == e.addrs.end()) {
      if (e.addrs.size() < kMaxAddrs)
        e.addrs.push_back(origin);
      else
        e.many = true;
    }
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

/// The reads --narrow-offsets narrows (docs/design/Ackermann.md, "Narrowed
/// offsets"): per read index (the expanded index term of a select, as the
/// query has it before the lowering), the object its address is in. sea-vc
/// fills it with the indices whose every select is a program load's own read
/// (seam::Rules::noteOwnReads) into an object of known size N > 0;
/// k is the least width with 2^k >= N. The ownership drop (--ack-own, "The
/// ownership drop") takes a table of the same form, with the selects the
/// boundary made too (k unused). Under the A-INB of that access the
/// index is base + o with o in [0, N), so it equals base + zext(low_k(o)).
struct NarrowTable {
  struct Entry {
    Expr base;  ///< the object constant as the query has it (bv(w))
    unsigned k; ///< the offset width
  };
  std::unordered_map<ENode *, Entry> byIndex;
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
    /// of stridePairs, those with a read at a node over an object of size 0
    /// (they rest on A-NULL) and those with a byte of a wide access (K != 0:
    /// they rest on A-NOWRAP, the access address does not wrap)
    uint64_t strideANullPairs = 0, strideANoWrapPairs = 0;
    /// the difference decider (setDecider; DiffDecider.hh), on the pairs
    /// the tests above left: pairs it found never equal (no constraint), of
    /// them by class (residue, interval, congruence, signed: the weakest
    /// test a leaf pair needed), with the layout values (premise LAYOUT, a
    /// conjunct of the query), leaf by leaf (the unsplit form did not
    /// decide); pairs found always equal (kept); undecided pairs on which a
    /// leaf budget was hit
    bool decider = false;
    uint64_t deciderPairs = 0, deciderResidue = 0, deciderInterval = 0,
             deciderCongruence = 0, deciderSigned = 0, deciderLayout = 0,
             deciderSplit = 0, deciderAlways = 0, deciderBudget = 0;
    /// the ownership drop (setOwn; A-INB + A-OWN), on the pairs every rule
    /// above left: reads in one known object (setOwn's table), pairs of two
    /// such reads of different objects (dropped), of one object (kept), with a
    /// read not in one known object (kept)
    bool own = false;
    uint64_t ownReads = 0, ownPairs = 0, ownSameObject = 0, ownNotEligible = 0;
    uint64_t constraints = 0; ///< constraints added (the other pairs)
    /// --narrow-offsets (setNarrow): reads narrowed (their index is
    /// base + zext(low_k(offset))), objects they are in, the largest k;
    /// constraints compared at k bits (two narrowed reads of one object);
    /// sum of their k; constraints with one narrowed read and one other
    /// (64-bit, over base + zext(low_k(offset))); selects whose index was
    /// eligible but not of the form base + offset (not narrowed)
    uint64_t narrowReads = 0, narrowObjects = 0, narrowKMax = 0;
    uint64_t narrowPairs = 0, narrowPairBits = 0, narrowMixed = 0;
    uint64_t narrowShape = 0;

    /// What report() says of the stride rule: not asked (no line), used
    /// (its counters), asked but not used by this lowering (`stride:
    /// fallback`: the --reach rerun).
    enum class Stride { Off, On, Fallback };
    /// The `<prefix><counter>: <n>` lines of a lowering (sea-vc
    /// `vc.ackermann-`): arrays (= lowered), kept, reads, pairs, the
    /// dropped pairs by reason, the stride rule (\p stride), constraints.
    void report(sea::stats::Report &r, llvm::StringRef prefix,
                Stride stride) const {
      auto line = [&](const llvm::Twine &k) -> sea::stats::Line & {
        return r.line((prefix + k).str());
      };
      line("arrays").value(lowered);
      line("kept").value(kept);
      line("reads").value(reads);
      line("pairs").value(pairs);
      line("numeral-pairs").value(numeralPairs);
      line("offset-pairs").value(offsetPairs);
      line("residue-pairs").value(residuePairs);
      line("leaf-pairs").value(leafPairs);
      if (decider) {
        line("decider-pairs").value(deciderPairs);
        line("decider-residue").value(deciderResidue);
        line("decider-interval").value(deciderInterval);
        line("decider-congruence").value(deciderCongruence);
        line("decider-signed").value(deciderSigned);
        line("decider-layout").value(deciderLayout);
        line("decider-split").value(deciderSplit);
        line("decider-always").value(deciderAlways);
        line("decider-budget").value(deciderBudget);
      }
      if (stride == Stride::On) {
        line("stride-pairs").value(stridePairs);
        line("stride-a-null-pairs").value(strideANullPairs);
        line("stride-a-nowrap-pairs").value(strideANoWrapPairs);
        line("stride-not-eligible").value(strideNotEligible);
        line("stride-other-base").value(strideOtherBase);
        line("stride-divides").value(strideDivides);
      } else if (stride == Stride::Fallback)
        line("stride").value("fallback");
      if (own) {
        line("own-reads").value(ownReads);
        line("own-pairs").value(ownPairs);
        line("own-same-object").value(ownSameObject);
        line("own-not-eligible").value(ownNotEligible);
      }
      line("constraints").value(constraints);
    }
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
    /// the selects of the query merged into this read (keepImages)
    std::vector<Expr> selects;
    /// --narrow-offsets: the low k bits of the offset (a k-bit term; null:
    /// not narrowed) and the object base; index is then
    /// base + zext(low) (base + low's value for a numeral)
    Expr low;
    Expr nbase;
    /// --ack-own: the object every index term merged into this read is in
    /// (the own table's base); null if one is not in the table or two
    /// disagree
    Expr obase = Expr();
    /// when the read executes (setAccessOrigins): it does iff one of the
    /// conditions in exec is true; execAlways: unknown, taken as always
    /// (no origins, an index without an entry, or one with execAlways)
    std::vector<Expr> exec;
    bool execAlways = true;
    /// a narrowed read: its offset before narrowing (index = nbase + rest
    /// at w bits; null otherwise: the index itself is base + rest). The
    /// cells plan (AckCellsPlan.hh) enumerates its candidate offsets.
    Expr rest = Expr();
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
  /// the constraints conjoined (setUf: by the application of the array's
  /// function, no constraint).
  Expr lower(const Expr &root) {
    using namespace ackermann_detail;
    if (!m_analyzed)
      analyze(root);
    if (m_arrays.empty()) {
      m_identity = true; // image(): every node is its own
      return root;
    }
    if (m_uf)
      return lowerUf(root);
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
          // --narrow-offsets: the index's in-object representative
          Expr low, nbase, rest;
          if (m_narrow) {
            auto nt = m_narrow->byIndex.find(n->arg(1));
            if (nt != m_narrow->byIndex.end()) {
              if (nt->second.k < w &&
                  splitBase(idx, nt->second.base, w, rest)) {
                low = m_low.of(rest, nt->second.k);
                nbase = nt->second.base;
                idx = narrowIndex(nbase, low, w);
              } else {
                ++m_stats.narrowShape;
              }
            }
          }
          LinearForms &lf = forms.try_emplace(w, w).first->second;
          const Linear &lin = lf.of(idx.get());
          mpz_class off = lin.c;
          auto g = groups.emplace(lf.key(lin), groups.size()).first;
          if (g->second == groupKey.size())
            groupKey.push_back(&g->first);
          auto [p, fresh] = byKey[a].emplace(std::make_pair(g->second, off),
                                             m_arrays[a].reads.size());
          if (low && !m_narrowSel.count(n))
            m_narrowSel.emplace(
                n, NarrowedSelect{Expr(n), kids[1], idx, a, p->second});
          if (!fresh) {
            noteOrigins(m_arrays[a].reads[p->second], n->arg(1), false);
            noteOwn(m_arrays[a].reads[p->second], n->arg(1), false);
            if (m_keepImages)
              m_arrays[a].reads[p->second].selects.push_back(Expr(n));
            return m_arrays[a].reads[p->second].value;
          }
          Expr r = freshConstant(a);
          m_arrays[a].reads.push_back(
              Read{idx, r, g->second, off, {}, true, {}, low, nbase});
          if (low)
            m_arrays[a].reads.back().rest = rest;
          noteOrigins(m_arrays[a].reads.back(), n->arg(1), true);
          noteOwn(m_arrays[a].reads.back(), n->arg(1), true);
          if (m_keepImages)
            m_arrays[a].reads.back().selects.push_back(Expr(n));
          ++m_stats.reads;
          return r;
        });
    if (m_keepImages) {
      m_images = std::move(memo);
      m_imageKeep = std::move(keep);
    }
    std::vector<Expr> conj{body};
    // --narrow-offsets: the narrowed reads' objects and widths
    if (m_narrow) {
      std::unordered_set<ENode *> objs;
      for (const Array &arr : m_arrays)
        for (const Read &rd : arr.reads)
          if (rd.low) {
            ++m_stats.narrowReads;
            objs.insert(rd.nbase.get());
            m_stats.narrowKMax =
                std::max<uint64_t>(m_stats.narrowKMax, m_low.width(rd.low));
          }
      m_stats.narrowObjects = objs.size();
    }
    m_stats.own = m_own != nullptr;
    if (m_own) {
      m_ownDropped.assign(m_arrays.size(), {});
      for (const Array &arr : m_arrays)
        for (const Read &rd : arr.reads)
          m_stats.ownReads += rd.obase ? 1 : 0;
    }
    // linear forms and leaf tests of the narrowed offsets, per width k
    std::map<unsigned, LinearForms> lowForms;
    std::map<unsigned, LeafDecider> lowLeaves;
    // the difference deciders per width (w-bit indices with the layout
    // values; k-bit offsets without)
    std::map<unsigned, std::unique_ptr<DiffDecider>> deciders, lowDeciders;
    m_stats.decider = m_deciderOn;
    for (const Array &arr : m_arrays) {
      unsigned w =
          op::bv::width(sort::arrayIndexTy(op::bind::declaredSort(arr.array)));
      LeafDecider leaf(w, m_values.empty() ? nullptr : &m_values);
      const auto &rs = arr.reads;
      for (size_t k = 0; k < rs.size(); ++k)
        for (size_t l = k + 1; l < rs.size(); ++l) {
          ++m_stats.pairs;
          // two narrowed reads of one object: base + zext(t_k) = base +
          // zext(t_l) iff t_k = t_l; the exact drops at k bits, else a
          // k-bit constraint
          if (rs[k].low && rs[l].low && rs[k].nbase == rs[l].nbase &&
              m_low.width(rs[k].low) == m_low.width(rs[l].low)) {
            if (narrowPair(rs[k], rs[l], lowForms, lowLeaves, lowDeciders))
              continue;
            ++m_stats.constraints;
            ++m_stats.narrowPairs;
            m_stats.narrowPairBits += m_low.width(rs[k].low);
            if (m_recordPairs)
              m_pairs.push_back({size_t(&arr - m_arrays.data()), k, l});
            conj.push_back(mk<OR>(mk<NEG>(mk<EQ>(rs[k].low, rs[l].low)),
                                  mk<EQ>(rs[k].value, rs[l].value)));
            if (m_recordConstraints)
              m_constraints.push_back({size_t(&arr - m_arrays.data()), k, l,
                                       rs[k].low, rs[l].low, conj.back()});
            continue;
          }
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
          // the difference decider on the compared indices (w bits; a
          // narrowed read's index is base + zext(low))
          if (decided(deciders, w, &m_values, rs[k].index, rs[l].index))
            continue;
          // the stride rule (trusted GEP promises), after the exact tests
          if (StridePrem sp; strideApart(rs[k], rs[l], sp)) {
            ++m_stats.stridePairs;
            if (sp.null)
              ++m_stats.strideANullPairs;
            if (sp.noWrap)
              ++m_stats.strideANoWrapPairs;
            continue;
          }
          // the ownership drop (A-INB + A-OWN), after every other rule
          if (m_own) {
            if (rs[k].obase && rs[l].obase) {
              if (rs[k].obase != rs[l].obase) {
                ++m_stats.ownPairs;
                m_ownDropped[size_t(&arr - m_arrays.data())].insert(
                    ownKey(k, l, rs.size()));
                continue;
              }
              ++m_stats.ownSameObject;
            } else {
              ++m_stats.ownNotEligible;
            }
          }
          ++m_stats.constraints;
          if (rs[k].low || rs[l].low)
            ++m_stats.narrowMixed;
          if (m_origins)
            countStrideMiss(rs[k], rs[l]);
          if (m_recordPairs)
            m_pairs.push_back({size_t(&arr - m_arrays.data()), k, l});
          conj.push_back(mk<OR>(mk<NEG>(mk<EQ>(rs[k].index, rs[l].index)),
                                mk<EQ>(rs[k].value, rs[l].value)));
          if (m_recordConstraints)
            m_constraints.push_back({size_t(&arr - m_arrays.data()), k, l,
                                     rs[k].index, rs[l].index, conj.back()});
        }
    }
    if (m_recordConstraints)
      m_body = body;
    return conj.size() == 1 ? body : mknary<AND>(conj.begin(), conj.end());
  }

  const Stats &stats() const { return m_stats; }
  /// A pair of reads of one array that got a constraint (no reduction
  /// decided it): array position, read numbers k < l (arrays()[a].reads).
  struct Pair {
    size_t array, k, l;
  };
  /// A constraint lower() conjoined (recordConstraints): reads k < l of
  /// lowered array \p array, the compared terms (the indices, or the k-bit
  /// offsets of two narrowed reads of one object) and the conjunct itself,
  /// `lhs = rhs -> r_k = r_l`.
  struct Constraint {
    size_t array, k, l;
    Expr lhs, rhs;
    Expr fact;
  };
  /// Record, in lower(), every constraint it conjoins and the lowered body
  /// without them (sea-vc --ack-relax, docs/design/Ackermann.md "Lazy
  /// constraints"). The query lower() returns is the same either way:
  /// body() and constraints()' facts in order, one conjunction.
  void recordConstraints(bool b) { m_recordConstraints = b; }
  const std::vector<Constraint> &constraints() const { return m_constraints; }
  /// The lowered query without the constraints (recordConstraints; null if
  /// lower() lowered nothing).
  const Expr &body() const { return m_body; }
  /// Record the constrained pairs in lower() (for the equality audit,
  /// sea-vc --eq-audit); off by default (a pair per constraint).
  void recordPairs(bool b) { m_recordPairs = b; }
  /// Keep, after lower(), the image of every node of its input (image())
  /// and the selects merged into each read (Read::selects): sea-vc
  /// --arith-stats and --ack-dump-pairs follow the origins of the terms
  /// through the lowering (sea/VC/Origins.h). The nodes are kept alive.
  void keepImages(bool b) { m_keepImages = b; }
  /// The node lower() made of \p n (keepImages; null: not in its input).
  /// \p n must be alive: the memo is keyed by node address (its keys are
  /// kept alive, so a live \p n is never confused with a freed one).
  Expr image(const Expr &n) const {
    if (m_identity)
      return n;
    auto it = m_images.find(n.get());
    return it == m_images.end() ? Expr() : it->second;
  }
  /// Constants whose value every model of the query fixes (the layout,
  /// obj.k = <numeral>: unconditional facts of the query); used only to drop
  /// pairs that are never equal (leafPairs), not to share reads.
  void setValues(std::unordered_map<ENode *, mpz_class> v) {
    m_values = std::move(v);
  }
  const std::vector<Pair> &constrainedPairs() const { return m_pairs; }
  /// --narrow-offsets: narrow the reads whose index is in \p t (null, the
  /// default: none). Rests on A-INB for each narrowed read (counted).
  void setNarrow(const NarrowTable *t) { m_narrow = t; }
  /// A select of the query whose index was narrowed: the select, its index
  /// as lowered (64 bits, i) and the narrowed one (i'), the read it is.
  struct NarrowedSelect {
    Expr select, index, narrowed;
    size_t array, read;
  };
  /// The narrowed selects (by node of the query before the lowering).
  const std::unordered_map<ENode *, NarrowedSelect> &narrowedSelects() const {
    return m_narrowSel;
  }
  /// The read constant of narrowed select \p n; null if \p n is not one.
  Expr narrowedValue(const ENode *n) const {
    auto it = m_narrowSel.find(const_cast<ENode *>(n));
    return it == m_narrowSel.end()
               ? Expr()
               : m_arrays[it->second.array].reads[it->second.read].value;
  }
  /// The truncation of the narrowed reads (LowBits; --seam-check builds the
  /// same terms on the unlowered indices).
  LowBits &lowBits() { return m_low; }
  /// at most this many ite leaves in an index splitBase takes apart
  static constexpr unsigned kMaxSplitLeaves = 16;
  /// \p idx is base + rest at width \p w (a top-level bvadd with \p base
  /// as one summand, or \p base itself; an ite of such, at most
  /// kMaxSplitLeaves leaves: rest is the ite of the leaves' rests): \p rest.
  static bool splitBase(const Expr &idx, const Expr &base, unsigned w,
                        Expr &rest) {
    unsigned leaves = 0;
    return splitBase(idx, base, w, rest, leaves);
  }
  static bool splitBase(const Expr &idx, const Expr &base, unsigned w,
                        Expr &rest, unsigned &leaves) {
    if (isOpX<ITE>(idx)) {
      Expr r1, r2;
      if (!splitBase(Expr(idx->arg(1)), base, w, r1, leaves) ||
          !splitBase(Expr(idx->arg(2)), base, w, r2, leaves))
        return false;
      rest = mk<ITE>(Expr(idx->arg(0)), r1, r2);
      return true;
    }
    if (++leaves > kMaxSplitLeaves)
      return false;
    if (idx == base) {
      rest = op::bv::bvnum(mpz_class(0UL), w, idx->efac());
      return true;
    }
    if (!isOpX<BADD>(idx))
      return false;
    llvm::SmallVector<Expr, 8> others;
    unsigned seen = 0;
    for (size_t i = 0; i < idx->arity(); ++i) {
      if (idx->arg(i) == base.get())
        ++seen;
      else
        others.push_back(Expr(idx->arg(i)));
    }
    if (seen != 1)
      return false;
    if (others.empty())
      rest = op::bv::bvnum(mpz_class(0UL), w, idx->efac());
    else if (others.size() == 1)
      rest = others[0];
    else
      rest = idx->efac().mkNary(idx->op(), others.begin(), others.end());
    return true;
  }
  /// base + zext(low) at width \p w (base + its value for a numeral low,
  /// base for 0).
  static Expr narrowIndex(const Expr &base, const Expr &low, unsigned w) {
    if (op::bv::isBvNum(low)) {
      mpz_class v = ackermann_detail::reducedValue(low);
      if (v == 0L)
        return base;
      return mk<BADD>(base, op::bv::bvnum(v, w, base->efac()));
    }
    return mk<BADD>(base, op::bv::zext(low, w));
  }
  /// --ack-own (docs/design/Ackermann.md, "The ownership drop"): the reads
  /// whose index is in \p t (null, the default: none) are in its object; a
  /// pair of two such reads of different objects gets no constraint. Rests
  /// on A-INB of one read and A-OWN (counted).
  void setOwn(const NarrowTable *t) { m_own = t; }
  /// Did the ownership drop drop the constraint of reads \p k and \p l of
  /// lowered array \p a?
  bool ownDropped(size_t a, size_t k, size_t l) const {
    if (a >= m_ownDropped.size())
      return false;
    if (k > l)
      std::swap(k, l);
    return m_ownDropped[a].count(ownKey(k, l, m_arrays[a].reads.size()));
  }
  /// \p fn(k, l) on every pair k < l of reads of lowered array \p a whose
  /// constraint the ownership drop dropped (in no particular order).
  template <typename Fn>
  void forEachOwnDropped(size_t a, Fn fn) const {
    if (a >= m_ownDropped.size())
      return;
    const uint64_t n = m_arrays[a].reads.size();
    for (uint64_t key : m_ownDropped[a])
      fn(size_t(key / n), size_t(key % n));
  }
  /// Did the stride rule drop the constraint of reads \p a and \p b?
  bool strideDropped(const Read &a, const Read &b) const {
    return strideApart(a, b);
  }
  /// Enables the stride rule (PtrIndex.md section 5) with the access
  /// addresses of the read indices; null (the default) disables it.
  void setAccessOrigins(const AccessOrigins *o) { m_origins = o; }
  /// The bases of objects of size 0 (the stride rule: a read at a node over
  /// one is vacuous under A-NULL).
  void setSize0Bases(std::unordered_set<ENode *> b) { m_size0 = std::move(b); }
  /// Off: no stride firing that rests on a named premise (A-NOWRAP: K != 0;
  /// A-NULL: size-0 bases).
  void setAssumePremises(bool b) { m_premises = b; }
  /// The difference decider (DiffDecider.hh) on the pairs the other exact
  /// tests leave, before the stride rule; on by default. Exact (premise
  /// LAYOUT at most, a conjunct of the query).
  void setDecider(bool on, DiffDecider::Options opt = DiffDecider::Options()) {
    m_deciderOn = on;
    m_deciderOpt = opt;
  }
  /// The UF form (sea-vc --ro-arrays=uf): lower() replaces each read
  /// select(A, i) of a lowered array by uf.<A>(i), an uninterpreted function
  /// with A's index and cell sorts, and adds no constraint. The pair rules
  /// (and setNarrow, setOwn, setAccessOrigins) do not apply: there are no
  /// pairs. Off by default.
  void setUf(bool b) { m_uf = b; }
  bool uf() const { return m_uf; }
  /// The lowered arrays with their reads (the table of model reconstruction).
  const std::vector<Array> &arrays() const { return m_arrays; }
  /// Trailing zeros of \p x modulo 2^w (w for 0).
  static unsigned trailingZeros(const mpz_class &x, unsigned w) {
    return mpzTrailingZeros(x, w);
  }
  /// The position of \p a in arrays(), or -1 if it is not lowered.
  long arrayIndex(const Expr &a) const {
    auto it = m_index.find(a.get());
    return it == m_index.end() ? -1 : long(it->second);
  }

private:
  /// Merge the own object of index term \p idx into \p r (\p first: r's
  /// first index term): null when idx has none or another one.
  void noteOwn(Read &r, ENode *idx, bool first) {
    if (!m_own)
      return;
    auto it = m_own->byIndex.find(idx);
    Expr b = it == m_own->byIndex.end() ? Expr() : it->second.base;
    if (first)
      r.obase = b;
    else if (r.obase != b)
      r.obase = Expr();
  }
  static uint64_t ownKey(size_t k, size_t l, size_t n) {
    return uint64_t(k) * uint64_t(n) + uint64_t(l);
  }
  /// Merge the access addresses of index term \p idx into \p r (\p first:
  /// r's first index term).
  void noteOrigins(Read &r, ENode *idx, bool first) {
    if (!m_origins)
      return;
    auto it = m_origins->byIndex.find(idx);
    noteExec(r, it == m_origins->byIndex.end() ? nullptr : &it->second, first);
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

  /// Merge the execution conditions of an index term's entry \p e (null:
  /// none) into \p r (\p first: r's first index term).
  static void noteExec(Read &r, const AccessOrigins::Entry *e, bool first) {
    if (!e || e->execAlways) {
      r.execAlways = true;
      r.exec.clear();
      return;
    }
    if (first)
      r.execAlways = false;
    if (r.execAlways)
      return;
    for (const Expr &g : e->exec)
      if (std::find(r.exec.begin(), r.exec.end(), g) == r.exec.end())
        r.exec.push_back(g);
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
    StridePrem sp;
    return strideApart(a, b, sp);
  }
  /// The named premises a stride answer also rests on.
  struct StridePrem {
    bool null = false;   ///< A-NULL: a node over an object of size 0
    bool noWrap = false; ///< A-NOWRAP: a byte of a wide access (K != 0)
  };
  /// \p sp: the premises the answer also rests on.
  bool strideApart(const Read &a, const Read &b, StridePrem &sp) const {
    if (!m_origins || a.plain || b.plain || a.nodes.empty() ||
        b.nodes.empty() || a.low || b.low)
      return false;
    for (const Expr &na : a.nodes)
      for (const Expr &nb : b.nodes)
        if (!strideApartNodes(na, nb, sp))
          return false;
    return true;
  }
  /// A node over an object of size 0 (null, a zero-sized allocation): no
  /// access of positive width is in it under A-NULL (cmp.size0-vacuous), so
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
  bool strideApartNodes(const Expr &a, const Expr &b, StridePrem &sp) const {
    using namespace op;
    if (size0Base(a) || size0Base(b)) {
      sp.null = true;
      return m_premises;
    }
    if (ptr::byteOffset(a) != mpz_class(0L) ||
        ptr::byteOffset(b) != mpz_class(0L)) {
      if (!m_premises)
        return false;
      sp.noWrap = true;
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

  /// Two narrowed reads of one object never at one index by an exact test
  /// on their k-bit offsets (as the 64-bit ones: same symbolic part with
  /// different numerals, residues, leaves): counted with those drops.
  bool narrowPair(const Read &a, const Read &b,
                  std::map<unsigned, ackermann_detail::LinearForms> &forms,
                  std::map<unsigned, LeafDecider> &leaves,
                  std::map<unsigned, std::unique_ptr<DiffDecider>> &deciders) {
    using namespace ackermann_detail;
    unsigned k = m_low.width(a.low);
    LinearForms &lf = forms.try_emplace(k, k).first->second;
    const Linear la = lf.of(a.low.get());
    const Linear lb = lf.of(b.low.get());
    LinearForms::Key ka = lf.key(la), kb = lf.key(lb);
    if (ka == kb) {
      if (la.c == lb.c)
        return false; // equal offsets: the constraint is r_a = r_b
      ++(ka.empty() ? m_stats.numeralPairs : m_stats.offsetPairs);
      return true;
    }
    unsigned t = diffTrailingZeros(ka, kb, k);
    mpz_class dc = lb.c - la.c;
    if (t > 0 && mpzTrailingZeros(dc, k) < t) {
      ++m_stats.residuePairs;
      return true;
    }
    LeafDecider &ld = leaves.try_emplace(k, k, nullptr).first->second;
    if (ld.neverEqual(a.low.get(), b.low.get())) {
      ++m_stats.leafPairs;
      return true;
    }
    // the difference decider at k bits, on the offsets compared (no layout
    // values: the base cancels)
    return decided(deciders, k, nullptr, a.low, b.low);
  }

  /// The difference decider on \p a, \p b at width \p w (one decider per
  /// width in \p deciders): true if they are never equal (counted).
  bool decided(std::map<unsigned, std::unique_ptr<DiffDecider>> &deciders,
               unsigned w, const std::unordered_map<ENode *, mpz_class> *values,
               const Expr &a, const Expr &b) {
    if (!m_deciderOn)
      return false;
    auto &d = deciders[w];
    if (!d)
      d = std::make_unique<DiffDecider>(
          w, values && !values->empty() ? values : nullptr, m_deciderOpt);
    DiffDecider::Result r = d->decide(a.get(), b.get());
    if (r.verdict == DiffDecider::Verdict::Always)
      ++m_stats.deciderAlways;
    if (r.verdict != DiffDecider::Verdict::Never) {
      if (r.budget)
        ++m_stats.deciderBudget;
      return false;
    }
    ++m_stats.deciderPairs;
    switch (r.cls) {
    case DiffDecider::Class::None:
    case DiffDecider::Class::Residue:
      ++m_stats.deciderResidue;
      break;
    case DiffDecider::Class::Interval:
      ++m_stats.deciderInterval;
      break;
    case DiffDecider::Class::Congruence:
      ++m_stats.deciderCongruence;
      break;
    case DiffDecider::Class::Signed:
      ++m_stats.deciderSigned;
      break;
    }
    if (r.premise == DiffDecider::Premise::Layout)
      ++m_stats.deciderLayout;
    if (r.split)
      ++m_stats.deciderSplit;
    return true;
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
    // the atoms in the keys' order (by id, LinearForms::key)
    const std::less<ENode *> before;
    auto i = a.begin(), j = b.begin();
    while ((i != a.end() || j != b.end()) && t > 0) {
      if (j == b.end() || (i != a.end() && before(i->first, j->first))) {
        t = std::min(t, mpzTrailingZeros(i->second, w));
        ++i;
      } else if (i == a.end() || before(j->first, i->first)) {
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

  /// lower() in the UF form: one read per distinct index term (after the
  /// reads inside it are rewritten), its value the application.
  Expr lowerUf(const Expr &root) {
    using namespace ackermann_detail;
    std::vector<Expr> decls(m_arrays.size());
    std::vector<std::unordered_map<ENode *, size_t>> byIndex(m_arrays.size());
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
          Array &arr = m_arrays[a];
          Expr idx = kids[1];
          auto [p, fresh] = byIndex[a].emplace(idx.get(), arr.reads.size());
          if (!fresh) {
            if (m_keepImages)
              arr.reads[p->second].selects.push_back(Expr(n));
            return arr.reads[p->second].value;
          }
          if (!decls[a]) {
            std::string name;
            llvm::raw_string_ostream os(name);
            os << "uf." << *op::bind::fname(arr.array->first());
            Expr sort = op::bind::declaredSort(arr.array);
            Expr sig[] = {sort::arrayIndexTy(sort), sort::arrayValTy(sort)};
            decls[a] = op::bind::fdecl(
                mkTerm<std::string>(os.str(), arr.array->efac()), sig);
          }
          Expr r = op::bind::fapp(decls[a], idx);
          arr.reads.push_back(Read{idx, r, 0, mpz_class(0UL), {}, true, {}});
          if (m_keepImages)
            arr.reads.back().selects.push_back(Expr(n));
          ++m_stats.reads;
          return r;
        });
    if (m_keepImages) {
      m_images = std::move(memo);
      m_imageKeep = std::move(keep);
    }
    if (m_recordConstraints)
      m_body = body;
    return body;
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
  bool m_uf = false;
  bool m_recordPairs = false;
  bool m_recordConstraints = false;
  std::vector<Constraint> m_constraints;
  Expr m_body;
  bool m_keepImages = false;
  bool m_identity = false;
  std::unordered_map<ENode *, Expr> m_images;
  std::vector<Expr> m_imageKeep;
  std::unordered_map<ENode *, mpz_class> m_values;
  std::vector<Pair> m_pairs;
  const AccessOrigins *m_origins = nullptr;
  const NarrowTable *m_narrow = nullptr;
  std::unordered_map<ENode *, NarrowedSelect> m_narrowSel;
  const NarrowTable *m_own = nullptr;
  /// per lowered array: the pairs the ownership drop dropped (ownKey)
  std::vector<std::unordered_set<uint64_t>> m_ownDropped;
  LowBits m_low;
  std::unordered_set<ENode *> m_size0;
  bool m_premises = true;
  bool m_deciderOn = true;
  DiffDecider::Options m_deciderOpt;
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
  /// the stride rule dropped, both reads executed in the model (executed()):
  /// the model breaks a trusted GEP promise at an access the run performs
  /// (a wrapping offset; poison, UB in LLVM). Not a lowering bug: sea-vc
  /// answers sat with the model (--ack-stride; --reach reruns without the
  /// rule).
  uint64_t strideBroken() const { return m_strideBroken; }
  /// Such pairs with a read that does not execute in the model: no promise
  /// is a fact at its address (PtrIndex.md, "Where the promise is a fact"),
  /// so its index is free and nothing is broken. Its cell does not reach
  /// the array (an executed read at that index wins, build()).
  uint64_t strideIdle() const { return m_strideIdle; }
  /// Of inconsistent(): read pairs at one index value with different cells
  /// whose constraint the ownership drop (--ack-own) dropped. The model breaks
  /// A-INB of a read or A-OWN (an access through one object reaches another
  /// object's cells: a memory-safety error of the program), or puts a read
  /// that does not execute at another object's cell. Counted in
  /// inconsistent() too: sea-vc reports vc.ackermann-inconsistent (status 2),
  /// never an answer built on that model.
  uint64_t ownBroken() const { return m_ownBroken; }
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
          // a narrowed select is the cell the query read, at its narrowed
          // index (what the VC computed with; a run that reads elsewhere
          // differs from it: validation sees it)
          if (Expr r = m_ack.narrowedValue(n))
            return r;
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
    const auto &reads = m_ack.arrays()[a].reads;
    for (const Ackermannizer::Read &rd : reads) {
      Expr i = m_inner.eval(rd.index, /*complete=*/true);
      Expr v = m_inner.eval(rd.value, /*complete=*/true);
      if (!i || !v || !op::bv::is_bvnum(i) || !op::bv::is_bvnum(v)) {
        ++m_inconsistent;
        continue;
      }
      unsigned w = op::bv::widthBvNum(v);
      Expr vn = op::bv::bvnum(reducedValue(v), w, v->efac());
      auto [it, fresh] = m_funcs[a].emplace(reducedValue(i), vn);
      if (fresh) {
        first.emplace(reducedValue(i), &rd);
        continue;
      }
      // the representative of the index: the first executed read at it,
      // else the first read; every later read is compared with it, so
      // every executed read after the first executed one is compared with
      // that one
      const Ackermannizer::Read *&rep = first.at(reducedValue(i));
      const Ackermannizer::Read &f = *rep;
      const bool fExec = executed(f), rdExec = executed(rd);
      if (it->second != vn) {
        if (m_ack.strideDropped(f, rd)) {
          if (fExec && rdExec)
            ++m_strideBroken;
          else
            ++m_strideIdle;
        } else {
          if (m_ack.ownDropped(a, size_t(&f - reads.data()),
                               size_t(&rd - reads.data())))
            ++m_ownBroken;
          ++m_inconsistent;
        }
      }
      // the cell is an executed read's: a replay reads it there (a read
      // that does not execute only fills an index no executed read has);
      // also for pairs the ownership drop dropped
      if (!fExec && rdExec) {
        it->second = vn;
        rep = &rd;
      }
    }
  }

  /// Does read \p rd execute in the inner model: one of its execution
  /// conditions (Read::exec, the guards of the blocks of its loads) is
  /// true. Unknown (no conditions recorded, or one without a Boolean value
  /// in the model) counts as executed.
  bool executed(const Ackermannizer::Read &rd) {
    if (rd.execAlways)
      return true;
    for (const Expr &g : rd.exec) {
      auto [it, fresh] = m_execMemo.emplace(g.get(), true);
      if (fresh) {
        Expr v = m_inner.eval(g, /*complete=*/false);
        it->second = !(v && isOpX<FALSE>(v));
        m_keep.push_back(g);
      }
      if (it->second)
        return true;
    }
    return false;
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
  uint64_t m_strideIdle = 0;
  /// executed(): guard -> true unless the model makes it false
  std::unordered_map<ENode *, bool> m_execMemo;
  uint64_t m_ownBroken = 0;
  uint64_t m_defaulted = 0;
};

} // namespace expr
