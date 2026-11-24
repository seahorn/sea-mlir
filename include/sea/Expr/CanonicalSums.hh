// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Canonical linear forms of the solver-facing query (sea-vc
// --vc-canonical-sums; docs/design/CanonicalSums.md).
//
// An encoding step of the query after the solver boundary, the Ackermann
// lowering and the model-trail elimination: not a program change, not a SeaM
// rewrite (SeaM's own sum normal form, BvRules.hh, is untouched). A solver
// that bit-blasts the same linear form spelled in different ways builds
// adder chains that share no gates; this step spells every linear form one
// way.
//
// A linear node: bvadd (any arity), bvsub (binary), bvneg (unary), bvmul
// with at most one non-numeral factor f (when f is itself a proper sum, only
// with the factor 1 or -1: a general factor is not distributed over a sum,
// the product is a leaf). Its form is c + sum of k_i * leaf_i modulo 2^w.
// Every other bit-vector node is a leaf, with its kids rewritten first. A
// maximal linear term (a linear node used by a non-linear node) is emitted
// as
//     P,   (bvadd P c),   (bvadd (bvadd P (bvnot N)) c+1),
//     (bvnot N),   (bvadd (bvnot N) c+1),   or the numeral c
// (c+1 folded modulo 2^w; a zero numeral is not written; NegForm::Sub
// writes a negative chain as a subtraction instead: (bvsub P N),
// (bvsub (bvadd P c) N), (bvsub c N) with c also 0), where
//  - P is the n-ary bvadd of the positive leaves (one leaf: the leaf), N the
//    n-ary bvadd of the negative ones; a coefficient k > 2^(w-1) goes into
//    N as 2^w - k, every other coefficient k != 1 is (bvmul k leaf);
//  - leaves are ordered by their key (base, eff): the low eff bits of the
//    node base, zero-extended to the sum's width. x, (zext x),
//    (concat 0 x) and a low (extract k-1 0 x) share the base x, so the
//    order does not depend on the width; equal keys are one leaf (their
//    coefficients add), spelled as the first node of that key and width
//    met in the walk; bases are compared by creation id (ENode::getId), so
//    equal forms give one hash-consed chain;
//  - an extract over a sum is a leaf whose base is the (full-width,
//    canonical) sum: extracts are never pushed into a sum;
//  - bvnot, and every bitwise operator, is a leaf: ~x stays ~x.
// An equality with a linear side (a sum, not a single leaf or a numeral) is
// balanced (EqForm: Balance; Unshared only when a side is a sum used once;
// Keep never, each side then canonical on its own): the form D = a - b (or -D,
// the one with fewer negative leaves; ties by the leaf keys, then by the
// constant) becomes (= P -c), (= (bvadd P c) N) or (= P N), never one isolated
// term; an empty form is true or false.
//
// Every step is an identity of SMT-LIB bit-vector arithmetic (no
// assumption): the forms are equal to their terms modulo 2^w; P - N + c =
// P + ~N + (c + 1) = (P + c) - N (Sub), -N + c = c - N; a = b iff a - b = 0 iff
// P + c = N; zext / concat 0 / low extract of a leaf name its low bits. The
// unit test (sea-canonical-sums-test) checks each case with a z3 miter, and
// random linear terms at several widths with a miter (Yices, else Bitwuzla,
// else z3, with a limit) plus random ground points checked in z3 (more, and
// corner values, when the miter is unknown).
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprOpBv.hh"
#include "sea/Expr/SmallArith.hh"
#include "sea/Expr/TypeChecker.hh"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace expr {

class CanonicalSums {
public:
  /// How the negative chain N is written (sea-vc --vc-canonical-sums-neg):
  /// Bvnot, P + ~N + (c+1): one adder chain, the negation as inverted bits;
  /// Sub, (P + c) - N: the subtraction kept as such, for a solver that
  /// reads sums as polynomials and ~N as a bit-array term.
  enum class NegForm { Bvnot, Sub };

  /// Which equalities with a sum side are balanced (sea-vc
  /// --vc-canonical-sums-eq): Balance, every one; Unshared, one with a
  /// side that is a sum used once in the input (that equality is its only
  /// use), so an equality between shared sums keeps its two (canonical)
  /// sides; Keep, none: each side is canonical on its own.
  enum class EqForm { Balance, Unshared, Keep };

