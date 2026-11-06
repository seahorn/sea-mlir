// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Bit-vector and Boolean normalization rules (docs/design/SeaM.md).
// They simplify what z3's `simplify` still removed from our --seam
// queries: byte reassembly (concat / extract), linear equalities, the order
// of the arguments of associative-commutative operators, Boolean ite.
//
// Every rule here is valid over plain bit-vectors and Booleans: no assumption
// (no A-INB, no object layout). The unit tests check each one with Yices
// (`in = out`) and that the result is a normal form.
//
// The rules are called by SeaMRewriteConfig (SeaMRules.hh) when
// SeaMOptions::simplify is on, after the SeaM rule of the same operator (eq,
// ite) or instead of it (and / or: these are a superset), so one rewriter and
// one cache serve the memory reads and the whole query. `sea-vc --simplify`
// runs the same config with the memory rules off.
//
// Term order. The arguments of bvadd, bvmul, bvand, bvor, bvxor, and, or are
// sorted by the creation id of the term (ENode::getId), registered object
// bases first in a bvadd (SeaM's view of a pointer needs them first), the
// folded numeral last. The id is a counter of the ExprFactory, bumped when a
// node is created: it does not depend on addresses, so the order is the same
// in every run as long as the terms are created in the same order (the VC
// generator is deterministic; the emitted query is compared across runs by the
// tests). Equal terms are one node (hash-consing), so sorting makes equal
// terms the same node. A node that is garbage-collected and made again gets
// a new id; no sorted term can contain a dead node, so this does not matter.
//
// RW_n. As in SeaMRules.hh: a rule returns RW_DONE when the term it returns is
// built of normal kids and is itself normal, and RW_n when the rules apply at
// the result and n levels below it; each rule says why n suffices.
#include "sea/Expr/BaseRegistry.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprRewriter.hh"
#include "sea/Expr/TypeChecker.hh"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace expr {
namespace bvnorm {

class Rules {
  ExprFactory &m_efac;
  const BaseRegistry &m_bases;
  RewriteStats &m_stats;
  /// widths of the non-leaf terms asked for (owning keys: a node stays alive,
  /// so its address stays its identity)
  std::unordered_map<Expr, unsigned, ExprPtrHash> m_widths;

public:
  Rules(ExprFactory &efac, const BaseRegistry &bases, RewriteStats &stats)
      : m_efac(efac), m_bases(bases), m_stats(stats) {}

  //===------------------------------------------------------------------===//
  // helpers
  //===------------------------------------------------------------------===//

  static mpz_class modPow2(const mpz_class &v, unsigned w) {
    mpz_class m, r;
    mpz_ui_pow_ui(m.get_mpz_t(), 2, w);
    mpz_fdiv_r(r.get_mpz_t(), v.get_mpz_t(), m.get_mpz_t());
    return r;
  }

  Expr num(const mpz_class &v, unsigned w) const {
    return op::bv::bvnum(modPow2(v, w), w, m_efac);
  }

  static mpz_class numVal(const Expr &e) {
    return modPow2(op::bv::toMpz(e), op::bv::widthBvNum(e));
  }

  /// 2^w - 1
  static mpz_class ones(unsigned w) { return modPow2(mpz_class(-1L), w); }

  /// The width of a bit-vector term; 0 if \p e is not one (or not known).
  /// Cheap for the operators the VC builds (the others go through the type
  /// checker); memoized for non-leaves.
  unsigned widthOf(const Expr &e) {
    unsigned w;
    if (op::bv::isBvNum(e, w))
      return w;
    // a constant: its declared sort (constant time, no inference); a
    // pointer constant (S2e): its address width (to_bits(p) and the
    // full-width extract over it are sized by it)
    if (isOpX<FAPP>(e) && e->arity() == 1 && isOpX<FDECL>(e->first())) {
      Expr s = op::bind::rangeTy(e->first());
      return isOpX<op::BVSORT>(s) || isOpX<op::PTRSORT>(s)
                 ? op::ptr::addrSortWidth(s)
                 : 0;
    }
    auto it = m_widths.find(e);
    if (it != m_widths.end())
      return it->second;
    w = computeWidth(e);
    m_widths.emplace(e, w);
    return w;
  }

  /// bvadd / bvmul / ... of the terms: a single term, or the numeral \p unit
  /// when there is none.
  template <typename Op>
  Expr nary(const llvm::SmallVectorImpl<Expr> &terms, const Expr &unit) const {
    if (terms.empty())
      return unit;
    if (terms.size() == 1)
      return terms.front();
    return mknary<Op>(terms.begin(), terms.end());
  }

