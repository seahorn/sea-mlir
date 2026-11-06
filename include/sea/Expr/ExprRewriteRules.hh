// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Minimal rewrite rules to drive the rewriter: read
// over store, read over ite, equality of bit-vector numerals, ite
// simplification. The real rule set (pointer normalization, object-aware
// compare, ...) is described in docs/design/SeaM.md.
//
// Ported in shape from SeaHorn seam-dev14 (lib/seahorn/Smt/
// ExprRewriterDefs.cc: ITERewriteRule, CompareRewriteRule,
// ReadOverWriteRule, ITECompRewriteConfig); each rule counts its
// applications by name in RewriteStats (the branch used seahorn::Stats).
// Thesis figures: ITE rules Fig. 2.1, comparison Fig. 2.2, read-over-write
// Fig. 2.6 (T_A) and Fig. 4.6 (T_M).
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprRewriter.hh"

namespace expr {

namespace rules {

/// \brief Terms without operands to rewrite: numerals and declarations (BIND),
/// constants (a FAPP of a declaration only), and nodes without kids.
inline bool isLeaf(const Expr &e) {
  return e->arity() == 0 || isOpX<BIND>(e) ||
         (isOpX<FAPP>(e) && e->arity() == 1);
}

/// \brief Fig. 2.1: ite(c, x, y) with c true / false, or x = y.
struct ITERewriteRule {
  RewriteStats &m_stats;
  explicit ITERewriteRule(RewriteStats &stats) : m_stats(stats) {}

  RewriteResult operator()(const Expr &exp) const {
    if (!isOpX<ITE>(exp))
      return {exp, RWStatus::RW_SKIP};
    Expr c = exp->arg(0);
    Expr t = exp->arg(1);
    Expr e = exp->arg(2);
    // ITE-TRUE / ITE-FALSE: |- c, |- !c
    if (isOpX<TRUE>(c)) {
      m_stats.count("ite.true");
      return {t, RWStatus::RW_DONE};
    }
    if (isOpX<FALSE>(c)) {
      m_stats.count("ite.false");
      return {e, RWStatus::RW_DONE};
    }
    // ite(c, a, a) ~> a (|- x = y)
    if (t == e) {
      m_stats.count("ite.same");
      return {t, RWStatus::RW_DONE};
    }
    return {exp, RWStatus::RW_DONE};
  }
};

/// \brief Fig. 2.2: a = b over two bit-vector numerals is true / false.
struct CompareRewriteRule {
  RewriteStats &m_stats;
  explicit CompareRewriteRule(RewriteStats &stats) : m_stats(stats) {}

  RewriteResult operator()(const Expr &exp) const {
    if (!isOpX<EQ>(exp))
      return {exp, RWStatus::RW_SKIP};
    Expr lhs = exp->left();
    Expr rhs = exp->right();
    // The numerals are compared as the solvers read them: modulo 2^width
    // (bv::bvnum does not reduce, and a negative numeral means 2^w - n).
    // Numerals of different widths are not a well-sorted compare: left alone.
    if (op::bv::is_bvnum(lhs) && op::bv::is_bvnum(rhs) &&
        op::bv::widthBvNum(lhs) == op::bv::widthBvNum(rhs)) {
      m_stats.count("eq.bvnum");
      ExprFactory &efac = exp->efac();
      mpz_class mod;
      mpz_ui_pow_ui(mod.get_mpz_t(), 2, op::bv::widthBvNum(lhs));
      // floor remainder: in [0, mod) whatever the sign
      mpz_class l, r;
      mpz_fdiv_r(l.get_mpz_t(), op::bv::toMpz(lhs).get_mpz_t(),
                 mod.get_mpz_t());
      mpz_fdiv_r(r.get_mpz_t(), op::bv::toMpz(rhs).get_mpz_t(),
                 mod.get_mpz_t());
      return {l == r ? mk<TRUE>(efac) : mk<FALSE>(efac), RWStatus::RW_DONE};
    }
    return {exp, RWStatus::RW_DONE};
  }
};

/// \brief Fig. 4.6 ROW and Fig. 2.1 ITE-PUSH for reads:
///   select(store(a, i, v), j) ~> ite(i = j, v, select(a, j))
///   select(ite(c, x, y), j)   ~> ite(c, select(x, j), select(y, j))
/// Both return RW_2: the new ite and the terms below it (the compare, the
/// inner select) are rewritten; the inner select starts the next chain.
struct ReadOverWriteRule {
  RewriteStats &m_stats;
  explicit ReadOverWriteRule(RewriteStats &stats) : m_stats(stats) {}

  RewriteResult operator()(const Expr &exp) const {
    if (!isOpX<SELECT>(exp))
      return {exp, RWStatus::RW_SKIP};
    const Expr arr = exp->arg(0);
    const Expr idx = exp->arg(1);
    if (isOpX<STORE>(arr)) {
      m_stats.count("row.store");
      // store(a, i, v): arg 0 = a, arg 1 = i, arg 2 = v
      Expr res = mk<ITE>(mk<EQ>(idx, arr->arg(1)), arr->arg(2),
                         op::array::select(arr->arg(0), idx));
      return {std::move(res), RWStatus::RW_2};
    }
    if (isOpX<ITE>(arr)) {
      m_stats.count("row.ite");
      Expr res = mk<ITE>(arr->arg(0), op::array::select(arr->arg(1), idx),
                         op::array::select(arr->arg(2), idx));
      return {std::move(res), RWStatus::RW_2};
    }
    return {exp, RWStatus::RW_DONE};
  }
};

} // end of namespace rules

/// \brief Config of the minimal rule set.
///
/// \p aggressiveCache selects the caching policy: true = the branch's
/// (utils::shouldCache: select / store / ite always, the rest when shared),
/// false = only shared terms (ExprRewriterConfigBase::shouldCache).
class MinimalRewriteConfig : public ExprRewriterConfigBase {
  rules::ITERewriteRule m_iteRule;
  rules::CompareRewriteRule m_compRule;
  rules::ReadOverWriteRule m_rowRule;
  bool m_aggressiveCache;

public:
  MinimalRewriteConfig(ExprFactory &efac, RewriteStats &stats,
                       bool aggressiveCache = true)
      : ExprRewriterConfigBase(efac, stats), m_iteRule(stats),
        m_compRule(stats), m_rowRule(stats),
        m_aggressiveCache(aggressiveCache) {}

  /// Every application is visited, so that a term under store values,
  /// addresses or arithmetic is normalized before a rule looks at it (the
  /// rule promises of RW_n assume the terms it builds out of kids are normal).
  /// Leaves (rules::isLeaf: numerals, constants) are not visited.
  bool shouldRewrite(const Expr &exp) const { return !rules::isLeaf(exp); }

  RewriteResult doRewrite(const Expr &exp) {
    if (isOpX<ITE>(exp))
      return m_iteRule(exp);
    if (isOpX<EQ>(exp))
      return m_compRule(exp);
    if (isOpX<SELECT>(exp))
      return m_rowRule(exp);
    // an operator without rules: kids are normal, the term is final
    return {exp, RWStatus::RW_DONE};
  }

  bool shouldCache(const Expr &e) const {
    return m_aggressiveCache ? utils::shouldCache(e)
                             : ExprRewriterConfigBase::shouldCache(e);
  }
};

} // end of namespace expr
