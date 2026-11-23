// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "sea/Expr/Smt/EZ3.hh"
#include "sea/Expr/Smt/Solver.hh"
#include "sea/Expr/Smt/Z3ModelImpl.hh"
#include "llvm/Support/raw_ostream.h"

namespace sea {
namespace solver {

class z3_solver_impl : public Solver {
  expr::ExprFactory &m_efac;
  std::unique_ptr<EZ3> m_zctx;
  std::unique_ptr<ZSolver<EZ3>> m_solver;
  SolverResult m_last_result;
  /// the logic of the solver (empty: z3's default), for the printed query
  std::string m_logic;

public:
  using model_ref = typename Solver::model_ref;

  /// \p logic: z3's solver for that logic (Z3_mk_solver_for_logic); null:
  /// z3's default solver (Z3_mk_solver: the logic is probed at the first
  /// check)
  z3_solver_impl(expr::ExprFactory &efac, const char *logic = nullptr)
      : Solver(), m_efac(efac), m_zctx(new EZ3(m_efac)),
        m_solver(logic ? new ZSolver<EZ3>(*m_zctx, logic)
                       : new ZSolver<EZ3>(*m_zctx)),
        m_last_result(SolverResult::UNKNOWN), m_logic(logic ? logic : "") {}

  ~z3_solver_impl() = default;

  SolverKind get_kind() const override { return SolverKind::Z3; }

  EZ3 &get_context() { return *m_zctx; }

  ZSolver<EZ3> &get_solver() { return *m_solver; }

  virtual bool add(expr::Expr exp) override {
    m_solver->assertExpr(exp);
    return true;
  }

  /** Check for satisfiability */
  virtual SolverResult check() override {
    auto res = m_solver->solve();
    if (res) {
      m_last_result = SolverResult::SAT;
    } else if (!res) {
      m_last_result = SolverResult::UNSAT;
    } else {
      m_last_result = SolverResult::UNKNOWN;
    }
    return m_last_result;
  }

  virtual SolverResult
  check_with_assumptions(const expr_const_it_range &lits) override {
    auto res = m_solver->solveAssuming(lits);
    if (res) {
      m_last_result = SolverResult::SAT;
    } else if (!res) {
      m_last_result = SolverResult::UNSAT;
    } else {
      m_last_result = SolverResult::UNKNOWN;
    }
    return m_last_result;
  }

  virtual void unsat_core(expr::ExprVector &out) override {
    m_solver->unsatCore(std::back_inserter(out));
  }

  /** Push a context */
  virtual void push() override { m_solver->push(); }

  /** Pop a context */
  virtual void pop() override { m_solver->pop(); }

  /** Clear all assertions */
  virtual void reset() override { m_solver->reset(); }

  /** Get a model */
  virtual model_ref get_model() override {
    assert(m_last_result == SolverResult::SAT);
    ZModel<EZ3> model = m_solver->getModel();
    return model_ref(new z3_model_impl(model));
  }

  /** z3's reason, normalized: "timeout" (z3's timeout / canceled),
      "memout" (z3's memory_max_size), else z3's text */
  std::string reason_unknown() override {
    if (m_last_result != SolverResult::UNKNOWN)
      return std::string();
    std::string r = m_solver->reasonUnknown();
    if (r.find("timeout") != std::string::npos ||
        r.find("canceled") != std::string::npos)
      return "timeout";
    if (r.find("memory") != std::string::npos)
      return "memout";
    return r;
  }

  std::optional<uint64_t> rlimit_count() override {
    z3::stats st = m_solver->stats();
    for (unsigned i = 0, e = st.size(); i < e; ++i)
      if (st.key(i) == "rlimit count")
        return st.is_uint(i) ? uint64_t(st.uint_value(i))
                             : uint64_t(st.double_value(i));
    return std::nullopt;
  }

  std::optional<uint64_t> conflicts() override {
    z3::stats st = m_solver->stats();
    uint64_t n = 0;
    for (unsigned i = 0, e = st.size(); i < e; ++i)
      if (st.key(i) == "conflicts" || st.key(i) == "sat conflicts")
        n += st.is_uint(i) ? uint64_t(st.uint_value(i))
                           : uint64_t(st.double_value(i));
    return n;
  }

  std::optional<uint64_t> statistic(const std::string &key) override {
    z3::stats st = m_solver->stats();
    std::optional<uint64_t> n;
    for (unsigned i = 0, e = st.size(); i < e; ++i)
      if (st.key(i) == key)
        n = n.value_or(0) + (st.is_uint(i) ? uint64_t(st.uint_value(i))
                                           : uint64_t(st.double_value(i)));
    return n;
  }

  /// Write asserted formulas to SMT-LIB format: a header (the dialect, the
  /// constructs some solver does not read, the logic: smt2Header), then the
  /// assertions; \p dialect Portable
  /// expands what has an exact portable form (sea/Expr/Smt/Smt2Dialect.hh)
  virtual void to_smt_lib(llvm::raw_ostream &o,
                          Smt2Dialect dialect = Smt2Dialect::Native,
                          bool warn = true) override {
    expr::ExprVector asserts;
    m_solver->assertions(std::back_inserter(asserts));
    Smt2Scan scan = smt2Scan(asserts, dialect);
    smt2Header(o, dialect, scan, m_logic, warn);
    m_solver->toSmtLibOf(o, asserts, expr::ExprVector());
  }
};
} // namespace solver
} // namespace sea