  /// The canonical order of operands: by id, read through observation
  /// markers (S1.6c: ADDR / PTR_TO_INT / PTR_TO_BITS are erased at the
  /// boundary, so a term keeps the order it has without them), then by the
  /// term's own id (a strict total order).
  static bool idLess(const Expr &a, const Expr &b) {
    const auto sa = op::ptr::stripObservation(a)->getId(),
               sb = op::ptr::stripObservation(b)->getId();
    if (sa != sb)
      return sa < sb;
    return a->getId() < b->getId();
  }

  /// The order of the addends of a sum: registered bases first, then by id.
  /// SeaM's add uses it too.
  void sortSum(llvm::SmallVectorImpl<Expr> &terms) const {
    std::stable_sort(terms.begin(), terms.end(),
                     [&](const Expr &a, const Expr &b) {
                       // bases first (through an observation marker),
                       // then idLess
                       bool ba = m_bases.isBase(op::ptr::stripObservation(a)),
                            bb = m_bases.isBase(op::ptr::stripObservation(b));
                       return ba != bb ? ba : idLess(a, b);
                     });
  }

  static RewriteResult same(const Expr &e) { return {e, RWStatus::RW_DONE}; }

  //===------------------------------------------------------------------===//
  // extract / concat (byte reassembly)
  //===------------------------------------------------------------------===//

  /// zext / sext of a numeral (SMT-LIB zero_extend / sign_extend): the
  /// numeral at the result width (bv.zext.num, bv.sext.num). The value of
  /// n:bv(w) read unsigned for zext, signed for sext (n >= 2^(w-1) is
  /// n - 2^w), mod 2^k at the result width k. RW_DONE. Without it, an index
  /// read back from memory as a numeral stays sext(3) and b + sext(3) is not
  /// b + 3 (read-over-write cannot decide the hit in a dynamic-shape case).
  /// No assumption: bit-vector semantics. Only on a well-sorted input: a
  /// width-0 numeral (2^(w-1) would wrap) or a target narrower than the
  /// numeral (the type checker's rule) is left alone.
  RewriteResult ext(const Expr &exp) {
    const Expr x = exp->arg(0);
    unsigned wn;
    if (!op::bv::isBvNum(x, wn) || !isOpX<op::BVSORT>(exp->arg(1)))
      return same(exp);
    const unsigned k = op::bv::width(exp->arg(1));
    if (wn < 1 || k < wn)
      return same(exp);
    mpz_class v = numVal(x);
    if (isOpX<BSEXT>(exp)) {
      mpz_class half, full;
      mpz_ui_pow_ui(half.get_mpz_t(), 2, wn - 1);
      mpz_ui_pow_ui(full.get_mpz_t(), 2, wn);
      if (v >= half)
        v = v - full;
      m_stats.count("bv.sext.num");
    } else {
      m_stats.count("bv.zext.num");
    }
    return {num(v, k), RWStatus::RW_DONE};
  }

  /// extract(h, l, x). Kids are normal.
  ///  - extract of the full width: x (bv.extract.full). RW_DONE: a kid.
  ///  - extract of a numeral: the numeral (bv.extract.num). RW_DONE.
  ///  - extract of extract: one extract of the inner term
  ///    (bv.extract.extract). RW_1: the new extract at level 0 over y, a kid
  ///    of a normal term (it can be a concat: the rule applies at the result).
  ///  - extract of concat: the parts of the concat that overlap [l, h], the
  ///    outer ones cut (bv.extract.concat): one part is that part, several are
  ///    a concat of the parts. RW_1: the concat at level 0 and the cut parts
  ///    (extracts of normal kids of the input) at level 1, where their own
  ///    rules apply; a concat made of adjacent extracts of one term merges at
  ///    level 0 once its kids are done.
  /// No assumption: bit-vector semantics (SMT-LIB).
  RewriteResult extract(const Expr &exp) {
    const unsigned h = op::bv::high(exp), l = op::bv::low(exp);
    const Expr x = op::bv::earg(exp);
    unsigned wn;
    if (op::bv::isBvNum(x, wn)) {
      m_stats.count("bv.extract.num");
      return {num(numVal(x) >> l, h - l + 1), RWStatus::RW_DONE};
    }
    unsigned wx = widthOf(x);
    if (wx && l == 0 && h + 1 == wx) {
      m_stats.count("bv.extract.full");
      return {x, RWStatus::RW_DONE};
    }
    if (isOpX<BEXTRACT>(x)) {
      m_stats.count("bv.extract.extract");
      unsigned l2 = op::bv::low(x);
      return {op::bv::extract(h + l2, l + l2, op::bv::earg(x)), RWStatus::RW_1};
    }
    if (isOpX<BCONCAT>(x)) {
      // the kids of a concat, most significant first: lo_i = the bits below
      llvm::SmallVector<unsigned, 8> ws(x->arity());
      for (size_t i = 0; i < x->arity(); ++i) {
        ws[i] = widthOf(x->arg(i));
        if (!ws[i])
          return same(exp);
      }
      llvm::SmallVector<Expr, 8> parts;
      unsigned lo = 0;
      for (size_t i = x->arity(); i-- > 0;) {
        unsigned hi = lo + ws[i] - 1;
        if (hi >= l && lo <= h) {
          unsigned a = std::max(l, lo) - lo, b = std::min(h, hi) - lo;
          parts.insert(parts.begin(), (a == 0 && b + 1 == ws[i])
                                          ? x->arg(i)
                                          : op::bv::extract(b, a, x->arg(i)));
        }
        lo = hi + 1;
      }
      m_stats.count("bv.extract.concat");
      if (parts.size() == 1)
        return {parts[0],
                isOpX<BEXTRACT>(parts[0]) ? RWStatus::RW_1 : RWStatus::RW_DONE};
      Expr c = mknary<BCONCAT>(parts.begin(), parts.end());
      if (c == x) // cannot happen for a full-width extract, which went first
        return same(exp);
      return {c, RWStatus::RW_1};
    }
    return same(exp);
  }

