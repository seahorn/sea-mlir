// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifdef WITH_BITWUZLA

#include "sea/Expr/Smt/BitwuzlaModelImpl.hh"
#include "sea/Expr/Smt/MarshalBitwuzla.hh"
#include "sea/Support/Diag.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

namespace sea {
namespace solver {

bitwuzla_model_impl::bitwuzla_model_impl(bitwuzla_solver_impl &solver,
                                         expr::ExprFactory &efac)
    : d_solver(solver), d_efac(efac) {}

expr::Expr bitwuzla_model_impl::eval(expr::Expr exp, bool complete) {
  try {
    return marshal_bitwuzla::eval(d_solver.inline_defs(exp), d_efac,
                                  d_solver.term_mgr(), d_solver.get_cache(),
                                  complete, d_solver.bitwuzla());
  } catch (const std::exception &ex) {
    // the Model interface has no error return
    diag::error("bitwuzla.eval", ex.what());
    llvm::report_fatal_error("bitwuzla_model_impl::eval failed");
  }
}

void bitwuzla_model_impl::print(llvm::raw_ostream &o) const {
  // Bitwuzla has no model printer in its API: one define-fun per constant
  // of the asserted formulas, and per array constant the solver inlined.
  // Built before printing: a failure (no model) prints nothing.
  try {
    bitwuzla_solver_impl &s = d_solver;
    std::string out;
    llvm::raw_string_ostream os(out);
    auto define = [&](const std::string &name, const bitwuzla::Term &t) {
      bitwuzla::Term v = s.bitwuzla().get_value(t);
      os << "(define-fun " << name << " () " << v.sort().str() << " "
         << v.str(10) << ")\n";
    };
    for (auto &kv : s.array_defs())
      define(marshal_bitwuzla::name_of(kv.first),
             marshal_bitwuzla::encode_term(kv.second, s.term_mgr(),
                                           s.get_cache()));
    for (expr::Expr e : s.live_consts()) {
      auto it = s.get_cache().find(e);
      if (it == s.get_cache().end() || !it->second.is_const() ||
          !it->second.symbol() || it->second.sort().is_fun())
        continue;
      define(it->second.symbol()->get(), it->second);
    }
    o << os.str();
  } catch (const std::exception &ex) {
    diag::error("bitwuzla.model-print", ex.what());
  }
}

} // namespace solver
} // namespace sea
#endif
