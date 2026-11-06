// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifdef WITH_BITWUZLA

#include "sea/Expr/Smt/BitwuzlaSolverImpl.hh"
#include "sea/Expr/ExprVisitor.hh"
#include "sea/Expr/Smt/BitwuzlaModelImpl.hh"
#include "sea/Expr/Smt/ConstArrayDefs.hh"
#include "sea/Expr/Smt/MarshalBitwuzla.hh"
#include "sea/Support/Diag.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <iterator>
#include <sstream>

using namespace expr;

namespace sea {
namespace solver {

void bitwuzla_solver_impl::new_instance() {
  bitwuzla::Options options;
  // -- models for get_model; unsat assumptions for unsat_core (as for Yices,
  // -- the core is a subset of the assumptions of check_with_assumptions)
  options.set(bitwuzla::Option::PRODUCE_MODELS, true);
  options.set(bitwuzla::Option::PRODUCE_UNSAT_ASSUMPTIONS, true);
  for (auto &kv : d_opts)
    options.set(kv.first, kv.second);
  d_bzla = std::make_unique<bitwuzla::Bitwuzla>(d_tm, options);
}

bitwuzla_solver_impl::bitwuzla_solver_impl(ExprFactory &efac, const char *logic,
                                           solver_options opts)
    : Solver(), d_efac(efac), d_opts(std::move(opts)) {
  if (logic != nullptr)
    diag::warn("bitwuzla.logic-ignored",
               std::string("logic '") + logic +
                   "' ignored: Bitwuzla picks its own procedure");
  try {
    new_instance();
  } catch (const std::exception &ex) {
    diag::error("bitwuzla.options", ex.what());
    llvm::report_fatal_error("cannot configure Bitwuzla");
  }
}

bitwuzla_solver_impl::~bitwuzla_solver_impl() = default;

Expr bitwuzla_solver_impl::inline_defs(Expr e) { return d_defs.apply(e); }

bool bitwuzla_solver_impl::add(Expr exp) {
  // -- the definitions the formula adds count only if it is asserted
  ConstArrayDefs defs = d_defs;
  try {
    exp = defs.add(exp);
    d_bzla->assert_formula(marshal_bitwuzla::encode_term(exp, d_tm, d_cache));
    d_defs = std::move(defs);
    expr::filter(
        exp,
        [](Expr e) {
          return bind::isFdecl(e) || (isOpX<FAPP>(e) && e->arity() == 1 &&
                                      bind::isFdecl(e->left()));
        },
        std::inserter(d_live, d_live.end()));
  } catch (const std::exception &ex) {
    diag::error("bitwuzla.encode",
                std::string("cannot assert the formula: ") + ex.what());
    return false;
  }
  return true;
}

SolverResult bitwuzla_solver_impl::convert(bitwuzla::Result res) {
  switch (res) {
  case bitwuzla::Result::SAT:
    return SolverResult::SAT;
  case bitwuzla::Result::UNSAT:
    return SolverResult::UNSAT;
  default:
    return SolverResult::UNKNOWN;
  }
}

SolverResult bitwuzla_solver_impl::validate(SolverResult res) {
  if (res != SolverResult::SAT || d_defs.joins.empty())
    return res;
  bool exact = d_defs.joinsExclusive([&](Expr g) {
    return d_bzla->get_value(marshal_bitwuzla::encode_term(g, d_tm, d_cache))
        .is_true();
  });
  if (exact)
    return res;
  diag::warn("bitwuzla.joins",
             "sat, but the model takes two edges into a join whose memory "
             "was inlined: the result is unknown");
  return SolverResult::UNKNOWN;
}

/** Check for satisfiability */
SolverResult bitwuzla_solver_impl::check() {
  d_last_assumptions.clear();
  try {
    return validate(convert(d_bzla->check_sat()));
  } catch (const std::exception &ex) {
    diag::error("bitwuzla.check", ex.what());
    return SolverResult::ERROR;
  }
}

SolverResult
bitwuzla_solver_impl::check_with_assumptions(const expr_const_it_range &lits) {
  d_last_assumptions.clear();
  try {
    std::vector<bitwuzla::Term> assumptions;
    for (auto lit : lits) {
      bitwuzla::Term t =
          marshal_bitwuzla::encode_term(inline_defs(lit), d_tm, d_cache);
      d_last_assumptions.insert({t, lit});
      assumptions.push_back(t);
    }
    return validate(convert(d_bzla->check_sat(assumptions)));
  } catch (const std::exception &ex) {
    diag::error("bitwuzla.check", ex.what());
    return SolverResult::ERROR;
  }
}

/** Return an unsatisfiable core */
void bitwuzla_solver_impl::unsat_core(ExprVector &out) {
  try {
    for (const bitwuzla::Term &t : d_bzla->get_unsat_assumptions()) {
      auto it = d_last_assumptions.find(t);
      if (it == d_last_assumptions.end())
        throw std::runtime_error("term in the unsat core is not an assumption");
      out.push_back(it->second);
    }
  } catch (const std::exception &ex) {
    // like the Yices backend: no core without an unsat check_with_assumptions
    diag::error("bitwuzla.unsat-core", ex.what());
    llvm::report_fatal_error("bitwuzla_solver_impl::unsat_core failed");
  }
}

/** Push a context */
void bitwuzla_solver_impl::push() {
  d_defScopes.push_back(d_defs);
  d_liveScopes.push_back(d_live);
  d_bzla->push(1);
}

/** Pop a context */
void bitwuzla_solver_impl::pop() {
  d_bzla->pop(1);
  if (!d_defScopes.empty()) {
    d_defs = std::move(d_defScopes.back());
    d_defScopes.pop_back();
  }
  if (!d_liveScopes.empty()) {
    d_live = std::move(d_liveScopes.back());
    d_liveScopes.pop_back();
  }
}

/** Clear all assertions */
void bitwuzla_solver_impl::reset() {
  d_last_assumptions.clear();
  d_defs = ConstArrayDefs();
  d_defScopes.clear();
  d_live.clear();
  d_liveScopes.clear();
  // -- the cache stays valid: its terms belong to the term manager
  new_instance();
}

/** Get a model */
bitwuzla_solver_impl::model_ref bitwuzla_solver_impl::get_model() {
  return model_ref(new bitwuzla_model_impl(*this, d_efac));
}

void bitwuzla_solver_impl::to_smt_lib(llvm::raw_ostream &o) {
  std::ostringstream ss;
  d_bzla->print_formula(ss, "smt2");
  // the query as Bitwuzla gets it: such definitions are substituted, not
  // asserted
  if (!d_defs.empty())
    o << "; " << d_defs.defs.size()
      << " array constant(s) defined through a const-array are inlined\n";
  o << ss.str();
}

} // namespace solver
} // namespace sea
#endif