  /// What a linear node inside a sum is (sea-vc --vc-canonical-sums-inner):
  /// Flatten, its form is merged into its parent's (one flat form per
  /// maximal linear term); SharedLeaf, a proper sum used more than once in
  /// the input (the uses of EqForm::Unshared) is canonical on its own, one
  /// node, and a leaf of each parent, equalities included.
  enum class InnerForm { Flatten, SharedLeaf };

  explicit CanonicalSums(NegForm neg = NegForm::Bvnot,
                         EqForm eq = EqForm::Balance,
                         InnerForm inner = InnerForm::Flatten)
      : m_neg(neg), m_eq(eq), m_inner(inner) {}

  struct Stats {
    uint64_t sums = 0;          ///< maximal linear terms emitted (not a leaf)
    uint64_t sumsChanged = 0;   ///< of them, a different node than the input
    uint64_t leaves = 0;        ///< leaves of the emitted sums (P and N)
    uint64_t eqs = 0;           ///< balanced equalities
    uint64_t eqsChanged = 0;    ///< of them, a different node than the input
    uint64_t eqsKeptShared = 0; ///< Unshared: not balanced, both sides shared
    uint64_t eqsKept = 0;       ///< Keep: not balanced
    uint64_t innerShared = 0;   ///< SharedLeaf: shared sums kept as leaves
    uint64_t opaque = 0;        ///< bvsub / bvneg of another arity: leaves
  };

  /// \p query with every maximal linear term and every equality with a
  /// linear side in the canonical form.
  Expr rewrite(const Expr &query) {
    m_efac = &query->efac();
    if (m_eq == EqForm::Unshared || m_inner == InnerForm::SharedLeaf)
      countUses(query);
    walk(query);
    return imageOf(query.get());
  }

  const Stats &stats() const { return m_stats; }

  /// After rewrite(): the image of a node of its input query, made by no
  /// call (null: the node has none). A non-linear node and a maximal sum:
  /// its rewritten term. A linear node inside a sum is no term of the
  /// output; a product k * leaf (one leaf, no constant) has the term its
  /// leaf gets in a chain, (bvmul k' leaf) for k' != 1, which is in the output
  /// when its coefficient did not merge with another term's (the origins of
  /// --arith-stats follow a multiplication this way); else null.
  Expr image(const Expr &n) {
    auto it = m_new.find(n.get());
    if (it != m_new.end())
      return it->second;
    if (!isOpX<BMUL>(n) || !m_linear.count(n.get()))
      return Expr();
    const Form &f = m_form.at(n.get());
    if (f.c != 0 || f.coef.size() != 1)
      return Expr();
    std::vector<Item> pos, neg;
    split(f, pos, neg);
    const Item &item = pos.empty() ? neg[0] : pos[0];
    // coefficient 1: the leaf itself, which is not this product
    return item.k == 1 ? Expr() : leaf(item, f.w);
  }

private:
  /// the low eff bits of base, zero-extended
  struct Key {
    ENode *base;
    unsigned eff;
    bool operator<(const Key &o) const {
      return std::make_tuple(base->getId(), eff) <
             std::make_tuple(o.base->getId(), o.eff);
    }
    bool operator==(const Key &o) const {
      return base == o.base && eff == o.eff;
    }
  };
  /// c + sum coef(k) * leaf(k), modulo 2^w; coefficients nonzero, reduced
  struct Form {
    unsigned w = 0;
    mpz_class c;
    std::map<Key, mpz_class> coef;
    /// a proper sum: not a numeral and not one leaf with coefficient 1
    bool proper() const {
      return !coef.empty() &&
             !(coef.size() == 1 && c == 0 && coef.begin()->second == 1);
    }
  };
  struct Item {
    Key key;
    mpz_class k;
  };

  ExprFactory &efac() const { return *m_efac; }

  unsigned widthOf(ENode *n) {
    Expr t = m_tc.typeOf(Expr(n));
    return t && isOpX<op::BVSORT>(t) ? op::bv::width(t) : 0;
  }

  Expr num(const mpz_class &v, unsigned w) {
    return op::bv::bvnum(smallarith::modPow2(v, w), w, efac());
  }

  static bool isLinearOp(ENode *n) {
    return isOpX<BADD>(n) || isOpX<BSUB>(n) || isOpX<BNEG>(n) || isOpX<BMUL>(n);
  }

