// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
/// The SMT-LIB dialect of a printed query (sea-vc --smt2-dialect).
///
/// z3's printer writes what z3 reads: the solver's own terms, under z3's
/// names. Some of them other solvers do not read (Yices 2.8, Bitwuzla 0.9).
/// The constructs the printer can write that are not portable, and what the
/// portable dialect does with each:
///
///   bvumul_noovfl, bvsmul_noovfl, bvsmul_noudfl  z3 only. Portable: the
///       product at twice the width (zero- / sign-extended operands) and a
///       range test, exact. (SMT-LIB 2.7's bvumulo / bvsmulo: z3 and
///       Bitwuzla read them, Yices 2.8 does not.)
///   bvredor, bvredand  z3 and Bitwuzla, not Yices. Portable: a comparison
///       with 0 / all ones, exact.
///   const-array ((as const ...))  z3 and Bitwuzla, not Yices. No portable
///       form.
///   lambda (z3's array-sorted lambda)  z3 only. No portable form.
///   quantifier (forall, exists)  z3 and Bitwuzla, not Yices. No portable
///       form.
///   array-map, array-default, as-array  z3 only. No portable form.
///
/// A query without a logic: Yices reads nothing (no bit-vector sort). Both
/// dialects start with (set-logic ALL) (or z3's logic, sea-vc --z3-logic):
/// z3 sets up for ALL as for no logic.

#include "sea/Expr/Expr.hh"

#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace sea {
namespace solver {

enum class Smt2Dialect {
  /// the backend's own printer (z3: z3's names)
  Native,
  /// what z3, Yices 2.8 and Bitwuzla 0.9 read, as far as there is a form
  Portable,
};

/// A construct of a printed query that some solver does not read.
struct Smt2Construct {
  /// the name in the query, also the diag kind's suffix
  /// (smt2.nonportable.<name>)
  std::string name;
  /// who reads it ("z3 only", "z3 and Bitwuzla, not Yices")
  llvm::StringRef readers;
  /// the portable dialect expands it
  bool expandable;
};

/// The non-portable constructs of \p asserts, in a fixed order. With \p
/// dialect Portable, \p asserts are rewritten in place: every expandable
/// construct is replaced by its exact portable form; the result lists the
/// constructs left (none expandable).
std::vector<Smt2Construct> smt2Constructs(expr::ExprVector &asserts,
                                          Smt2Dialect dialect);

/// The header of a printed query: a comment naming the dialect, one comment
/// line and one sea::diag warning (kind smt2.nonportable.<name>) per
/// construct left, then (set-logic \p logic).
void smt2Header(llvm::raw_ostream &o, Smt2Dialect dialect,
                const std::vector<Smt2Construct> &left, llvm::StringRef logic);

} // namespace solver
} // namespace sea
