// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- VCGen.h - VC generation for loop-free sea programs -------*- C++ -*-===//
//
// The half of the VC generator that connects symbols (SeaHorn's split; see
// OpSem.h for the other half). SeaHorn-style encoding, ported from SeaHorn's
// VCGen (lib/seahorn/VCGen.cc, dev18 0cb925cd) over a whole program instead of
// a cut-point edge:
//   - a Bool per block: the block executes;
//   - bb => OR(edges into bb); an edge is pred && bb, or a fresh edge
//     variable for a critical edge;
//   - edge => branch condition; edge => (arg = value passed on the edge);
//   - the exit block executes.
//
// Input must be in BMC form (verifyBmcForm). Then the program can fail its
// assertion iff side && !AND(guarantees) is satisfiable.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_VC_VCGEN_H
#define SEA_VC_VCGEN_H

#include "sea/VC/OpSem.h"

namespace sea {

/// Checks the form the VC generator needs, emitting an error per violation:
/// loop-free; one sea.halt; exactly one sea.assert, immediately before the
/// sea.halt; no critical edges; sea.cond_br targets distinct. A program
/// with neither a sea.halt nor a sea.assert (every path ends in
/// sea.unreachable) is in the form too: it is statically safe.
mlir::LogicalResult verifyBmcForm(ProgramOp program);

/// True if no sea.halt is reachable from the entry block (none at all, or
/// only in blocks no path reaches): no execution reaches the assertion, so
/// it cannot fail. In BMC form the assert is right before the halt.
bool isStaticallySafe(ProgramOp program);

/// How a join defines its block arguments (SeaHorn's VCGen has both;
/// --horn-vcgen-use-ite is the dominant one).
enum class JoinEncoding {
  /// The argument is the term ite(e1, v1, ite(e2, v2, ... vn)) over the
  /// incoming edges (the last edge is the default): no new constant.
  Ite,
  /// The argument is a fresh constant with e_j -> (arg = v_j) per edge.
  Eq,
};

class VCGen {
public:
  explicit VCGen(SeaOpSem &sem, JoinEncoding join = JoinEncoding::Ite)
      : sem(sem), join(join) {}

  /// Encodes `program` into ctx.side() and ctx.guarantees(). The program
  /// must pass verifyBmcForm. With SeaM on (the SeaOpSem's seam()) the join
  /// encoding must be Ite (error vc.seam-join).
  mlir::LogicalResult genVc(ProgramOp program, OpSemContext &ctx);

  /// Seconds spent so far executing the ops of the blocks (sem.exec), and in
  /// the block guards, edges and block arguments (computePathCondForBb).
  double execSeconds() const { return execTime; }
  double joinSeconds() const { return joinTime; }

  /// The object bases of the encoded program (SeaM; see SeaOpSem).
  const expr::BaseRegistry &baseRegistry() const { return sem.baseRegistry(); }

private:
  expr::Expr computePathCondForBb(mlir::Block &bb,
                                  const llvm::DenseSet<mlir::Block *> &reached,
                                  OpSemContext &ctx);

  SeaOpSem &sem;
  JoinEncoding join;
  double execTime = 0, joinTime = 0;
};

/// The query: satisfiable iff some execution fails the assertion.
expr::Expr mkQuery(OpSemContext &ctx);

/// Checks that distinct constants in `e` have distinct names (solvers
/// identify constants by name, so a clash would merge two variables).
mlir::LogicalResult verifyUniqueNames(expr::Expr e, std::string &clash);

} // namespace sea

#endif // SEA_VC_VCGEN_H
