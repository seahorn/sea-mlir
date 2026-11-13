// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Passes.h - sea-mlir passes -------------------------------*- C++ -*-===//

#ifndef SEA_TRANSFORMS_PASSES_H
#define SEA_TRANSFORMS_PASSES_H

#include "mlir/Pass/Pass.h"

#include <memory>
#include <string>

namespace mlir {
class PassManager;
} // namespace mlir

namespace sea {

class ProgramOp;

/// sea-place-static: lowers abstract pointers to addresses by placing every
/// object at a constant address (docs/design/Memory-Step2.md) and records
/// the placement as sea.segment ops; a segment's result (the object's base
/// address) replaces the object's uses.
std::unique_ptr<mlir::Pass> createPlaceStaticPass();

/// External placement (sea-place-static place-in / place-out; sea-vc
/// --place-in / --place-out): `out` writes the placement the pass computed,
/// one object per line; `in` takes every object's base from such a file
/// instead of computing it, and checks it as placement does (an object the
/// file does not name is an error). Empty: not used.
struct PlaceFiles {
  std::string in, out;
};
std::unique_ptr<mlir::Pass> createPlaceStaticPass(const PlaceFiles &files);

/// sea-split-wide-access: loads and stores spanning several cells become
/// cell accesses joined with sea.concat / split with sea.extract.
std::unique_ptr<mlir::Pass>
createSplitWideAccessPass(bool keepByteWide = false);
/// sea-merge-asserts: many sea.assert ops -> one exit block with the only
/// assert and halt (BMC form; ctac's ua merge).
/// reportConstFalse: warn fold.assert-false for each assert of constant 0.
std::unique_ptr<mlir::Pass>
createMergeAssertsPass(bool reportConstFalse = true);
/// sea-sink-assumes: every sea.assume of a BMC-form program -> one assume
/// of the conjunction of the assumes on the path (an i1 carried along the
/// CFG), right before the final assert (SeaHorn's UnifyAssumes; sea-vc
/// --sink-assumes). `moved`, if given, receives the number of assumes
/// replaced.
std::unique_ptr<mlir::Pass> createSinkAssumesPass(unsigned *moved = nullptr);
/// The name sea-merge-asserts wraps an original assert's location in, on
/// the branches it puts in the assert's place (`loc("sea.assert"(<loc>))`):
/// the interpreter reports it as the failing assert, not as a branch.
inline constexpr llvm::StringLiteral kMergedAssertLocName = "sea.assert";
/// A stable id of a sea.assert (an i64 discardable attribute; sea-vc
/// --replay-original sets it before the lowering). sea-merge-asserts copies
/// it onto the branches it makes of the assert, and RunResult::assertOp
/// is the op that carries it for the failing assert.
inline constexpr llvm::StringLiteral kAssertIdAttr = "sea.assert_id";
/// sea-lower-ptr-index: sea.ptr.index -> mul / add / ptr.add (the no-wrap
/// flags dropped; docs/design/PtrIndex.md).
std::unique_ptr<mlir::Pass> createLowerPtrIndexPass();
/// sea-check-align: assert every `align` promise (the A-ALIGN discharge
/// job's instrumentation); drops the program's own asserts.
std::unique_ptr<mlir::Pass> createCheckAlignPass();
/// sea-check-ptr-index: assert the no-wrap promise of every flagged
/// sea.ptr.index at the accesses that use it; drop the program's asserts
/// (the paranoid job of the trusted GEP flags, docs/design/PtrIndex.md
/// section 6).
std::unique_ptr<mlir::Pass> createCheckPtrIndexPass(bool assumeAsserts = false);

/// sea-expand-intrinsics: intrinsic ops (min/max, bswap, funnel shifts,
/// overflow bits) become core bit-vector ops; with \p keepMulOverflow,
/// sea.umulo / sea.smulo stay (for a solver that encodes them natively).
std::unique_ptr<mlir::Pass>
createExpandIntrinsicsPass(bool keepMulOverflow = false);

/// sea-wrap-flags: turns the nsw / nuw promises of add / sub / mul / shl
/// into sea.assume (mode=assume) or sea.assert (mode=assert) of "no
/// overflow", or drops them (mode=drop); removes the flags.
std::unique_ptr<mlir::Pass> createWrapFlagsPass();

/// sea-print-strengthen / sea-print-weaken: convert print ops between the
/// weak form (names) and the strong form (SSA operands)
/// (docs/design/Print.md).
std::unique_ptr<mlir::Pass> createPrintStrengthenPass();
std::unique_ptr<mlir::Pass> createPrintWeakenPass();
/// The form tools give a program's prints before a lowering (sea-vc /
/// sea-run --print-refs).
enum class PrintRefs { AsIs, Weak, Strong };
/// Adds sea-print-weaken / sea-print-strengthen (nested on programs) for
/// `refs` (nothing for AsIs).
void addPrintRefsPass(mlir::OpPassManager &pm, PrintRefs refs);
/// sea-test-insert-prints (tests only): prints after every value.
std::unique_ptr<mlir::Pass> createTestInsertPrintsPass();

/// sea-scalarize-structs: struct values become their leaves (struct ops,
/// struct selects and struct block arguments go away).
std::unique_ptr<mlir::Pass> createScalarizeStructsPass();

/// sea-expand-memset: a memset with a symbolic length becomes a bounded
/// chain of stores (option max-length, default 16) with an explicit
/// assume / assert of the bound.
std::unique_ptr<mlir::Pass> createExpandMemSetPass();
/// The discardable attribute (an i64, the bound K) sea-expand-memset puts on
/// each sea.assume(n <= K) it adds: the bounded expansions, an
/// under-approximation sea-vc reports (assume.memset-bound).
inline constexpr llvm::StringLiteral kMemSetBoundAttr = "sea.memset_bound";
/// sea-expand-small-memmove: a memmove of a constant length <= maxBytes
/// index units as loads of every source cell, then stores in address order
/// (exact). \p ptrCompanion: keep every memmove (sea-vc --ptr-companion).
std::unique_ptr<mlir::Pass>
createExpandSmallMemMovePass(unsigned maxBytes, bool ptrCompanion = false);
/// The largest max-bytes of sea-expand-small-memmove: a guard against a
/// mistaken K, well above the measured crossover (expansion pays only for
/// short copies; docs/design/SeaM-memops.md).
inline constexpr unsigned kMaxExpandMemMoveBytes = 1024;
std::unique_ptr<mlir::Pass> createExpandSmallMemMovePass();
/// On the sea.program after sea-expand-small-memmove: its counters (a
/// dictionary of i64 attributes, the keys of `vc.expand-memmove`).
inline constexpr llvm::StringLiteral kExpandMemMoveStatsAttr =
    "sea.expand_memmove";
/// On the sea.program: the number of memhavocs sea-drop-fresh-memhavoc
/// dropped whose soundness rests on A-INB (an i64; absent: none). sea-vc
/// reports it as `assume.a-inb: drop-fresh-memhavoc=<n>`.
/// On the sea.program: the number of memhavocs sea-guard-null-memhavoc
/// guarded (their pointer may be null). sea-vc reports it as
/// `assume.memhavoc-null-noop: vc sites=<n>`.
inline constexpr llvm::StringLiteral kMemhavocNullGuardsAttr =
    "sea.memhavoc_null_guards";
inline constexpr llvm::StringLiteral kDropFreshInbAttr =
    "sea.a_inb.drop_fresh_memhavoc";

/// sea-split-critical-edges: a new block on every critical edge.
std::unique_ptr<mlir::Pass> createSplitCriticalEdgesPass();

/// sea-split-at-checks: every sea.assume, sea.assert and sea.unreachable
/// starts a block, entered from a block that ends right before it (sea-vc
/// --reach=checks); `blocks`: every block is split at its front (--reach=
/// blocks). Adds the splits to `*splits`.
/// sea-split-assert-failures: before each sea.assert with a reach site k,
/// `cond_br c, ^pass, ^fail`, ^fail holding the reach-site marker of k's
/// failure (kAssertFailMarker), both branching to the assert: a reach
/// target reached iff an execution reaches the assert with c false
/// (sea-vc --reach-failures). The program means the same (the marker is a
/// print).
std::unique_ptr<mlir::Pass> createSplitAssertFailuresPass();
std::unique_ptr<mlir::Pass> createSplitAtChecksPass(bool blocks = false,
                                                    unsigned *splits = nullptr);

/// sea-reach-target: R_T of the reach target `id` (sea/Interp/ReachTargets.h;
/// `blocks`: a block target), the program with every assert dropped and an
/// assert(false) at the top of T (lib/Transforms/ReachTarget.cpp).
std::unique_ptr<mlir::Pass> createReachTargetPass(std::string id,
                                                  bool blocks = false);
std::unique_ptr<mlir::Pass> createReachTargetPass();

/// R_T of the target whose op (a block target: the block's first op) is
/// `target`, in place; the new assert gets kReachTargetAttr = `id`. Fails
/// (postcondition) only on a bug.
mlir::LogicalResult buildReachTargetProgram(ProgramOp program,
                                            mlir::Operation *target,
                                            llvm::StringRef id);

/// R_T's sea.assert carries kReachTargetAttr (sea/Interp/ReachTargets.h).

/// sea-apply-tracking: tracking_on/off markers -> `tracked` updates.
std::unique_ptr<mlir::Pass> createApplyTrackingPass();

/// sea-lower-tracking: modification tracking -> shadow memory chains keyed
/// by object base.
std::unique_ptr<mlir::Pass> createLowerTrackingPass();

/// sea-lower-fat-pointers: explicit offset / size companions of pointers
/// and of pointer-holding memories; sea.ptr.base / dereferenceable become
/// arithmetic on them.
std::unique_ptr<mlir::Pass> createLowerFatPointersPass();

/// sea-drop-fresh-memhavoc: memhavoc of an object nothing wrote since its
/// allocation is a no-op: removed.
std::unique_ptr<mlir::Pass> createDropFreshMemHavocPass();
/// sea-guard-null-memhavoc: a memhavoc whose pointer may be null runs only
/// when it is not (memhavoc of null is a no-op; GuardNullMemHavoc.cpp).
std::unique_ptr<mlir::Pass> createGuardNullMemHavocPass();

/// sea-lower-box-all: a sea.box.all whose result is only asserted (through
/// i1 sea.and / sea.or) -> its body at one arbitrary index of the box
/// (LowerBoxAll.cpp, docs/design/BoxAll.md); other uses are errors.
std::unique_ptr<mlir::Pass> createLowerBoxAllPass();
/// With expand-max = \p expandMax: a box of constant upper bounds and at
/// most that many indices is expanded (the body at every index).
std::unique_ptr<mlir::Pass> createLowerBoxAllPass(unsigned expandMax);
/// The default of expand-max (sea-vc --box-all-expand); untuned,
/// docs/design/BoxAll.md.
inline constexpr unsigned kBoxAllExpandDefault = 16;
/// The largest expand-max (the indices of one expanded box): a guard
/// against a mistaken K, well above the measured crossover
/// (docs/design/BoxAll.md).
inline constexpr unsigned kMaxBoxAllExpand = 1024;
/// On the sea.program after sea-lower-box-all: its counters (i64
/// attributes: lowered, guarded, expanded, expand-max, empty, dead,
/// nondets; `vc.box-all`).
inline constexpr llvm::StringLiteral kBoxAllStatsAttr = "sea.box_all";

/// sea-widen-memory: byte memories whose accesses are all k-aligned whole
/// words get k-byte cells (aligned word layout).
std::unique_ptr<mlir::Pass> createWidenMemoryPass();

/// The passes of the VC lowering that sea-vc --lower-disable can leave
/// out: optimizations, the program stays in the form sea-vc encodes
/// without them. sccp also folds constant conditions (fold=false of the
/// pipeline); cse and canonicalize are each of their runs (--lower-opt=false
/// leaves out all three runs). Not sea-drop-fresh-memhavoc: it also decides
/// that a memhavoc of a fresh object is no modification (tracking), a
/// semantics decision.
enum LowerDisable : unsigned {
  LowerDisableSccp = 1u << 0,
  LowerDisableCse = 1u << 1,
  LowerDisableCanonicalize = 1u << 2,
};

/// What sea-range-fold does with a value the integer ranges settle
/// (sea/Transforms/RangeFold.h): nothing (Off: the pass does not run),
/// replace it (Fold), or assert that it has the settled value and keep it
/// (Check: the paranoid job; the program's own asserts are dropped).
enum class RangeFoldMode { Off, Fold, Check };

/// Adds the passes that bring a program into the form sea-vc encodes:
/// folding constants (sccp; \p fold false leaves it out, for the oracle
/// that matches values by location), scalarizing structs, placement (if
/// abstract), expanding symbolic memsets, splitting critical edges,
/// splitting wide accesses, expanding intrinsics (except mul overflow if the
/// solver encodes it: \p nativeMulOverflow). \p widenMemory k > 0 runs
/// sea-widen-memory{k} after sccp. \p expandMemSet false leaves
/// sea-expand-memset out: the encoding with memory-operation terms (sea-vc
/// --seam, docs/design/SeaM-memops.md) takes symbolic lengths exactly, with
/// no bound. The pipeline depends on the selected memory encoding.
/// \p widenReport: sea-widen-memory remarks, per memory, widened or why not.
/// \p keepByteWide: sea-split-wide-access leaves wide accesses of byte
/// memories whole (width terms, sea-vc --seam --seam-wide).
/// \p checkAlign: the A-ALIGN discharge job (sea-check-align after
/// placement, before widening and splitting). \p checkPtrIndex: the GEP
/// promise job (sea-check-ptr-index, at the same place; 2: its diagnostic
/// assume-asserts form); not with checkAlign (each drops the program's
/// asserts). \p opt: upstream cse after sccp, canonicalize + cse after the
/// lowerings (sea-vc --lower-opt, default on); sea-split-critical-edges
/// runs last either way. \p disable: the optimization passes left out
/// (LowerDisable bits; sea-vc --lower-disable), each one whose absence
/// keeps the output in BMC form, so that a lowering with and without it
/// can be compared (utils/cover-diff.py). \p place: external placement
/// files of sea-place-static (with either file the pass runs on a concrete
/// program too: it has no objects to place, and writes an empty file).
/// \p rangeFold: sea-range-fold (after a cse, right before the final
/// canonicalize; sea-vc --range-fold, default off).
/// \p expandMemMove: sea-expand-small-memmove{max-bytes=K} after the
/// metadata lowerings, placement and widening (sea-vc --expand-memmove,
/// default 0: not run); \p ptrCompanion: its option ptr-companion (sea-vc
/// --ptr-companion is not off: every memmove kept). \p boxAllExpand:
/// sea-lower-box-all{expand-max=K} (sea-vc --box-all-expand).
void buildVCLoweringPipeline(
    mlir::OpPassManager &pm, bool abstract, bool nativeMulOverflow = false,
    bool fold = true, unsigned widenMemory = 0, bool expandMemSet = true,
    bool widenReport = false, bool keepByteWide = false,
    bool checkAlign = false, unsigned checkPtrIndex = 0, bool opt = true,
    bool memhavocNullNoop = true, unsigned disable = 0,
    const PlaceFiles &place = {}, RangeFoldMode rangeFold = RangeFoldMode::Off,
    unsigned expandMemMove = 0, bool ptrCompanion = false,
    unsigned boxAllExpand = kBoxAllExpandDefault);

/// Attaches the `lower.pass` log (one level-1 event per pass run: ops
/// before and after, time; lib/Transforms/PassLog.cpp) to \p pm. Costs a
/// check per pass when the tag is off.
void addPassLog(mlir::PassManager &pm);

/// Registers all sea passes with the global registry (for sea-opt).
void registerPasses();

} // namespace sea

#endif // SEA_TRANSFORMS_PASSES_H
