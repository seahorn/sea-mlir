// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifdef WITH_BITWUZLA
#pragma once

#include "sea/Expr/ExprAPInt.hh"
#include <bitwuzla/cpp/bitwuzla.h>

#include <map>
#include <stdexcept>
#include <string>

namespace sea {
namespace solver {

using bzcache_t = std::map<expr::Expr, bitwuzla::Term>;

/// Thrown for an Expr (or a Bitwuzla value) the backend cannot translate.
/// The solver / model entry points report it through sea::diag.
struct bitwuzla_unsupported : std::runtime_error {
  using std::runtime_error::runtime_error;
};

/// Expr <-> Bitwuzla (C++ API). Bool, bit-vectors, arrays (select / store /
/// const-array) and uninterpreted functions; integers and reals are
/// unsupported (bitwuzla_unsupported).
class marshal_bitwuzla {

public:
  /// the symbol of a constant (or declaration): its name as a string
  static std::string name_of(expr::Expr e);

  static bitwuzla::Sort encode_type(expr::Expr exp, bitwuzla::TermManager &tm);

  /// The cache maps every encoded Expr to its term; it must be shared by
  /// everything that talks about the same constants (a constant is a fresh
  /// Bitwuzla term per mk_const).
  static bitwuzla::Term encode_term(expr::Expr exp, bitwuzla::TermManager &tm,
                                    bzcache_t &cache);

  /// Value of `expr` in the current model of `bzla` (the last check_sat must
  /// have returned sat). Constants without a value get their sort's default
  /// (0 / false / const array of the default): the `complete` semantics of
  /// z3's model completion, which Bitwuzla applies unconditionally.
  static expr::Expr eval(expr::Expr expr, expr::ExprFactory &efac,
                         bitwuzla::TermManager &tm, bzcache_t &cache,
                         bool complete, bitwuzla::Bitwuzla &bzla);

  static expr::Expr decode_type(const bitwuzla::Sort &sort,
                                expr::ExprFactory &efac);

  /// A Bitwuzla value term (Bool, bit-vector, array of values) as Expr.
  static expr::Expr decode_value(const bitwuzla::Term &val,
                                 expr::ExprFactory &efac);
};
} // namespace solver
} // namespace sea
#endif
