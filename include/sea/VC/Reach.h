// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Reach.h - reachability of check sites and blocks ---------*- C++ -*-===//
//
// sea-vc --reach (docs/reference/cli.md): for each target T of a program in
// BMC form, is there an input whose execution reaches the top of T with
// every earlier assume holding?
//
// Targets: the check sites (sea.assume, sea.assert, and sea.unreachable,
// which is where the lowering leaves an assume of constant false), or every
// block. They are listed on the program as the VC sees it (after the
// lowering). Ids are stable for one input and one lowering: the op's file
// position and the call sites it was inlined at (`f.c:3:5@g.c:9:2`), a
// block's from its first located op; an op without a position gets
// `unlocated#<k>` (k in program order); a repeated id gets `#<n>`.
//
// Encoding: the VC of VCGen with setReach (no exit, no ban on
// sea.unreachable blocks), whose side conditions only say what holds in a
// block that executes, guarded by the block's Bool. An assume is a fact of
// its own block's guard, so the guard of T's block would also assume T and
// the assumes after it in the block. Instead, after the targets are listed,
// sea-split-at-checks gives each target a block right before it that holds
// no check site (checks: the ops between the previous check site and T;
// blocks: empty): its guard holds iff an execution reaches the top of T
// with every earlier assume holding. One query per target, check-sat-
// assuming that guard on the side conditions (mkReachQuery), on one
// incremental solver or one solver per target. A sat answer is replayed in
// the interpreter: the model's values are validated up to T, and the run
// must reach T.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_VC_REACH_H
#define SEA_VC_REACH_H

#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Expr/Smt/Solver.hh"
#include "sea/Support/Output.h"
#include "sea/VC/VCGen.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sea {

enum class ReachKind { Off, Checks, Blocks };

struct ReachTarget {
  std::string id;
  /// `sea.assume`, `sea.assert`, `sea.unreachable`; `block` for a block
  std::string opName;
  /// the op (a block: its first op): the target is the top of this op
  mlir::Operation *op = nullptr;
  /// the location reported (`at=`): the op's, a block's first located op's
  mlir::Location loc;
};

/// The targets of `program`, in program order.
std::vector<ReachTarget> collectReachTargets(ProgramOp program, ReachKind kind);

/// The side conditions, without the negated assertion.
expr::Expr mkReachQuery(OpSemContext &ctx);

struct ReachOptions {
  /// one solver for every target, check-sat-assuming its guard (else a
  /// fresh solver per target with the guard asserted: z3 answers
  /// check-sat-assuming with its incremental core)
  bool incremental = true;
  /// write each reachable target's inputs here (empty: none)
  std::string witnessDir;
  /// echoed in the replay hint of a witness file
  std::string programPath;
  /// the witnesses replay on the lowered program (sea-vc --lower), else
  /// on the input program
  bool lowered = false;
  /// Under --seam with A-INB: the program before the lowering (abstract,
  /// every access checked against its object), run on each model's nondet
  /// values (sea-vc's A-INB classification of a counterexample). A model
  /// whose run leaves an object before reaching its target is
  /// `replay=memory-error`. Null: no such check.
  ProgramOp abstractProgram;
  /// the interpreter's named assumptions, as for the safety replays
  /// (sea-vc --addr0-is-null, --memhavoc-null): every reach run (the placed
  /// replay and the A-INB check) uses them; the uses are printed as
  /// assume.addr0-is-null / assume.memhavoc-null-noop: uses=<n>
  bool addr0IsNull = true;
  bool memhavocNullNoop = true;
  /// TEST ONLY (sea-vc --x-reach-plant=mismatch): count one mismatch on
  /// each replay, to exercise the reporting
  bool plantMismatch = false;
};

/// What the driver needs from sea-vc, which owns the solver configuration
/// and the lowerings of the query.
struct ReachHooks {
  /// A solver with the query asserted; null after an error is reported.
  std::function<std::unique_ptr<solver::Solver>()> makeSolver;
  /// After a sat answer of `s`: calls `use` with the model the replay reads
  /// (the solver's, read back through the lowerings of the query). Returns
  /// 0, or a nonzero code that ends the run (sea-vc's retries, errors).
  std::function<int(solver::Solver &s,
                    llvm::function_ref<void(solver::Model &)> use)>
      withModel;
  /// After an unknown answer of `s` that took `seconds`: why (`rlimit`,
  /// `timeout`, or the backend's words). Optional; else
  /// Solver::reason_unknown.
  std::function<std::string(solver::Solver &s, double seconds)> reasonUnknown;
};

/// Runs the queries, prints a `reach` record per target and the
/// `reach.summary` line. Returns the exit status: 0 every target decided and
/// every reachable one replayed to its top; 3 some target unknown; 4 a
/// replay stopped at a memory-safety error before its target (the answer
/// rests on A-INB); 2 a replay that disagrees with the model (a bug); 1 an
/// error; or a code of ReachHooks::withModel.
int runReach(ProgramOp program, llvm::ArrayRef<ReachTarget> targets,
             const VCGen &vcgen, expr::Expr query, SeaOpSem &sem,
             OpSemContext &ctx, const ReachOptions &options,
             const ReachHooks &hooks, out::Emitter &out);

} // namespace sea

#endif // SEA_VC_REACH_H