  /// concat(c_1, ..., c_n), c_1 the most significant. Kids are normal.
  ///  - a nested concat is flattened (bv.concat.flatten);
  ///  - two adjacent numerals are one numeral (bv.concat.num);
  ///  - two adjacent extracts of one term, extract(h1, l1, x) and extract(h2,
  ///    l2, x) with l1 = h2 + 1, are extract(h1, l2, x) (bv.concat.merge);
  ///  - a concat of one part is that part.
  /// No assumption. RW_1 when something changed: the new concat at level 0,
  /// its new numerals / extracts at level 1 (kids of normal terms; a merged
  /// extract can be a full-width extract or an extract of a concat, which the
  /// extract rule resolves at that level).
  RewriteResult concat(const Expr &exp) {
    llvm::SmallVector<Expr, 8> flat;
    bool changed = false;
    for (size_t i = 0; i < exp->arity(); ++i) {
      const Expr &a = exp->arg(i);
      if (isOpX<BCONCAT>(a)) {
        changed = true;
        m_stats.count("bv.concat.flatten");
        for (size_t j = 0; j < a->arity(); ++j)
          flat.push_back(a->arg(j));
      } else {
        flat.push_back(a);
      }
    }
    llvm::SmallVector<Expr, 8> out;
    for (const Expr &a : flat) {
      if (!out.empty()) {
        const Expr p = out.back();
        unsigned wp, wa;
        if (op::bv::isBvNum(p, wp) && op::bv::isBvNum(a, wa)) {
          m_stats.count("bv.concat.num");
          out.back() = num((numVal(p) << wa) + numVal(a), wp + wa);
          changed = true;
          continue;
        }
        if (isOpX<BEXTRACT>(p) && isOpX<BEXTRACT>(a) &&
            op::bv::earg(p) == op::bv::earg(a) &&
            op::bv::low(p) == op::bv::high(a) + 1) {
          m_stats.count("bv.concat.merge");
          out.back() =
              op::bv::extract(op::bv::high(p), op::bv::low(a), op::bv::earg(p));
          changed = true;
          continue;
        }
      }
      out.push_back(a);
    }
    if (!changed)
      return same(exp);
    if (out.size() == 1)
      return {out[0], RWStatus::RW_1};
    return {mknary<BCONCAT>(out.begin(), out.end()), RWStatus::RW_1};
  }

  //===------------------------------------------------------------------===//
  // associative-commutative bit-vector operators
  //===------------------------------------------------------------------===//