  /// post-order over the input: the form of each linear node, the rebuilt
  /// term of every other node
  void walk(const Expr &root) {
    std::vector<std::pair<ENode *, bool>> stack{{root.get(), false}};
    while (!stack.empty()) {
      auto [n, done] = stack.back();
      if (m_visited.count(n)) {
        stack.pop_back();
        continue;
      }
      if (!done) {
        stack.back().second = true;
        for (size_t i = n->arity(); i-- > 0;)
          if (!m_visited.count(n->arg(i)))
            stack.push_back({n->arg(i), false});
        continue;
      }
      stack.pop_back();
      m_visited.insert(n);
      m_pin.push_back(Expr(n));
      visit(n);
    }
  }

  void visit(ENode *n) {
    unsigned w;
    if (op::bv::isBvNum(Expr(n), w)) {
      Form f;
      f.w = w;
      f.c = smallarith::modPow2(op::bv::toMpz(Expr(n)), w);
      m_form.emplace(n, std::move(f));
      m_new.emplace(n, Expr(n));
      return;
    }
    if (isLinearOp(n)) {
      if (std::optional<Form> f = linear(n)) {
        m_form.emplace(n, std::move(*f));
        m_linear.insert(n);
        return;
      }
    }
    if (isOpX<EQ>(n) && n->arity() == 2 &&
        (isSum(n->arg(0)) || isSum(n->arg(1)))) {
      if (m_eq == EqForm::Keep) {
        ++m_stats.eqsKept;
      } else if (m_eq == EqForm::Unshared && !soleUse(n->arg(0)) &&
                 !soleUse(n->arg(1))) {
        ++m_stats.eqsKeptShared;
      } else {
        m_new.emplace(n, balance(n));
        return;
      }
    }
    rebuild(n);
  }

  /// SharedLeaf: \p k is a proper sum with more than one use, a leaf of
  /// its parents (counted once)
  bool sharedLeaf(ENode *k) {
    if (m_inner != InnerForm::SharedLeaf || !isSum(k))
      return false;
    auto it = m_uses.find(k);
    if (it == m_uses.end() || it->second < 2)
      return false;
    if (m_sharedSeen.insert(k).second)
      ++m_stats.innerShared;
    return true;
  }

  /// a sum whose only use in the input is one argument slot (Unshared)
  bool soleUse(ENode *side) const {
    if (!isSum(side))
      return false;
    auto it = m_uses.find(side);
    return it != m_uses.end() && it->second == 1;
  }

  /// the uses of every node of the input: argument slots of its distinct
  /// parents (a parent with the node in two slots counts two)
  void countUses(const Expr &root) {
    std::unordered_set<ENode *> seen;
    std::vector<ENode *> stack{root.get()};
    while (!stack.empty()) {
      ENode *n = stack.back();
      stack.pop_back();
      if (!seen.insert(n).second)
        continue;
      for (size_t i = 0; i < n->arity(); ++i) {
        ++m_uses[n->arg(i)];
        stack.push_back(n->arg(i));
      }
    }
  }

  /// a linear node with a proper form
  bool isSum(ENode *n) const {
    if (!m_linear.count(n))
      return false;
    return m_form.at(n).proper();
  }

  /// the form of a linear op node; none: the node is a leaf
  std::optional<Form> linear(ENode *n) {
    if ((isOpX<BSUB>(n) && n->arity() != 2) ||
        (isOpX<BNEG>(n) && n->arity() != 1)) {
      ++m_stats.opaque;
      return std::nullopt;
    }
    unsigned w = widthOf(n);
    if (!w || n->arity() == 0)
      return std::nullopt;
    Form f;
    f.w = w;
    if (isOpX<BMUL>(n)) {
      mpz_class k = 1;
      ENode *other = nullptr;
      for (size_t i = 0; i < n->arity(); ++i) {
        Expr a(n->arg(i));
        if (op::bv::isBvNum(a))
          k = smallarith::modPow2(k * op::bv::toMpz(a), w);
        else if (other)
          return std::nullopt; // a product of two terms
        else
          other = n->arg(i);
      }
      if (!other) {
        f.c = k;
        return f;
      }
      const mpz_class minusOne = smallarith::modPow2(mpz_class(-1L), w);
      Form g = formOf(other, w);
      // k * (a + b) or k * (a + c): a general factor is not distributed
      if ((g.coef.size() > 1 || g.c != 0) && !g.coef.empty() && k != 1 &&
          k != minusOne)
        return std::nullopt;
      addScaled(f, g, k);
      return f;
    }
    const mpz_class minusOne = smallarith::modPow2(mpz_class(-1L), w);
    for (size_t i = 0; i < n->arity(); ++i) {
      bool neg = isOpX<BNEG>(n) || (isOpX<BSUB>(n) && i > 0);
      addScaled(f, formOf(n->arg(i), w), neg ? minusOne : mpz_class(1L));
    }
    return f;
  }

