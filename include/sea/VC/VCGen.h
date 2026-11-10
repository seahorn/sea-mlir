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

#include "llvm/ADT/DenseMap.h"

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

  /// Reachability encoding (sea-vc --reach, lib/VC/Reach.cpp): the exit
  /// block is not required to execute and a block ending in sea.unreachable
  /// is not forbidden. Every constraint left is an implication from a block
  /// guard to what holds when the block executes (its edges, branch
  /// conditions and assumes), but for the unguarded ADDR(obj) = base
  /// equalities of placed objects under --seam, which are definitional (they
  /// name the layout and hold on every execution), so a model with guard(B)
  /// true is a path from
  /// the entry into B on which every assume of B and of the blocks before
  /// it holds; no block after B constrains it. An op is at the top of its
  /// block after sea-split-at-checks (sea/VC/Reach.h). Set before genVc.
  void setReach(bool on) { reach = on; }

  /// The Bool constant of `bb` (true iff the block executes), after genVc;
  /// null for a block no path from the entry reaches (never encoded).
  expr::Expr guard(mlir::Block *bb) const { return guards.lookup(bb); }

private:
  expr::Expr computePathCondForBb(mlir::Block &bb,
                                  const llvm::DenseSet<mlir::Block *> &reached,
                                  OpSemContext &ctx);

  SeaOpSem &sem;
  JoinEncoding join;
  bool reach = false;
  llvm::DenseMap<mlir::Block *, expr::Expr> guards;
  double execTime = 0, joinTime = 0;
};

/// The query: satisfiable iff some execution fails the assertion.
expr::Expr mkQuery(OpSemContext &ctx);

/// Checks that distinct constants in `e` have distinct names (solvers
/// identify constants by name, so a clash would merge two variables).
mlir::LogicalResult verifyUniqueNames(expr::Expr e, std::string &clash);

} // namespace sea

#endif // SEA_VC_VCGEN_H
