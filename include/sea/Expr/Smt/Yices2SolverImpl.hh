// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifdef WITH_YICES2
#pragma once

#include "sea/Expr/Smt/ConstArrayDefs.hh"
#include "sea/Expr/Smt/Solver.hh"
#include "sea/Support/SeaLog.hh"
#include "yices.h"

#include <map>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace sea {
namespace solver {

namespace yices {
inline std::string error_string() {
  char *emsg = yices_error_string();
  std::string res(emsg);
  yices_free_string(emsg);
  return res;
}
} // namespace yices

/* the yices solver; actually a yices context. */
class yices_solver_impl : public Solver {
public:
  using model_ref = typename Solver::model_ref;

  using ycache_t = std::map<expr::Expr, term_t>;

  using solver_options = std::map<std::string, std::string>;

private:
  using assumptions_map_t = std::unordered_map<term_t, expr::Expr>;

  // ctx_config_t *d_cfg;

  /* the context */
  context_t *d_ctx;

  /// search parameters of every check (set_param); null: Yices' defaults
  param_t *d_params = nullptr;
  /// the first configuration key the constructor could not set (empty: all
  /// set); see config_error
  std::string d_config_error;

  expr::ExprFactory &d_efac;

  ycache_t d_cache;
  /// marshal_yices::encode_term's naryBv for every term of this context
  bool d_nary_bv = false;

  /* to build unsat cores: this avoids a decode_term function */
  assumptions_map_t d_last_assumptions;
  /// -- array constants defined by an equation whose right side bottoms out
  /// -- in a const-array (memory values named at joins): inlined, not
  /// -- asserted, so selects over them reach the const-array (see add)
  /// A push saves the state for pop to restore.
  ConstArrayDefs d_defs;
  std::vector<ConstArrayDefs> d_defScopes;
  /// a sat answer stands only if the inlined joins were exact (else unknown)
  SolverResult validate(SolverResult res);
  /// wall-clock limit of a check in ms (0 = none): a watchdog thread stops
  /// the search (yices_stop_search; Yices has no timeout parameter)
  unsigned d_timeout_ms = 0;
  bool d_timed_out = false;
  smt_status_t check_context(const term_t *lits, uint32_t n);

public:
  yices_solver_impl(expr::ExprFactory &efac, const char *logic = nullptr,
                    solver_options opts = solver_options());

  ~yices_solver_impl();

  SolverKind get_kind() const override { return SolverKind::YICES2; }

  bool add(expr::Expr exp) override;

  /** Check for satisfiability */
  SolverResult check() override;

  /** A search parameter of every later check (yices_set_param; e.g.
      random-seed). False and \p err set if Yices rejects it. */
  bool set_param(const std::string &key, const std::string &value,
                 std::string &err);

  /** The configuration (logic or a key of opts) the constructor could not
      set, with Yices' reason; empty if none. Such a context is made with
      the rest of the configuration: the caller must treat it as an error. */
  const std::string &config_error() const { return d_config_error; }

  /** A wall-clock limit for each check, in ms (0 = none) */
  void set_timeout(unsigned ms) { d_timeout_ms = ms; }

  /** "timeout" if the last check was stopped by the limit */
  std::string reason_unknown() override {
    return d_timed_out ? "timeout" : std::string();
  }

  /** Check with assumptions */
  SolverResult
  check_with_assumptions(const expr_const_it_range &assumptions) override;

  /** Return an unsatisfiable core */
  void unsat_core(expr::ExprVector &out) override;

  /** Push a context */
  void push() override;

  /** Pop a context */
  void pop() override;

  /** Get a model */
  model_ref get_model() override;

  /** Clear all assertions */
  void reset() override;

  /** Print asserted formulas to SMT-LIB format **/
  void to_smt_lib(llvm::raw_ostream &o,
                  Smt2Dialect dialect = Smt2Dialect::Native,
                  bool warn = true) override;

  ycache_t &get_cache(void);

  /// An n-ary bvadd as one yices_bvsum (sea-vc --solver-nary-sums; the same
  /// term as the default chain); set before the first term
  void set_nary_bv(bool on) { d_nary_bv = on; }
  bool nary_bv() const { return d_nary_bv; }

  /// \p e as the solver knows it: the inlined array definitions substituted
  /// and every select over a const-array skeleton expanded (Yices has no
  /// const-array; the marshaller refuses one). Every term asserted, assumed
  /// or evaluated in a model passes it.
  expr::Expr to_solver_term(expr::Expr e) const;
};
} // namespace solver
} // namespace sea
#endif
