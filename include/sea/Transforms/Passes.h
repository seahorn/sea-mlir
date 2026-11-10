// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Passes.h - sea-mlir passes -------------------------------*- C++ -*-===//

#ifndef SEA_TRANSFORMS_PASSES_H
#define SEA_TRANSFORMS_PASSES_H

#include "mlir/Pass/Pass.h"

#include <memory>

namespace mlir {
class PassManager;
} // namespace mlir

namespace sea {

/// sea-place-static: lowers abstract pointers to addresses by placing every
/// object at a constant address (docs/design/Memory-Step2.md) and records
/// the placement as sea.segment ops; a segment's result (the object's base
/// address) replaces the object's uses.
std::unique_ptr<mlir::Pass> createPlaceStaticPass();

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

/// sea-widen-memory: byte memories whose accesses are all k-aligned whole
/// words get k-byte cells (aligned word layout).
std::unique_ptr<mlir::Pass> createWidenMemoryPass();

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
/// runs last either way.
void buildVCLoweringPipeline(mlir::OpPassManager &pm, bool abstract,
                             bool nativeMulOverflow = false, bool fold = true,
                             unsigned widenMemory = 0, bool expandMemSet = true,
                             bool widenReport = false,
                             bool keepByteWide = false, bool checkAlign = false,
                             unsigned checkPtrIndex = 0, bool opt = true,
                             bool memhavocNullNoop = true);

/// Attaches the `lower.pass` log (one level-1 event per pass run: ops
/// before and after, time; lib/Transforms/PassLog.cpp) to \p pm. Costs a
/// check per pass when the tag is off.
void addPassLog(mlir::PassManager &pm);

/// Registers all sea passes with the global registry (for sea-opt).
void registerPasses();

} // namespace sea

#endif // SEA_TRANSFORMS_PASSES_H
