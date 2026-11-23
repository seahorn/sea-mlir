// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- LogicGuard.h - a query against an SMT-LIB logic ----------*- C++ -*-===//
//
// z3 made for a logic (Z3_mk_solver_for_logic, sea-vc --z3-logic) does not
// refuse a term outside that logic: its tactic for the logic notes what it
// does not understand, drops it and goes on, so the answer can be a wrong
// sat. The guard parses the logic's name into the theories it admits and
// scans every term asserted into such a solver; a term outside the logic is
// a diag::error of kind <prefix>z3-logic-mismatch, and the term is not
// asserted.
//
// Logic names (SMT-LIB): ALL (z3's: everything), else an optional QF_
// (quantifier-free), then in this order an optional A or AX (arrays), UF
// (uninterpreted functions), BV (bit-vectors), and an arithmetic part LIA,
// LRA, LIRA, NIA, NRA or NIRA; at least one of them. Difference logic (IDL,
// RDL) is refused: the scan does not check the difference-logic form. Any
// other name (FP, DT, S, HORN, QF_FD, ...) is refused: the scan has no
// table for it.
//
// What the scan classifies (the operators the z3 marshal, ExprToZ.def,
// encodes): Boolean operators and = / distinct: every logic; bit-vector
// operators, sorts and numerals: BV; select, store, const-array, array
// default, array map, array sorts, lambda (and its application): A;
// function symbols of arity > 0: UF; forall / exists: no QF_; Int / Real
// sorts and numerals: the arithmetic part's I / R; + - * div mod rem and
// < <= > >=: arithmetic, nonlinear (N) when a product has two non-numeral
// factors or a division a non-numeral divisor. Every other operator is
// refused as unclassified.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_EXPR_SMT_LOGICGUARD_H
#define SEA_EXPR_SMT_LOGICGUARD_H

#include "sea/Expr/Expr.hh"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>
#include <vector>

namespace sea::solver {

/// The theories a logic admits
struct LogicTheories {
  std::string name;
  bool quantifiers = false;
  bool arrays = false;
  bool uf = false;
  bool bv = false;
  bool ints = false;
  bool reals = false;
  bool nonlinear = false;
};

/// The theories of the logic \p name; nullopt and \p why when the guard
/// has no table for the name (see the file comment)
std::optional<LogicTheories> parseLogic(llvm::StringRef name, std::string &why);

/// Scans terms against one logic. Each node is scanned once over the life
/// of the scanner (the scanner keeps the roots it scanned, so a node it
/// remembers stays alive).
class LogicScan {
public:
  explicit LogicScan(LogicTheories logic) : m_logic(std::move(logic)) {}
  /// The first construct of \p e outside the logic, described (empty:
  /// \p e is inside it)
  std::string check(const expr::Expr &e);
  const LogicTheories &logic() const { return m_logic; }

private:
  LogicTheories m_logic;
  llvm::DenseSet<const expr::ENode *> m_seen;
  std::vector<expr::Expr> m_roots;
};

} // namespace sea::solver

#endif // SEA_EXPR_SMT_LOGICGUARD_H