  /// bvmul / bvand / bvor / bvxor (n-ary). Kids are normal, hence flat.
  /// One level of nested terms of the same operator is flattened, the
  /// numerals are folded into one (put last, dropped if it is the unit),
  /// the arguments are sorted by id (see the header), and per operator:
  ///   bvmul: a zero numeral gives 0;
  ///   bvand: a zero numeral gives 0, duplicates are dropped (x & x = x);
  ///   bvor:  an all-ones numeral gives all ones, duplicates are dropped;
  ///   bvxor: equal pairs cancel (x ^ x = 0).
  /// Counters bv.ac.mul / and / or / xor. No assumption. RW_DONE: the result
  /// is a term over normal kids with no redex of these rules.
  RewriteResult ac(const Expr &exp) {
    enum Kind { Mul, And, Or, Xor } kind;
    if (isOpX<BMUL>(exp))
      kind = Mul;
    else if (isOpX<BAND>(exp))
      kind = And;
    else if (isOpX<BOR>(exp))
      kind = Or;
    else
      kind = Xor;
    unsigned w = 0;
    mpz_class acc = 0;
    bool haveNum = false;
    llvm::SmallVector<Expr, 8> terms;
    auto take = [&](const Expr &t) {
      unsigned tw;
      if (op::bv::isBvNum(t, tw)) {
        w = tw;
        mpz_class v = numVal(t);
        if (!haveNum) {
          acc = v;
          haveNum = true;
        } else if (kind == Mul) {
          acc = modPow2(acc * v, w);
        } else if (kind == And) {
          acc = acc & v;
        } else if (kind == Or) {
          acc = acc | v;
        } else {
          acc = acc ^ v;
        }
      } else {
        terms.push_back(t);
      }
    };
    auto sameOp = [&](const Expr &a) {
      return kind == Mul   ? isOpX<BMUL>(a)
             : kind == And ? isOpX<BAND>(a)
             : kind == Or  ? isOpX<BOR>(a)
                           : isOpX<BXOR>(a);
    };
    for (size_t i = 0; i < exp->arity(); ++i) {
      const Expr &a = exp->arg(i);
      if (sameOp(a)) {
        for (size_t j = 0; j < a->arity(); ++j)
          take(a->arg(j));
      } else {
        take(a);
      }
    }
    if (!w)
      w = widthOf(exp);
    if (!w)
      return same(exp);
    const char *name = kind == Mul   ? "bv.ac.mul"
                       : kind == And ? "bv.ac.and"
                       : kind == Or  ? "bv.ac.or"
                                     : "bv.ac.xor";
    const mpz_class unit = kind == And   ? ones(w)
                           : kind == Mul ? mpz_class(1L)
                                         : mpz_class(0L);
    auto result = [&](const Expr &e) {
      m_stats.count(name);
      return RewriteResult{e, RWStatus::RW_DONE};
    };
    if (haveNum) {
      if ((kind == Mul || kind == And) && acc == 0)
        return result(num(0, w));
      if (kind == Or && acc == ones(w))
        return result(num(ones(w), w));
    }
    std::stable_sort(terms.begin(), terms.end(), idLess);
    if (kind == And || kind == Or) {
      terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    } else if (kind == Xor) {
      llvm::SmallVector<Expr, 8> odd;
      for (size_t i = 0; i < terms.size();) {
        size_t j = i;
        while (j < terms.size() && terms[j] == terms[i])
          ++j;
        if ((j - i) % 2)
          odd.push_back(terms[i]);
        i = j;
      }
      terms.swap(odd);
    }
    if (haveNum && acc != unit)
      terms.push_back(num(acc, w));
    Expr res;
    if (terms.empty())
      res = num(haveNum ? acc : unit, w);
    else if (terms.size() == 1)
      res = terms[0];
    else if (kind == Mul)
      res = mknary<BMUL>(terms.begin(), terms.end());
    else if (kind == And)
      res = mknary<BAND>(terms.begin(), terms.end());
    else if (kind == Or)
      res = mknary<BOR>(terms.begin(), terms.end());
    else
      res = mknary<BXOR>(terms.begin(), terms.end());
    if (res == exp)
      return same(exp);
    return result(res);
  }

  //===------------------------------------------------------------------===//
  // equality
  //===------------------------------------------------------------------===//

