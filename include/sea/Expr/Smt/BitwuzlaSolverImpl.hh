// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifdef WITH_BITWUZLA
#pragma once

#include "sea/Expr/Smt/ConstArrayDefs.hh"
#include "sea/Expr/Smt/MarshalBitwuzla.hh"
#include "sea/Expr/Smt/Solver.hh"

#include <bitwuzla/cpp/bitwuzla.h>

#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace sea {
namespace solver {

/* the Bitwuzla solver (C++ API: a TermManager and a Bitwuzla instance). */
class bitwuzla_solver_impl : public Solver {
public:
  using model_ref = typename Solver::model_ref;

  /// Bitwuzla option name -> value, as for `Options::set(lng, value)`
  using solver_options = std::map<std::string, std::string>;

private:
  using assumptions_map_t = std::unordered_map<bitwuzla::Term, expr::Expr>;

  expr::ExprFactory &d_efac;
  solver_options d_opts;

  // -- declaration order matters: terms (cache, assumptions) and the solver
  // -- must die before the term manager
  bitwuzla::TermManager d_tm;
  /// Bitwuzla has no reset: a new instance replaces the old one
  std::unique_ptr<bitwuzla::Bitwuzla> d_bzla;
  bzcache_t d_cache;
  /// to build unsat cores: this avoids a decode_term function
  assumptions_map_t d_last_assumptions;
  /// array constants defined by an equation whose right side bottoms out in a
  /// const-array (memory values named at joins): inlined, not asserted, since
  /// Bitwuzla 0.9 answers unknown on equalities over const-arrays. A push
  /// saves the state for pop to restore.
  ConstArrayDefs d_defs;
  std::vector<ConstArrayDefs> d_defScopes;
  /// the constants of the formulas asserted in the current context (what a
  /// model prints; the cache also holds those of reset and popped contexts);
  /// a push saves the set for pop to restore
  expr::ExprSet d_live;
  std::vector<expr::ExprSet> d_liveScopes;

  void new_instance();
  SolverResult convert(bitwuzla::Result res);
  /// a sat answer stands only if the inlined joins were exact (else unknown)
  SolverResult validate(SolverResult res);

public:
  bitwuzla_solver_impl(expr::ExprFactory &efac, const char *logic = nullptr,
                       solver_options opts = solver_options());

  ~bitwuzla_solver_impl();

  SolverKind get_kind() const override { return SolverKind::BITWUZLA; }

  /// false (after reporting through sea::diag) if the formula cannot be
  /// encoded: nothing is asserted then
  bool add(expr::Expr exp) override;

  /** Check for satisfiability */
  SolverResult check() override;

  /** Check with assumptions */
  SolverResult
  check_with_assumptions(const expr_const_it_range &assumptions) override;

  /** Return an unsatisfiable core (a subset of the last assumptions) */
  void unsat_core(expr::ExprVector &out) override;

  /** Push a context */
  void push() override;

  /** Pop a context */
  void pop() override;

  /** Get a model; valid until the next check / push / pop / reset */
  model_ref get_model() override;

  /** Clear all assertions */
  void reset() override;

  /** Print asserted formulas to SMT-LIB format **/
  void to_smt_lib(llvm::raw_ostream &o) override;

  /// \p e with the inlined array definitions substituted (what the solver
  /// knows an inlined constant as): apply before evaluating in the model
  expr::Expr inline_defs(expr::Expr e);

  const expr::ExprMap &array_defs() const { return d_defs.defs; }
  const expr::ExprSet &live_consts() const { return d_live; }

  bzcache_t &get_cache() { return d_cache; }
  bitwuzla::TermManager &term_mgr() { return d_tm; }
  bitwuzla::Bitwuzla &bitwuzla() { return *d_bzla; }
};
} // namespace solver
} // namespace sea
#endif
