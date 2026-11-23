// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
/* Generic API for a solver */

#include "sea/Expr/ExprAPInt.hh"
#include "sea/Expr/Smt/Model.hh"
#include "sea/Expr/Smt/Smt2Dialect.hh"
#include "llvm/ADT/iterator_range.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace llvm {
class raw_ostream;
}

namespace sea {
namespace solver {

/** Kind of solver **/
enum class SolverKind { Z3, YICES2, BITWUZLA };

/** Result of the check */
enum class SolverResult {
  /** Formula is satisfiable */
  SAT,
  /** Formula is unsatisfiable */
  UNSAT,
  /** The result is unknown */
  UNKNOWN,
  /** There was an error    */
  ERROR,
};

class Solver {
public:
  using model_ref = std::shared_ptr<Model>;
  using expr_const_it_range =
      llvm::iterator_range<expr::ExprVector::const_iterator>;

  Solver() {}

  virtual ~Solver() {}

  /* Tell the underlying solver without rtti */
  virtual SolverKind get_kind() const = 0;

  /* assert a formula */
  virtual bool add(expr::Expr exp) = 0;

  /** Check for satisfiability */
  virtual SolverResult check() = 0;

  /** Check for satisfiability */
  virtual SolverResult
  check_with_assumptions(const expr_const_it_range &lits) = 0;

  /** Return an unsatisfiable core */
  virtual void unsat_core(expr::ExprVector &out) = 0;

  /** Push a context */
  virtual void push() = 0;

  /** Pop a context */
  virtual void pop() = 0;

  /** Get a model */
  virtual model_ref get_model() = 0;

  /** Clear all assertions */
  virtual void reset() = 0;

  /// Write asserted formulas to SMT-LIB format, in \p dialect
  /// (sea/Expr/Smt/Smt2Dialect.hh; z3's printer only: the others print
  /// their own, whatever \p dialect)
  virtual void to_smt_lib(llvm::raw_ostream &o,
                          Smt2Dialect dialect = Smt2Dialect::Native) = 0;

  /// Why the last check answered unknown, as far as the backend says
  /// ("timeout", "memout", or the backend's own text); empty if not known.
  virtual std::string reason_unknown() { return std::string(); }

  /// The resource count of the solver so far, in the unit of
  /// Limits::rlimit (z3's `rlimit count` statistic; deterministic for a
  /// given z3); nullopt for a backend without one (Yices, Bitwuzla)
  virtual std::optional<uint64_t> rlimit_count() { return std::nullopt; }

  /// The search conflicts of the last check (z3: `conflicts` of the SMT
  /// core plus `sat conflicts` of the SAT core, 0 when neither ran);
  /// nullopt for a backend without the count (Yices, Bitwuzla)
  virtual std::optional<uint64_t> conflicts() { return std::nullopt; }

  /// The backend's statistic \p key (z3: a key of its statistics, as
  /// `sat conflicts`; summed if z3 lists it twice); nullopt if the backend
  /// does not list it, or has no statistics here (Yices, Bitwuzla)
  virtual std::optional<uint64_t> statistic(const std::string &key) {
    (void)key;
    return std::nullopt;
  }
};
} // namespace solver
} // namespace sea
