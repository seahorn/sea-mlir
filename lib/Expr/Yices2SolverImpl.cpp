// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifdef WITH_YICES2
#include "yices.h"
#include <gmp.h>

#include "sea/Expr/ExprVisitor.hh"
#include "sea/Expr/Smt/ConstArrayDefs.hh"
#include "sea/Expr/Smt/MarshalYices.hh"
#include "sea/Expr/Smt/Yices2ModelImpl.hh"
#include "sea/Expr/Smt/Yices2SolverImpl.hh"

// Yices 2.7 renamed the smt_status_t constants (STATUS_SAT ->
// YICES_STATUS_SAT, ...)
#if __YICES_VERSION > 2 || (__YICES_VERSION == 2 && __YICES_VERSION_MAJOR >= 7)
#define SEA_YICES_STATUS(x) YICES_STATUS_##x
#else
#define SEA_YICES_STATUS(x) STATUS_##x
#endif
#include "sea/Support/Diag.h"
#include "sea/Support/Log.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <vector>

using namespace expr;

namespace sea {
namespace solver {

static log::Tag logConstArray(
    "yices.const-array", 2,
    "an assertion in which a const-array survives the select rewrite "
    "(yices cannot assert it)");

static void report_fatal_error(std::string const &s) {
  llvm::report_fatal_error(llvm::StringRef(s));
}

/* flag to indicate library status; we are single threaded so we can be lazy. */
static bool s_yices_lib_initialized = false;

inline void yices_library_initialize(void) {
  if (!s_yices_lib_initialized) {
    s_yices_lib_initialized = true;
    yices_init();
  }
}

yices_solver_impl::yices_solver_impl(expr::ExprFactory &efac, const char *logic,
                                     solver_options opts)
    : Solver(), d_efac(efac) {

  yices_library_initialize();
  /* the yices configuration structure */
  ctx_config_t *cfg = yices_new_config();
  if (logic != nullptr) {
    int32_t res = yices_default_config_for_logic(cfg, logic);
    if (res && d_config_error.empty())
      d_config_error =
          std::string("logic ") + logic + ": " + yices::error_string();
  }
  /* iterate through the opts map and set the keys */
  for (const auto &[key, value] : opts)
    if (yices_set_config(cfg, key.c_str(), value.c_str()) &&
        d_config_error.empty())
      d_config_error = key + "=" + value + ": " + yices::error_string();

  d_ctx = yices_new_context(cfg);

  if (cfg != nullptr) {
    yices_free_config(cfg);
  }
}

yices_solver_impl::~yices_solver_impl() {
  if (d_params)
    yices_free_param_record(d_params);
  yices_free_context(d_ctx);
}

bool yices_solver_impl::set_param(const std::string &key,
                                  const std::string &value, std::string &err) {
  if (!d_params) {
    d_params = yices_new_param_record();
    yices_default_params_for_context(d_ctx, d_params);
  }
  if (yices_set_param(d_params, key.c_str(), value.c_str())) {
    err = yices::error_string();
    return false;
  }
  return true;
}

yices_solver_impl::ycache_t &yices_solver_impl::get_cache(void) {
  return d_cache;
}

namespace {
/// -- push select(arr, idx) through the store/ite skeleton of arr:
/// -- select-over-store becomes an index-equality ite, select-over-ite
/// -- distributes into the branches, and select-over-const-array yields the
/// -- default value. Leaves that are plain array terms keep a plain select.
Expr expandSelectOverConstArray(Expr arr, Expr idx, ExprMap &memo) {
  auto it = memo.find(arr);
  if (it != memo.end())
    return it->second;
  Expr res;
  if (isOpX<CONST_ARRAY>(arr))
    res = arr->right();
  else if (isOpX<STORE>(arr))
    res = boolop::lite(mk<EQ>(idx, arr->arg(1)), arr->arg(2),
                       expandSelectOverConstArray(arr->arg(0), idx, memo));
  else if (isOpX<ITE>(arr))
    res = boolop::lite(arr->arg(0),
                       expandSelectOverConstArray(arr->arg(1), idx, memo),
                       expandSelectOverConstArray(arr->arg(2), idx, memo));
  else
    res = op::array::select(arr, idx);
  memo[arr] = res;
  return res;
}

/// -- rewrite every select whose store/ite skeleton bottoms out in a
/// -- const-array into an ite cascade. Yices contexts cannot assert lambda
/// -- terms, which is the only encoding the marshaller has for const-arrays;
/// -- selects over havoc'd (non-const) memories are left untouched.
Expr rewriteConstArraySelects(Expr e, ExprMap &cache) {
  if (e->arity() == 0)
    return e;
  auto it = cache.find(e);
  if (it != cache.end())
    return it->second;

  ExprVector kids;
  kids.reserve(e->arity());
  bool changed = false;
  for (auto b = e->args_begin(), be = e->args_end(); b != be; ++b) {
    Expr kid = rewriteConstArraySelects(Expr(*b), cache);
    changed |= (kid.get() != *b);
    kids.push_back(kid);
  }
  Expr res = changed ? e->efac().mkNary(e->op(), kids.begin(), kids.end()) : e;
  if (isOpX<SELECT>(res) && hasConstArrayLeaf(res->left())) {
    ExprMap memo;
    res = expandSelectOverConstArray(res->left(), res->right(), memo);
  }
  cache[e] = res;
  return res;
}
} // namespace

bool yices_solver_impl::add(expr::Expr exp) {
  // -- array constants defined through a const-array cannot be asserted
  // -- (a lambda): inline their definitions, so the selects below see the
  // -- const-array
  exp = d_defs.add(exp);
  ExprMap rw_cache;
  exp = rewriteConstArraySelects(exp, rw_cache);
  // -- debug aid: the assertions in which a const-array survives the
  // -- rewrite (yices cannot assert them; see rewriteConstArraySelects)
  if (logConstArray.on(log::Object)) {
    std::vector<Expr> matches;
    expr::filter(
        exp, [](Expr e) { return isOpX<CONST_ARRAY>(e); },
        std::back_inserter(matches));
    if (!matches.empty())
      SEA_LOG(logConstArray, log::Object)
          .kv("const-arrays", matches.size())
          .kv("assertion", *exp);
  }
  term_t yt = marshal_yices::encode_term(exp, get_cache());
  if (yt == NULL_TERM) {
    std::string str;
    raw_string_ostream str_os(str);
    str_os << "yices_solver_impl::add:  failed to encode: " << *exp << "\n";
    report_fatal_error(str_os.str());
  }
  int32_t errcode = yices_assert_formula(d_ctx, yt);
  if (errcode == -1) {
    std::string str;
    raw_string_ostream str_os(str);
    str_os << "yices_solver_impl::add:  yices_assert_formula failed: "
           << yices::error_string() << "\n";
    report_fatal_error(str_os.str());
  }
  return true;
}

SolverResult yices_solver_impl::validate(SolverResult res) {
  if (res != SolverResult::SAT || d_defs.joins.empty())
    return res;
  model_ref model = get_model();
  bool exact = d_defs.joinsExclusive(
      [&](Expr g) { return isOpX<TRUE>(model->eval(g, true)); });
  if (exact)
    return res;
  diag::warn("yices.joins",
             "sat, but the model takes two edges into a join whose memory "
             "was inlined: the result is unknown");
  return SolverResult::UNKNOWN;
}

/** Check for satisfiability */
/// One check of the context (with assumptions if \p n), under the
/// wall-clock limit: a watchdog thread calls yices_stop_search when it
/// expires (the check then returns STATUS_INTERRUPTED).
smt_status_t yices_solver_impl::check_context(const term_t *lits, uint32_t n) {
  d_timed_out = false;
  auto run = [&] {
    return n ? yices_check_context_with_assumptions(d_ctx, d_params, n, lits)
             : yices_check_context(d_ctx, d_params);
  };
  if (!d_timeout_ms)
    return run();
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  std::thread watchdog([&] {
    std::unique_lock<std::mutex> lock(m);
    if (cv.wait_for(lock, std::chrono::milliseconds(d_timeout_ms),
                    [&] { return done; }))
      return;
    d_timed_out = true;
    // stop_search does nothing unless the context is searching: repeat
    // until the check returns
    do
      yices_stop_search(d_ctx);
    while (!cv.wait_for(lock, std::chrono::milliseconds(50),
                        [&] { return done; }));
  });
  smt_status_t stat = run();
  {
    std::lock_guard<std::mutex> lock(m);
    done = true;
  }
  cv.notify_one();
  watchdog.join();
  if (stat != SEA_YICES_STATUS(INTERRUPTED))
    d_timed_out = false;
  return stat;
}

SolverResult yices_solver_impl::check() {
  d_last_assumptions.clear();

  // could have a param_t field for this call.
  smt_status_t stat = check_context(nullptr, 0);
  switch (stat) {
  case SEA_YICES_STATUS(UNSAT):
    return SolverResult::UNSAT;
  case SEA_YICES_STATUS(SAT):
    return validate(SolverResult::SAT);
  case SEA_YICES_STATUS(UNKNOWN):
    return SolverResult::UNKNOWN;
  case SEA_YICES_STATUS(INTERRUPTED):
    return SolverResult::UNKNOWN;
  case SEA_YICES_STATUS(ERROR):
    return SolverResult::ERROR;
  default:
    return SolverResult::UNKNOWN;
  }
}

SolverResult
yices_solver_impl::check_with_assumptions(const expr_const_it_range &lits) {
  d_last_assumptions.clear();

  std::vector<term_t> y_lits;
  for (auto lit : lits) {
    ExprMap rw_cache;
    term_t y_lit = marshal_yices::encode_term(
        rewriteConstArraySelects(d_defs.apply(lit), rw_cache), get_cache());
    d_last_assumptions.insert({y_lit, lit});
    if (y_lit == NULL_TERM) {
      std::string str;
      raw_string_ostream str_os(str);
      str_os << "yices_solver_impl::check_with_assumptions:  failed to encode: "
             << *lit << "\n";
      report_fatal_error(str_os.str());
    }
    y_lits.push_back(y_lit);
  }

  smt_status_t stat = check_context(y_lits.data(), y_lits.size());
  switch (stat) {
  case SEA_YICES_STATUS(UNSAT):
    return SolverResult::UNSAT;
  case SEA_YICES_STATUS(SAT):
    return validate(SolverResult::SAT);
  case SEA_YICES_STATUS(UNKNOWN):
    return SolverResult::UNKNOWN;
  case SEA_YICES_STATUS(INTERRUPTED):
    return SolverResult::UNKNOWN;
  case SEA_YICES_STATUS(ERROR):
    return SolverResult::ERROR;
  default:
    return SolverResult::UNKNOWN;
  }
}

/** Return an unsatisfiable core */
void yices_solver_impl::unsat_core(ExprVector &out) {
  term_vector_t v;
  yices_init_term_vector(&v);
  int32_t errcode = yices_get_unsat_core(d_ctx, &v);
  if (errcode == -1) {
    std::string str;
    raw_string_ostream str_os(str);
    str_os << "yices_solver_impl::unsat_core: the solver is not unsat";
    report_fatal_error(str_os.str());
  }
  for (unsigned i = 0, e = v.size; i < e; ++i) {
    auto it = d_last_assumptions.find(v.data[i]);
    if (it == d_last_assumptions.end()) {
      std::string str;
      raw_string_ostream str_os(str);
      str_os << "yices_solver_impl::unsat_core: term in the unsat core is not "
                "an assumption\n";
      report_fatal_error(str_os.str());
    }
    out.push_back(it->second);
  }
  yices_delete_term_vector(&v);
}

/** Push a context */
void yices_solver_impl::push() {
  d_defScopes.push_back(d_defs);
  yices_push(d_ctx);
}

/** Pop a context */
void yices_solver_impl::pop() {
  yices_pop(d_ctx);
  if (!d_defScopes.empty()) {
    d_defs = std::move(d_defScopes.back());
    d_defScopes.pop_back();
  }
}

/** Clear all assertions */
void yices_solver_impl::reset() {
  d_defs = ConstArrayDefs();
  d_defScopes.clear();
  yices_reset_context(d_ctx);
}

/** Get a model */
yices_solver_impl::model_ref yices_solver_impl::get_model() {
  model_t *model = yices_get_model(d_ctx, 1);
  return model_ref(new yices_model_impl(model, *this, d_efac));
}

void yices_solver_impl::to_smt_lib(raw_ostream &o) {
  diag::warn("yices.smt2-unimplemented",
             "yices::to_smt_lib is not implemented; nothing printed");
}

} // namespace solver
} // namespace sea
#endif
