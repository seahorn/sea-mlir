// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SolverFactory.h - one place that makes solvers -----------*- C++ -*-===//
//
// Every solver of the tools (sea-vc's verdict and auxiliary checks,
// sea-expr-eval) is made here: a backend, the limits of its checks and the
// backend's configuration. The caller decides the limits; the factory applies
// them as the backend can, and says when it cannot.
//
//   backend unavailable (not built) or configuration rejected (Yices'
//     logic, config or params; z3's tactic: <kindPrefix>z3-tactic)          ->
//     diag::error, null solver
//   a nonzero limit the backend has no form for -> diag::warn
//     (<kindPrefix>limit-unsupported), the solver runs without it
//
// Limits per backend (0 = none):
//   timeoutMs  z3 `timeout` (solver parameter), Bitwuzla `time-limit-per`,
//              Yices set_timeout (a watcher stops the search)
//   rlimit     z3 `rlimit` only
//   memMB      Bitwuzla `memory-limit` only. z3 and Yices have no limit per
//              solver: the tool bounds the process (sea-vc --mem-limit:
//              RLIMIT_AS, z3's global memory_max_size)
//
//===----------------------------------------------------------------------===//

#ifndef SEA_EXPR_SMT_SOLVERFACTORY_H
#define SEA_EXPR_SMT_SOLVERFACTORY_H

#include "sea/Expr/Smt/Solver.hh"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace expr {
class ExprFactory;
}

namespace sea::solver {

/// The backends a tool may select (whether this build has them: available)
enum class Backend { Z3, Yices, Bitwuzla };

/// "z3", "yices", "bitwuzla"
const char *backendName(Backend b);

/// Whether this build links the backend (z3 always; Yices with
/// SEA_ENABLE_YICES, Bitwuzla with BITWUZLA_ROOT)
bool available(Backend b);

/// The limits of every check of one solver (0 = none); see the file comment
/// for what each backend honours
struct Limits {
  unsigned timeoutMs = 0;
  unsigned rlimit = 0;
  uint64_t memMB = 0;
};

/// The configuration of one solver beyond its limits. Fields name their
/// backend; another backend does not read them.
struct SolverConfig {
  /// diag kinds: <prefix>solver-unavailable, <prefix>yices-config,
  /// <prefix>yices-param, <prefix>z3-tactic, <prefix>limit-unsupported (sea-vc:
  /// "vc.")
  std::string kindPrefix = "smt.";
  /// z3: relevancy 0 and one push before anything is asserted (sea-vc
  /// --seam-check's setup: the push makes z3 pick its incremental
  /// configuration; the solver is meant to be dropped, never popped)
  bool z3Relevancy0Push = false;
  /// z3: the default tactic of this solver only (sea-vc --z3-tactic; empty:
  /// z3's own). z3 reads it as a global parameter when it builds the solver:
  /// the factory sets it, makes z3 build the solver, and restores the old
  /// value, so no other solver sees it. Not applied by setLimits.
  std::string z3Tactic;
  /// z3: sat.euf=true for this solver only (the same way; SeaHorn sets it
  /// with the `sat` tactic)
  bool z3SatEuf = false;
  /// Yices: the logic of the context (empty: Yices' default)
  std::string yicesLogic;
  /// Yices: context configuration, `key=value` each
  std::vector<std::string> yicesConfig;
  /// Yices: search parameters of every check, `key=value` each
  std::vector<std::string> yicesParams;
};

/// A fresh solver of \p backend with \p limits and \p config; null after a
/// diag::error (backend not built, configuration rejected).
std::unique_ptr<Solver> makeSolver(expr::ExprFactory &efac, Backend backend,
                                   const Limits &limits = Limits(),
                                   const SolverConfig &config = SolverConfig());

/// Applies \p limits to an existing solver (of any backend; its kind says
/// which). z3: one parameter set with the nonzero limits, none if all are
/// zero; Yices: the timeout (the others warn); Bitwuzla: its options are
/// fixed at construction, so any nonzero limit warns.
void setLimits(Solver &s, const Limits &limits,
               const SolverConfig &config = SolverConfig());

} // namespace sea::solver

#endif // SEA_EXPR_SMT_SOLVERFACTORY_H