  /// `a = b`. Kids are normal. In order:
  ///  - a = a is true (bv.eq.refl); two Boolean literals compare directly;
  ///    `p = true` is p, `p = false` is !p (bv.eq.bool; RW_1: the `!` at
  ///    level 0 over a normal kid, whose own rule applies there);
  ///  - ite(c, n1, n2) = n3 over numerals is c, !c, true or false
  ///    (bv.eq.ite-const; RW_1 as above);
  ///  - linear equality (bv.eq.linear), when a side is a bvadd or a numeral:
  ///    each side is read as a sum of terms and one numeral (mod 2^w); the
  ///    terms common to both sides cancel (any term), the numerals are folded
  ///    into one, put on the left sum, and moved to the right when the left
  ///    has no term left: A + d = B, or A = d' (B empty) with the numeral on
  ///    the right. Addition is cancellative mod 2^w, so this is `a = b` for
  ///    any bit-vectors. Two empty sides are true / false. Of the two
  ///    orientations (A, B, d) and (B, A, -d) the one where the sorted ids of
  ///    A are lexicographically smaller is kept (canonical, deterministic).
  ///    RW_1: the new `=` at level 0, its sides (sums of normal terms, sorted
  ///    as the add rule sorts them) at level 1;
  ///  - otherwise the two sides are ordered by id (bv.eq.order), RW_1 for the
  ///    same reason.
  /// No assumption.
  RewriteResult eq(const Expr &exp) {
    const Expr a = exp->left(), b = exp->right();
    if (a == b) {
      m_stats.count("bv.eq.refl");
      return {mk<TRUE>(m_efac), RWStatus::RW_DONE};
    }
    auto lit = [](const Expr &e) { return isOpX<TRUE>(e) || isOpX<FALSE>(e); };
    if (lit(a) || lit(b)) {
      m_stats.count("bv.eq.bool");
      if (lit(a) && lit(b))
        return {mk<FALSE>(m_efac), RWStatus::RW_DONE};
      const Expr lt = lit(b) ? b : a, p = lit(b) ? a : b;
      if (isOpX<TRUE>(lt))
        return {p, RWStatus::RW_DONE};
      return {mk<NEG>(p), RWStatus::RW_1};
    }
    // ite(c, n1, n2) = n3
    for (int side = 0; side < 2; ++side) {
      const Expr &i = side ? b : a, &n = side ? a : b;
      unsigned wi;
      if (isOpX<ITE>(i) && op::bv::isBvNum(n, wi) &&
          op::bv::isBvNum(i->arg(1), wi) && op::bv::isBvNum(i->arg(2), wi)) {
        bool t = numVal(i->arg(1)) == numVal(n);
        bool e = numVal(i->arg(2)) == numVal(n);
        m_stats.count("bv.eq.ite-const");
        if (t && e)
          return {mk<TRUE>(m_efac), RWStatus::RW_DONE};
        if (!t && !e)
          return {mk<FALSE>(m_efac), RWStatus::RW_DONE};
        if (t)
          return {i->arg(0), RWStatus::RW_DONE};
        return {mk<NEG>(i->arg(0)), RWStatus::RW_1};
      }
    }
    unsigned wn;
    const bool linear = isOpX<BADD>(a) || isOpX<BADD>(b) ||
                        op::bv::isBvNum(a, wn) || op::bv::isBvNum(b, wn);
    if (!linear) {
      if (idLess(b, a)) {
        m_stats.count("bv.eq.order");
        return {mk<EQ>(b, a), RWStatus::RW_1};
      }
      return same(exp);
    }
    llvm::SmallVector<Expr, 4> A, B;
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
    collect(a, A, ca);
    collect(b, B, cb);
    if (!w)
      w = widthOf(a);
    if (!w)
      return same(exp);
    std::stable_sort(A.begin(), A.end(), idLess);
    std::stable_sort(B.begin(), B.end(), idLess);
    // multiset difference: the common terms cancel
    llvm::SmallVector<Expr, 4> A2, B2;
    for (size_t i = 0, j = 0; i < A.size() || j < B.size();) {
      if (i < A.size() && j < B.size() && A[i] == B[j]) {
        ++i;
        ++j;
      } else if (j >= B.size() || (i < A.size() && idLess(A[i], B[j]))) {
        A2.push_back(A[i++]);
      } else {
        B2.push_back(B[j++]);
      }
    }
    mpz_class d = modPow2(ca - cb, w);
    Expr l, r;
    if (A2.empty() && B2.empty()) {
      m_stats.count("bv.eq.linear");
      return {d == 0 ? Expr(mk<TRUE>(m_efac)) : Expr(mk<FALSE>(m_efac)),
              RWStatus::RW_DONE};
    }
    if (B2.empty()) {
      l = sum(A2, 0, w);
      r = num(mpz_class(0L) - d, w);
    } else if (A2.empty()) {
      l = sum(B2, 0, w);
      r = num(d, w);
    } else {
      bool swap = std::lexicographical_compare(B2.begin(), B2.end(), A2.begin(),
                                               A2.end(), idLess);
      if (swap) {
        std::swap(A2, B2);
        d = modPow2(mpz_class(0L) - d, w);
      }
      l = sum(A2, d, w);
      r = sum(B2, 0, w);
    }
    // a sum whose low bits are zero for every value is never a numeral that
    // has one of them set: 24 * x = 7 is false (bv.eq.low-zero)
    if (op::bv::isBvNum(r, wn) && numVal(r) != 0) {
      unsigned z = zeroLow(l, w, 3);
      if (z && modPow2(numVal(r), z) != 0) {
        m_stats.count("bv.eq.low-zero");
        return {mk<FALSE>(m_efac), RWStatus::RW_DONE};
      }
    }
    if (l == a && r == b)
      return same(exp);
    m_stats.count("bv.eq.linear");
    return {mk<EQ>(l, r), RWStatus::RW_1};
  }