  /// the form of a kid in a linear context: its own (a linear node or a
  /// numeral), or one leaf
  Form formOf(ENode *k, unsigned w) {
    auto it = m_form.find(k);
    if (it != m_form.end() && !sharedLeaf(k))
      return it->second;
    Form f;
    f.w = w;
    Expr leaf = imageOf(k);
    f.coef.emplace(keyOf(leaf.get(), w), mpz_class(1L));
    return f;
  }

  /// acc += k * g (modulo 2^w)
  static void addScaled(Form &acc, const Form &g, const mpz_class &k) {
    const unsigned w = acc.w;
    acc.c = smallarith::modPow2(acc.c + k * g.c, w);
    for (const auto &[key, v] : g.coef) {
      auto [p, fresh] = acc.coef.emplace(key, mpz_class(0L));
      p->second = smallarith::modPow2(p->second + k * v, w);
      if (p->second == 0)
        acc.coef.erase(p);
    }
  }

  /// the key of a leaf term \p l of width \p w (rebuilt kids), and its
  /// spelling: the first leaf of that key at that width
  Key keyOf(ENode *l, unsigned w) {
    Key key = baseOf(l);
    auto [it, fresh] =
        m_spelling.emplace(std::make_tuple(key.base, key.eff, w), Expr(l));
    (void)it;
    if (fresh)
      m_pin.push_back(Expr(l));
    return key;
  }

  /// (base, eff): zext / concat 0 / low extract of a leaf name its low bits;
  /// every other term (a sum among them) is its own base at its width
  Key baseOf(ENode *l) {
    auto it = m_base.find(l);
    if (it != m_base.end())
      return it->second;
    Key key{l, 0};
    ENode *inner = nullptr;
    unsigned cap = 0;
    if (isOpX<BZEXT>(l) && l->arity() == 2) {
      inner = l->arg(0);
    } else if (isOpX<BCONCAT>(l) && l->arity() == 2 &&
               op::bv::isBvNum(Expr(l->arg(0))) &&
               op::bv::toMpz(Expr(l->arg(0))) == 0) {
      inner = l->arg(1);
    } else if (isOpX<BEXTRACT>(l) && l->arity() == 3 &&
               op::bv::low(Expr(l)) == 0) {
      inner = l->arg(2);
      cap = op::bv::high(Expr(l)) + 1;
    }
    if (inner && widthOf(inner)) {
      key = baseOf(inner);
      if (cap)
        key.eff = std::min(key.eff, cap);
    } else {
      key.eff = widthOf(l);
    }
    m_pin.push_back(Expr(l));
    m_base.emplace(l, key);
    return key;
  }

  /// the leaves of a form, split by sign and sorted by key
  void split(const Form &f, std::vector<Item> &pos, std::vector<Item> &neg) {
    mpz_class half;
    mpz_ui_pow_ui(half.get_mpz_t(), 2, f.w - 1);
    for (const auto &[key, v] : f.coef) {
      if (v > half)
        neg.push_back({key, smallarith::modPow2(mpz_class(0L) - v, f.w)});
      else
        pos.push_back({key, v});
    }
    // f.coef is ordered by key already
  }

  Expr leaf(const Item &it, unsigned w) {
    Expr t = m_spelling.at(std::make_tuple(it.key.base, it.key.eff, w));
    return it.k == 1 ? t : Expr(mk<BMUL>(num(it.k, w), t));
  }

  /// the n-ary bvadd of the leaves (one: the leaf); none for no leaf
  Expr chain(const std::vector<Item> &items, unsigned w) {
    if (items.empty())
      return Expr();
    std::vector<Expr> ts;
    for (const Item &it : items)
      ts.push_back(leaf(it, w));
    m_stats.leaves += ts.size();
    return ts.size() == 1 ? ts[0] : mknary<BADD>(ts.begin(), ts.end());
  }

