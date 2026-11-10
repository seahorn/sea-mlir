// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Replay.h - replay a solver model in the interpreter ------*- C++ -*-===//
//
// Counterexamples come from the interpreter, not from the model: the model
// only supplies nondet values, which the interpreter asks for lazily (as it
// executes each sea.nondet) by evaluating that nondet's VC constant in the
// model. Validation compares every value the interpreter computes with the
// model's value of the same SSA value; a mismatch is a bug in the VC or the
// interpreter.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_VC_REPLAY_H
#define SEA_VC_REPLAY_H

#include "sea/Expr/Smt/Model.hh"
#include "sea/Interp/Interpreter.h"
#include "sea/VC/OpSem.h"

#include "llvm/ADT/DenseSet.h"

namespace sea {

/// The value of a bit-vector numeral as a `width`-bit APInt, or std::nullopt
/// if `num` is not a numeral of that width.
std::optional<llvm::APInt> numeralToAPInt(expr::Expr num, unsigned width);

/// Nondet values from a model. Origin `default`: a value the model leaves
/// unconstrained (its evaluation without completion is a term, not a value),
/// or a cell no constraint reads (solver::Model::defaultCell).
class ModelSource : public NondetSource {
public:
  ModelSource(solver::Model &model, SeaOpSem &sem, OpSemContext &ctx)
      : model(model), sem(sem), ctx(ctx) {}

  mlir::FailureOr<llvm::APInt> get(NondetOp op, std::string &origin,
                                   std::string &error) override;
  mlir::FailureOr<llvm::APInt> getCell(mlir::Operation *siteOp,
                                       const llvm::APInt &addr,
                                       llvm::StringRef object,
                                       unsigned cellWidth, std::string &origin,
                                       std::string &error) override;
  std::optional<llvm::APInt> peekCell(mlir::Operation *siteOp,
                                      const llvm::APInt &addr,
                                      llvm::StringRef object,
                                      unsigned cellWidth,
                                      std::string &origin) override;

private:
  solver::Model &model;
  SeaOpSem &sem;
  OpSemContext &ctx;
};

struct ValueMismatch {
  mlir::Value value;
  llvm::APInt interp;
  std::optional<llvm::APInt> model;
};

/// One split pointer load of the replayed run under the A-PTRINT
/// discharge (sea-vc --ptr-companion=check): the property of that query is
/// the guard to_bits(c) = bits of every split load, c the pointer last
/// stored at the load's address (OpSem companionLoad). The replay keeps a
/// concrete companion beside each pointer-indexed memory, as OpSem builds
/// it (written by pointer stores, whole or the offset-0 byte of a split
/// one; copied by a memmove; kept by every other update; empty at a root)
/// and checks the guard on the run's values.
struct PtrIntObservation {
  mlir::Operation *fromBits; ///< the sea.addr.from_bits
  mlir::Operation *load;     ///< the load of its first byte
  llvm::APInt addr;          ///< the load's address
  llvm::APInt bits;          ///< the loaded bits
  /// the bits of the pointer last stored at addr; none: no pointer was
  /// stored there (the VC's companion cell is fresh)
  std::optional<llvm::APInt> stored;
  bool holds() const { return stored && *stored == bits; }
};

struct ReplayResult {
  RunResult run;
  /// --ptr-companion=check (SeaOpSem::ptrIntChecking): the split pointer
  /// loads the run executed, in order (each execution)
  llvm::SmallVector<PtrIntObservation> ptrInt;
  /// the number of ptrInt entries whose guard fails
  unsigned ptrIntFailed() const {
    unsigned n = 0;
    for (const PtrIntObservation &o : ptrInt)
      n += !o.holds();
    return n;
  }
  uint64_t checked = 0;
  /// values the model leaves unconstrained (its evaluation is not a
  /// numeral: the value does not matter); not compared
  uint64_t dontCare = 0;
  llvm::SmallVector<ValueMismatch> mismatches;
  /// loads whose value was compared and equal to the model's
  llvm::DenseSet<mlir::Value> matchedLoads;
};

/// Runs `program` with nondet values from `model` and, for every value it
/// computes, compares it with the model's value of that SSA value.
/// Runs `program` with nondet values from `model` for its print ops only
/// (docs/design/Print.md): the same execution as replayAndValidate, which
/// skips prints so that its model evaluations do not depend on them.
RunResult replayPrints(ProgramOp program, solver::Model &model, SeaOpSem &sem,
                       OpSemContext &ctx, const RunOptions &options);

ReplayResult replayAndValidate(ProgramOp program, solver::Model &model,
                               SeaOpSem &sem, OpSemContext &ctx,
                               bool addr0IsNull = true,
                               bool memhavocNullNoop = true);

/// A run of the original program (the input before the lowering, sea-vc
/// --replay-original) on the inputs of the model of the lowered one, and
/// where each input came from. The origins (NondetRead / CellRead::origin):
///  - `model`: the model's value, read lazily through the lowered program's
///    op of the same site: nondet sites are stable across the lowering; a
///    cell `<object>+<offset>` of an abstract memory is the cell at the
///    object's placed base (its sea.segment) plus the offset; a byte of a
///    widened memory (aligned k-byte words) is its byte of the word,
///    little-endian;
///  - `default`: the model leaves it unconstrained (ModelSource): 0;
///  - `unmapped`: the lowered program has no op of that site (folded away)
///    or no segment of that object (an object it never accesses): 0. A
///    memhavoc the lowering dropped (sea-drop-fresh-memhavoc: a fresh
///    object's) reads its cells from its chain's sea.mem.nondet, as the
///    lowered program does (origin `model`).
/// The run defers memory-safety violations (RunOptions::deferMemoryErrors):
/// one that reaches the failing assert ends as MemoryError.
struct OriginalReplay {
  RunResult run;
  uint64_t nondets = 0, cells = 0; ///< inputs read
  uint64_t defaulted = 0;          ///< origin default
  uint64_t unmapped = 0;           ///< origin unmapped
};

/// Gives every sea.assert of `program` a stable id (kAssertIdAttr: 0, 1,
/// ... in program order), before the clone of sea-vc --replay-original,
/// so that a run of the original program and of the lowered one name the
/// same assert by the same id, also when asserts share a location or have
/// none.
void tagAsserts(ProgramOp program);

/// The id of the failing assert of `run` (RunResult::assertOp), if any.
std::optional<uint64_t> assertIdOf(const RunResult &run);

OriginalReplay replayOriginal(ProgramOp original, ProgramOp lowered,
                              solver::Model &model, SeaOpSem &sem,
                              OpSemContext &ctx, bool addr0IsNull = true,
                              bool memhavocNullNoop = true);

/// The cause of a failed validation when it is the program's own Mem-SSA
/// no-clobber claim, not the VC (docs/design/Memory.md, "Loads of a version
/// after its consumer"): the loads of an earlier memory version the run
/// executed (RunResult::earlierVersionLoads) whose value is a mismatch, or
/// that a mismatched value depends on (operands, block arguments through
/// the branches, memory chains included) and whose own value was not
/// confirmed by the model. In the order first executed.
llvm::SmallVector<mlir::Operation *>
noClobberSuspects(const ReplayResult &replay);

} // namespace sea

#endif // SEA_VC_REPLAY_H