  /// The sum of \p terms (any order) and the numeral \p d, in the order of
  /// the add rule: bases first, then by id, the numeral last, none if zero.
  Expr sum(llvm::SmallVector<Expr, 4> terms, const mpz_class &d, unsigned w) {
    sortSum(terms);
    if (d != 0)
      terms.push_back(num(d, w));
    return nary<BADD>(terms, num(0, w));
  }

  //===------------------------------------------------------------------===//
  // Boolean
  //===------------------------------------------------------------------===//

  /// and / or. Kids are normal. A unit (true for and, false for or) is
  /// dropped, a zero is the result, nested terms of the same operator are
  /// flattened, the arguments are sorted by id and duplicates dropped, and x
  /// together with !x gives the zero. A superset of SeaM's andOr, which it
  /// replaces under simplify. Counter bool.and / bool.or. No assumption.
  /// RW_DONE: kids are normal.
  RewriteResult andOr(const Expr &exp, bool isAnd) {
    auto unit = [&](const Expr &e) {
      return isAnd ? isOpX<TRUE>(e) : isOpX<FALSE>(e);
    };
    auto zero = [&](const Expr &e) {
      return isAnd ? isOpX<FALSE>(e) : isOpX<TRUE>(e);
    };
    auto lit = [&](bool z) {
      return (isAnd == z) ? Expr(mk<FALSE>(m_efac)) : Expr(mk<TRUE>(m_efac));
    };
    llvm::SmallVector<Expr, 8> kept;
    kept.reserve(exp->arity());
    // the starts of the runs kept is made of: a flattened kid's arguments
    // are one run (a normal kid is sorted), any other argument its own
    llvm::SmallVector<size_t, 8> runs;
    bool changed = false;
    auto take = [&](const Expr &t) {
      if (unit(t))
        changed = true;
      else
        kept.push_back(t);
    };
    for (size_t i = 0; i < exp->arity(); ++i) {
      const Expr &a = exp->arg(i);
      if (zero(a)) {
        m_stats.count(isAnd ? "bool.and" : "bool.or");
        return {a, RWStatus::RW_DONE};
      }
      runs.push_back(kept.size());
      if ((isAnd && isOpX<AND>(a)) || (!isAnd && isOpX<OR>(a))) {
        changed = true;
        for (size_t j = 0; j < a->arity(); ++j) {
          if (zero(a->arg(j)))
            return {a->arg(j), RWStatus::RW_DONE};
          take(a->arg(j));
        }
      } else {
        take(a);
      }
    }
    if (!std::is_sorted(kept.begin(), kept.end(), idLess)) {
      // few runs (a flattened kid and the new arguments): merge them, in
      // linear time per merge; else sort (the same order: idLess is total)
      bool runsSorted = runs.size() <= 8;
      for (size_t r = 0; runsSorted && r < runs.size(); ++r) {
        size_t b = runs[r], e = r + 1 < runs.size() ? runs[r + 1] : kept.size();
        runsSorted = std::is_sorted(kept.begin() + b, kept.begin() + e, idLess);
      }
      if (runsSorted) {
        for (size_t r = 1; r < runs.size(); ++r) {
          size_t mid = runs[r],
                 e = r + 1 < runs.size() ? runs[r + 1] : kept.size();
          std::inplace_merge(kept.begin(), kept.begin() + mid, kept.begin() + e,
                             idLess);
        }
      } else {
        std::stable_sort(kept.begin(), kept.end(), idLess);
      }
      changed = true;
    }
    size_t n = kept.size();
    kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
    changed = changed || kept.size() != n;
    if (kept.size() > 1) {
      // x together with !x: only the negations are looked up (the arguments
      // are sorted by id, so a search by id finds x)
      for (const Expr &e : kept)
        if (isOpX<NEG>(e) &&
            std::binary_search(kept.begin(), kept.end(), e->left(), idLess)) {
          m_stats.count(isAnd ? "bool.and" : "bool.or");
          return {lit(true), RWStatus::RW_DONE};
        }
    }
    if (!changed)
      return same(exp);
    m_stats.count(isAnd ? "bool.and" : "bool.or");
    if (kept.empty())
      return {lit(false), RWStatus::RW_DONE};
    if (kept.size() == 1)
      return {kept.front(), RWStatus::RW_DONE};
    return {isAnd ? mknary<AND>(kept.begin(), kept.end())
                  : mknary<OR>(kept.begin(), kept.end()),
            RWStatus::RW_DONE};
  }

