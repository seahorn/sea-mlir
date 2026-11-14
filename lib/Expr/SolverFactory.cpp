// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SolverFactory.cpp - one place that makes solvers -------------------===//

#include "sea/Expr/Smt/SolverFactory.h"

#include "sea/Expr/Smt/EZ3.hh"
#include "sea/Expr/Smt/Z3SolverImpl.hh"
#include "sea/Support/Diag.h"
#ifdef WITH_YICES2
#include "sea/Expr/Smt/Yices2SolverImpl.hh"
#endif
#ifdef WITH_BITWUZLA
#include "sea/Expr/Smt/BitwuzlaSolverImpl.hh"
#endif

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <optional>
#include <z3.h>

namespace sea::solver {

const char *backendName(Backend b) {
  switch (b) {
  case Backend::Z3:
    return "z3";
  case Backend::Yices:
    return "yices";
  case Backend::Bitwuzla:
    return "bitwuzla";
  }
  return "?";
}

bool available(Backend b) {
  switch (b) {
  case Backend::Z3:
    return true;
  case Backend::Yices:
#ifdef WITH_YICES2
    return true;
#else
    return false;
#endif
  case Backend::Bitwuzla:
#ifdef WITH_BITWUZLA
    return true;
#else
    return false;
#endif
  }
  return false;
}

namespace {

/// A nonzero limit the backend has no form for: visible, then ignored
void unsupported(const SolverConfig &config, const char *backend,
                 const char *limit) {
  sea::diag::warn(config.kindPrefix + "limit-unsupported",
                  llvm::Twine(backend) + " has no " + limit +
                      " limit: its checks run without it");
}

/// z3: the nonzero limits (and the seam-check setup) as one parameter set;
/// nothing is set when there is nothing to set
void z3Limits(z3_solver_impl &z, const Limits &limits,
              const SolverConfig &config, bool relevancy0) {
  if (limits.memMB)
    unsupported(config, "z3", "per-solver memory");
  if (!limits.timeoutMs && !limits.rlimit && !relevancy0)
    return;
  sea::ZParams<sea::EZ3> params(z.get_context());
  if (limits.rlimit)
    params.set("rlimit", limits.rlimit);
  if (limits.timeoutMs)
    params.set("timeout", limits.timeoutMs);
  if (relevancy0)
    params.set("relevancy", 0u);
  z.get_solver().set(params);
}

/// A global z3 parameter set for the life of the guard, then restored
class GlobalParam {
  const char *m_name;
  std::string m_old;
  bool m_had = false;

public:
  GlobalParam(const char *name, const char *value) : m_name(name) {
    Z3_string old = nullptr;
    m_had = Z3_global_param_get(name, &old) && old;
    if (m_had)
      m_old = old;
    Z3_global_param_set(name, value);
  }
  ~GlobalParam() { Z3_global_param_set(m_name, m_had ? m_old.c_str() : ""); }
};

#ifdef WITH_YICES2
void yicesLimits(yices_solver_impl &y, const Limits &limits,
                 const SolverConfig &config) {
  if (limits.rlimit)
    unsupported(config, "Yices", "resource (rlimit)");
  if (limits.memMB)
    unsupported(config, "Yices", "per-solver memory");
  y.set_timeout(limits.timeoutMs);
}
#endif

#if !defined(WITH_YICES2) || !defined(WITH_BITWUZLA)
void notBuilt(const SolverConfig &config, Backend b) {
  sea::diag::error(
      config.kindPrefix + "solver-unavailable",
      b == Backend::Yices
          ? "built without Yices (configure with -DSEA_ENABLE_YICES=ON "
            "-DYICES2_ROOT=<install>)"
          : "built without Bitwuzla (configure with "
            "-DBITWUZLA_ROOT=<install>)");
}
#endif

} // namespace

std::unique_ptr<Solver> makeSolver(expr::ExprFactory &efac, Backend backend,
                                   const Limits &limits,
                                   const SolverConfig &config) {
  switch (backend) {
  case Backend::Z3: {
    auto z = std::make_unique<z3_solver_impl>(efac);
    z3Limits(*z, limits, config, config.z3Relevancy0Push);
    // the global parameters z3 reads when it builds the solver: for this
    // solver only
    if (!config.z3Tactic.empty() || config.z3SatEuf) {
      std::optional<GlobalParam> tactic, euf;
      if (!config.z3Tactic.empty())
        tactic.emplace("tactic.default_tactic", config.z3Tactic.c_str());
      if (config.z3SatEuf)
        euf.emplace("sat.euf", "true");
      std::string err = z->get_solver().init();
      if (!err.empty()) {
        sea::diag::error(config.kindPrefix + "z3-tactic",
                         "z3 rejects --z3-tactic '" + config.z3Tactic +
                             "': " + err);
        return nullptr;
      }
    }
    // after the parameters, before anything is asserted
    if (config.z3Relevancy0Push)
      z->push();
    return z;
  }
  case Backend::Bitwuzla: {
#ifdef WITH_BITWUZLA
    if (limits.rlimit)
      unsupported(config, "Bitwuzla", "resource (rlimit)");
    bitwuzla_solver_impl::solver_options o;
    if (limits.timeoutMs)
      o["time-limit-per"] = std::to_string(limits.timeoutMs);
    if (limits.memMB)
      o["memory-limit"] = std::to_string(limits.memMB);
    return std::make_unique<bitwuzla_solver_impl>(efac, nullptr, o);
#else
    notBuilt(config, backend);
    return nullptr;
#endif
  }
  case Backend::Yices: {
#ifdef WITH_YICES2
    yices_solver_impl::solver_options yo;
    for (const std::string &kv : config.yicesConfig) {
      auto [k, v] = llvm::StringRef(kv).split('=');
      yo[k.str()] = v.str();
    }
    auto y = std::make_unique<yices_solver_impl>(
        efac, config.yicesLogic.empty() ? nullptr : config.yicesLogic.c_str(),
        yo);
    if (!y->config_error().empty()) {
      sea::diag::error(config.kindPrefix + "yices-config",
                       "Yices rejects --yices-logic / --yices-config " +
                           y->config_error());
      return nullptr;
    }
    for (const std::string &kv : config.yicesParams) {
      auto [k, v] = llvm::StringRef(kv).split('=');
      std::string err;
      if (!y->set_param(k.str(), v.str(), err)) {
        sea::diag::error(config.kindPrefix + "yices-param",
                         "Yices rejects --yices-param " + kv + ": " + err);
        return nullptr;
      }
    }
    yicesLimits(*y, limits, config);
    return y;
#else
    notBuilt(config, backend);
    return nullptr;
#endif
  }
  }
  return nullptr;
}

void setLimits(Solver &s, const Limits &limits, const SolverConfig &config) {
  switch (s.get_kind()) {
  case SolverKind::Z3:
    z3Limits(static_cast<z3_solver_impl &>(s), limits, config,
             /*relevancy0=*/false);
    return;
  case SolverKind::YICES2:
#ifdef WITH_YICES2
    yicesLimits(static_cast<yices_solver_impl &>(s), limits, config);
#endif
    return;
  case SolverKind::BITWUZLA:
    if (limits.timeoutMs || limits.rlimit || limits.memMB)
      sea::diag::warn(config.kindPrefix + "limit-unsupported",
                      "Bitwuzla takes no limit after construction: its "
                      "checks run without it");
    return;
  }
}

} // namespace sea::solver