  Expr emit(const Form &f) {
    std::vector<Item> pos, neg;
    split(f, pos, neg);
    Expr acc = chain(pos, f.w);
    mpz_class c = f.c;
    if (!neg.empty() && m_neg == NegForm::Sub) {
      // P - N + c = (P + c) - N; -N + c = c - N
      Expr l = acc;
      if (!l)
        l = num(c, f.w);
      else if (c != 0)
        l = mk<BADD>(l, num(c, f.w));
      return mk<BSUB>(l, chain(neg, f.w));
    }
    if (!neg.empty()) {
      Expr n = mk<BNOT>(chain(neg, f.w));
      acc = acc ? Expr(mk<BADD>(acc, n)) : n;
      c = smallarith::modPow2(c + 1, f.w);
    }
    if (!acc)
      return num(c, f.w);
    if (c != 0)
      acc = mk<BADD>(acc, num(c, f.w));
    return acc;
  }

  /// (= a b) with a linear side, balanced
  Expr balance(ENode *n) {
    unsigned w = m_form.count(n->arg(0)) ? m_form.at(n->arg(0)).w
                                         : m_form.at(n->arg(1)).w;
    Form d = formOf(n->arg(0), w);
    addScaled(d, formOf(n->arg(1), w), smallarith::modPow2(mpz_class(-1L), w));
    Form e;
    e.w = w;
    addScaled(e, d, smallarith::modPow2(mpz_class(-1L), w));
    // the orientation: fewer negative leaves, then the smaller key sequence,
    // then the smaller constant (when every coefficient is 2^(w-1), D and
    // -D have the same leaves and differ in the constant only)
    std::vector<Item> dp, dn, ep, en;
    split(d, dp, dn);
    split(e, ep, en);
    auto rank = [](const std::vector<Item> &p, const std::vector<Item> &q) {
      std::vector<std::tuple<unsigned, unsigned, mpz_class>> r;
      for (const auto *v : {&q, &p})
        for (const Item &it : *v)
          r.emplace_back(it.key.base->getId(), it.key.eff, it.k);
      return std::make_pair(q.size(), r);
    };
    const auto re = rank(ep, en), rd = rank(dp, dn);
    const bool flip = re < rd || (re == rd && smallarith::modPow2(e.c, w) <
                                                  smallarith::modPow2(d.c, w));
    const Form &f = flip ? e : d;
    std::vector<Item> &pos = flip ? ep : dp, &neg = flip ? en : dn;
    ++m_stats.eqs;
    Expr res;
    if (pos.empty() && neg.empty()) {
      res = f.c == 0 ? Expr(mk<TRUE>(efac())) : Expr(mk<FALSE>(efac()));
    } else if (neg.empty()) {
      res = mk<EQ>(chain(pos, w), num(mpz_class(0L) - f.c, w));
    } else {
      // pos is not empty: the orientation with fewer negative leaves
      // has some positive one unless both sides are empty
      Expr l = chain(pos, w);
      if (!l)
        l = num(0, w);
      if (f.c != 0)
        l = mk<BADD>(l, num(f.c, w));
      res = mk<EQ>(l, chain(neg, w));
    }
    if (res.get() != n)
      ++m_stats.eqsChanged;
    return res;
  }

  /// a non-linear node over the images of its kids
  void rebuild(ENode *n) {
    ExprVector kids;
    bool changed = false;
    for (size_t i = 0; i < n->arity(); ++i) {
      Expr k = imageOf(n->arg(i));
      changed |= k.get() != n->arg(i);
      kids.push_back(k);
    }
    m_new.emplace(n, changed ? n->efac().mkNary(n->op(), kids) : Expr(n));
  }

  /// the image of a visited node: a linear node is emitted on first use
  Expr imageOf(ENode *n) {
    auto it = m_new.find(n);
    if (it != m_new.end())
      return it->second;
    if (!m_linear.count(n))
      return Expr();
    const Form &f = m_form.at(n);
    Expr r = emit(f);
    if (f.proper()) {
      ++m_stats.sums;
      if (r.get() != n)
        ++m_stats.sumsChanged;
    }
    m_new.emplace(n, r);
    return r;
  }

  NegForm m_neg;
  EqForm m_eq;
  InnerForm m_inner;
  std::unordered_set<ENode *> m_sharedSeen;
  std::unordered_map<ENode *, unsigned> m_uses;
  ExprFactory *m_efac = nullptr;
  TypeChecker m_tc;
  std::unordered_map<ENode *, Form> m_form;
  std::unordered_map<ENode *, Expr> m_new;
  std::unordered_map<ENode *, Key> m_base;
  std::map<std::tuple<ENode *, unsigned, unsigned>, Expr> m_spelling;
  std::unordered_set<ENode *> m_linear;
  std::unordered_set<ENode *> m_visited;
  std::vector<Expr> m_pin;
  Stats m_stats;
};

} // namespace expr
