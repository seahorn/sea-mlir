// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Reach.h - reachability of check sites and blocks ---------*- C++ -*-===//
//
// sea-vc --reach (docs/reference/cli.md): for each target T of a program in
// BMC form, is there an input whose execution reaches the top of T with
// every earlier assume holding?
//
// Targets and their ids: sea/Interp/ReachTargets.h. They are listed on the
// program as the VC sees it (after the lowering); ids are stable for one
// input and one lowering.
//
// Concrete runs first (--reach-corpus, --reach-fuzz; runReachRuns): before
// any query, corpus inputs and fuzz trials run on the same program, and a
// target some run reaches is reachable (via=run), with that run as its
// witness. The solver gets only the rest.
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
#include "sea/Interp/Fuzz.h"
#include "sea/Interp/ReachTargets.h"
#include "sea/Support/Output.h"
#include "sea/VC/VCGen.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sea {

/// On every sea.assume and sea.assert of a module before the lowering (and
/// before the copy the A-INB check runs, sea-vc --seam): its position in
/// program order. A target the lowering kept is found in the input
/// program by it, not by its location, which inlined code repeats.
inline constexpr llvm::StringLiteral kReachSiteAttr = "sea.reach_site";
void tagReachSites(mlir::ModuleOp module);

/// The side conditions, without the negated assertion.
expr::Expr mkReachQuery(OpSemContext &ctx);

/// The order of the queries (records are printed in program order).
enum class ReachOrder { Program, Deepest };

struct ReachOptions {
  /// one solver for every target, check-sat-assuming its guard (else a
  /// fresh solver per target with the guard asserted: z3 answers
  /// check-sat-assuming with its incremental core)
  bool incremental = true;
  /// a model's run decides every later target it reaches (via=model-run),
  /// with the A-INB check of each under --seam: no query for them
  bool modelRuns = true;
  /// Deepest: reverse program order, so that a model's run passes through
  /// the targets before its own
  ReachOrder order = ReachOrder::Deepest;
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

/// The concrete runs before the queries (sea-vc --reach-corpus,
/// --reach-fuzz): the corpus entries run first, in order, then `fuzzTrials`
/// fuzz trials (sea-run --fuzz's mutations of them) with seed `seed`.
struct ReachRunOptions {
  std::vector<CorpusEntryText> corpus;
  uint64_t fuzzTrials = 0;
  uint64_t seed = 0;
  uint64_t maxSteps = 1'000'000;
};

/// What the runs decided, per target (parallel to the targets).
struct ReachRunDecision {
  bool reached = false;
  /// `seed:<corpus file>` or `trial:<n>` (the fuzz trial, 1-based, seeds
  /// counted)
  std::string by;
  /// the corpus file that reached it, or the witness written for it
  /// (ReachOptions::witnessDir), else empty
  std::string witness;
  /// under the A-INB check: the input program's run read a nondet memory
  /// cell and decided nothing (as inb=undetermined of a model's run)
  bool inbUndetermined = false;
};

struct ReachRuns {
  std::vector<ReachRunDecision> decisions;
  uint64_t trials = 0, seedTrials = 0;
  double seconds = 0;
  unsigned decided = 0;
  /// reached by a run that leaves an object first (the A-INB check): left
  /// to the solver
  unsigned inbViolated = 0;
  /// the deciding runs' uses of the interpreter's assumptions (each run
  /// once, whole runs)
  uint64_t addr0Uses = 0, memhavocUses = 0;
};

/// Runs the corpus and fuzz trials on `program` (in the reach form: after
/// sea-split-at-checks, the program the queries are about) and decides the
/// targets they reach, with the options of the reach replays (interpreter
/// assumptions, the A-INB check, witnesses). Fails with `error` on a corpus
/// value that does not fit its site or an unwritable witness.
mlir::FailureOr<ReachRuns> runReachRuns(ProgramOp program,
                                        llvm::ArrayRef<ReachTarget> targets,
                                        const ReachRunOptions &runOptions,
                                        const ReachOptions &options,
                                        std::string &error);

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
///
/// `runs` (optional): the targets concrete runs decided (runReachRuns) are
/// reported reachable via=run without a query, and a `reach.runs` line
/// counts what runs and the solver decided.
int runReach(ProgramOp program, llvm::ArrayRef<ReachTarget> targets,
             const VCGen &vcgen, expr::Expr query, SeaOpSem &sem,
             OpSemContext &ctx, const ReachOptions &options,
             const ReachHooks &hooks, out::Emitter &out,
             const ReachRuns *runs = nullptr);

} // namespace sea

#endif // SEA_VC_REACH_H