  /// ite(c, t, e), after SeaM's ite rule found nothing. Kids are normal.
  ///  - ite(!c, t, e) ~> ite(c, e, t) (bool.ite-neg-cond);
  ///  - Boolean arms (a literal arm, or an arm equal to the condition, makes
  ///    both Boolean): ite(c, t, false) ~> c & t, ite(c, t, true) ~> !c | t,
  ///    ite(c, false, e) ~> !c & e, ite(c, true, e) ~> c | e, ite(c, c, e) ~>
  ///    c | e, ite(c, t, c) ~> c & t (bool.ite-and / bool.ite-or);
  ///  - the same condition in an arm: ite(c, t, ite(c, y, z)) ~> ite(c, t, z),
  ///    ite(c, ite(c, x, y), e) ~> ite(c, x, e) (bool.ite-same-cond);
  ///  - the same value on two paths: ite(c, a, ite(d, a, f)) ~> ite(c | d, a,
  ///    f), ite(c, ite(d, a, f), f) ~> ite(c & d, a, f) (bv.ite-merge; the
  ///    new `|` / `&` at level 1 sorts its arguments there).
  /// No assumption. RW_1: the new term at level 0 over normal kids; a new
  /// `!c` is at level 1 (its rule applies there; c is not a negation here).
  RewriteResult ite(const Expr &exp) {
    const Expr c = exp->arg(0), t = exp->arg(1), e = exp->arg(2);
    if (isOpX<NEG>(c)) {
      m_stats.count("bool.ite-neg-cond");
      return {mk<ITE>(c->left(), e, t), RWStatus::RW_1};
    }
    auto bool1 = [&](const Expr &r) {
      m_stats.count("bool.ite-and-or");
      return RewriteResult{r, RWStatus::RW_1};
    };
    if (isOpX<FALSE>(e) || e == c)
      return bool1(mk<AND>(c, t));
    if (isOpX<TRUE>(e))
      return bool1(mk<OR>(mk<NEG>(c), t));
    if (isOpX<FALSE>(t))
      return bool1(mk<AND>(mk<NEG>(c), e));
    if (isOpX<TRUE>(t) || t == c)
      return bool1(mk<OR>(c, e));
    // ite(c, a, ite(d, a, f)) ~> ite(c | d, a, f); ite(c, ite(d, a, f), f)
    // ~> ite(c & d, a, f): the same value on two paths (z3's ite-extra rule)
    if (isOpX<ITE>(e) && e->arg(1) == t && e->arg(0) != c) {
      m_stats.count("bv.ite-merge");
      return {mk<ITE>(mk<OR>(c, e->arg(0)), t, e->arg(2)), RWStatus::RW_1};
    }
    if (isOpX<ITE>(t) && t->arg(2) == e && t->arg(0) != c) {
      m_stats.count("bv.ite-merge");
      return {mk<ITE>(mk<AND>(c, t->arg(0)), t->arg(1), e), RWStatus::RW_1};
    }
    if (isOpX<ITE>(e) && e->arg(0) == c) {
      m_stats.count("bool.ite-same-cond");
      return {mk<ITE>(c, t, e->arg(2)), RWStatus::RW_1};
    }
    if (isOpX<ITE>(t) && t->arg(0) == c) {
      m_stats.count("bool.ite-same-cond");
      return {mk<ITE>(c, t->arg(1), e), RWStatus::RW_1};
    }
    return same(exp);
  }

  /// a -> b: true -> b is b, a -> true and false -> b are true, a -> a is
  /// true, a -> false is !a (bool.impl). No assumption. RW_DONE for a kid or
  /// a literal; RW_1 for the new `!a` (level 0 over a normal kid).
  RewriteResult impl(const Expr &exp) {
    const Expr a = exp->left(), b = exp->right();
    if (isOpX<TRUE>(a)) {
      m_stats.count("bool.impl");
      return {b, RWStatus::RW_DONE};
    }
    if (isOpX<TRUE>(b) || isOpX<FALSE>(a) || a == b) {
      m_stats.count("bool.impl");
      return {mk<TRUE>(m_efac), RWStatus::RW_DONE};
    }
    if (isOpX<FALSE>(b)) {
      m_stats.count("bool.impl");
      return {mk<NEG>(a), RWStatus::RW_1};
    }
    return same(exp);
  }

private:
  /// A lower bound of the number of low bits of \p e (a bit-vector of width
  /// \p w) that are zero whatever the values of its variables, capped at w;
  /// looks \p depth levels down (ite makes it exponential).
  unsigned zeroLow(const Expr &e, unsigned w, unsigned depth) {
    unsigned nw;
    if (op::bv::isBvNum(e, nw)) {
      mpz_class v = numVal(e);
      return v == 0 ? w : std::min<unsigned>(mpz_scan1(v.get_mpz_t(), 0), w);
    }
    if (!depth)
      return 0;
    if (isOpX<BMUL>(e)) {
      unsigned z = 0;
      for (size_t i = 0; i < e->arity() && z < w; ++i)
        z += zeroLow(e->arg(i), w, depth - 1);
      return std::min(z, w);
    }
    if (isOpX<BADD>(e) || isOpX<BOR>(e) || isOpX<BXOR>(e)) {
      unsigned z = w;
      for (size_t i = 0; i < e->arity() && z; ++i)
        z = std::min(z, zeroLow(e->arg(i), w, depth - 1));
      return z;
    }
    if (isOpX<BAND>(e)) {
      unsigned z = 0;
      for (size_t i = 0; i < e->arity(); ++i)
        z = std::max(z, zeroLow(e->arg(i), w, depth - 1));
      return z;
    }
    if (isOpX<ITE>(e))
      return std::min(zeroLow(e->arg(1), w, depth - 1),
                      zeroLow(e->arg(2), w, depth - 1));
    if (isOpX<BSHL>(e) && op::bv::isBvNum(e->arg(1), nw)) {
      mpz_class k = numVal(e->arg(1));
      if (k >= mpz_class(static_cast<unsigned long>(w)))
        return w;
      return std::min<unsigned>(w,
                                k.get_ui() + zeroLow(e->arg(0), w, depth - 1));
    }
    return 0;
  }

  /// width of the values of a memory term (an array of bit-vectors), 0 if
  /// not found without walking more than the head of the term
  unsigned arrayRange(Expr m) {
    for (;;) {
      if (isOpX<STORE>(m))
        return widthOf(m->arg(2));
      if (isOpX<ITE>(m))
        m = m->arg(1);
      else if (isOpX<STORE_MAP>(m) || isOpX<MEM_COPY>(m) || isOpX<MEM_SET>(m) ||
               isOpX<MEM_HAVOC>(m))
        m = m->arg(0);
      else
        break;
    }
    if (isOpX<FAPP>(m) && m->arity() == 1 && isOpX<FDECL>(m->first())) {
      Expr s = op::bind::rangeTy(m->first());
      if (isOpX<ARRAY_TY>(s) && isOpX<op::BVSORT>(op::sort::arrayValTy(s)))
        return op::bv::width(op::sort::arrayValTy(s));
    }
    return 0;
  }

  unsigned computeWidth(const Expr &e) {
    if (isOpX<BEXTRACT>(e))
      return op::bv::high(e) - op::bv::low(e) + 1;
    if (isOpX<BCONCAT>(e)) {
      unsigned s = 0;
      for (size_t i = 0; i < e->arity(); ++i) {
        unsigned w = widthOf(e->arg(i));
        if (!w)
          return 0;
        s += w;
      }
      return s;
    }
    if (op::ptr::isPtrIndex(e))
      return widthOf(op::ptr::base(e)); // a node: its base's (S2e)
    if (isOpX<BADD>(e) || isOpX<BSUB>(e) || isOpX<PTR_DIFF>(e) ||
        op::ptr::isMarker(e) || isOpX<BMUL>(e) || isOpX<BAND>(e) ||
        isOpX<BOR>(e) || isOpX<BXOR>(e) || isOpX<BNOT>(e) || isOpX<BNEG>(e) ||
        isOpX<BUDIV>(e) || isOpX<BSDIV>(e) || isOpX<BUREM>(e) ||
        isOpX<BSREM>(e) || isOpX<BSMOD>(e) || isOpX<BSHL>(e) ||
        isOpX<BLSHR>(e) || isOpX<BASHR>(e))
      return widthOf(e->arg(0));
    if (isOpX<ITE>(e))
      return widthOf(e->arg(1));
    if (isOpX<BZEXT>(e) || isOpX<BSEXT>(e))
      return isOpX<op::BVSORT>(e->arg(1)) ? op::bv::width(e->arg(1)) : 0;
    if (isOpX<SELECT>(e))
      return arrayRange(e->arg(0));
    TypeChecker tc;
    Expr t = tc.typeOf(e);
    return isOpX<op::BVSORT>(t) || isOpX<op::PTRSORT>(t)
               ? op::ptr::addrSortWidth(t)
               : 0;
  }
};

} // namespace bvnorm
} // end of namespace expr
