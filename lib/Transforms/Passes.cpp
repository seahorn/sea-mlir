// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Passes.cpp - pass registration and the VC lowering pipeline -*- C++ -*-//
//
// The VC lowering brings a program into the form sea-vc encodes (see
// sea-vc --agent, INPUT FORM). Order and why:
//  -1. sea-apply-tracking        tracking markers -> `tracked` updates (part
//                                of the meaning, not an optimization)
//   0. sccp                      (upstream) first: values computed from
//                                constants become sea.const (by the op
//                                folders = evalPureOp), so allocation sizes
//                                (placement) and memset / memmove /
//                                memhavoc lengths (expand-memset, the VC's
//                                store chains) are constants, not symbolic
//                                (calloc's n * size: treating it as
//                                symbolic once made a verdict wrong). Ops
//                                with nsw / nuw do not fold; nondet sites
//                                stay; the CFG is kept (dead blocks too).
//                                Not canonicalize here: only constants are
//                                wanted before the lowerings
//   0'. cse                      (upstream; opt, sea-vc --lower-opt) merges
//                                equal pure values (A-INB accesses: one
//                                per distinct load / store left). Ops with
//                                effects (allocations, nondets, sea.mem.const
//                                = an allocation, memory updates, asserts,
//                                prints) never merge
//   1. sea-scalarize-structs     first real lowering: placement retypes
//                                pointers, not the pointer fields of struct
//                                types
//   2. sea-lower-tracking,       abstract programs only: modification
//      (sea-drop-fresh-memhavoc first: a no-op memhavoc is not a tracked
//      modification either)
//      sea-lower-fat-pointers    tracking -> shadow memory keyed by object
//                                base; offsets / sizes made explicit, so
//                                sea.ptr.base / dereferenceable can be
//                                placed (both need pointers' objects)
//      sea-place-static          abstract programs only (!sea.ptr anywhere,
//                                struct fields included: sea::isAbstract)
//   3. sea-expand-memset         symbolic lengths (the array encoding has
//                                only store chains); adds blocks, so before 4
//   4. sea-merge-asserts         BMC form: one assert before the halt
//   5. sea-split-wide-access     the VC encodes single-cell accesses
//   6. sea-expand-intrinsics     the VC encodes core ops; mul overflow is
//                                kept for a solver that has it natively
//   7. canonicalize, cse         (upstream; opt) the values the lowerings
//                                made: folders (evalPureOp), dead pure ops
//                                and unreachable blocks erased, dead block
//                                arguments dropped (region simplification
//                                at its default level, Normal: no block
//                                merging). Prints: weak ones name values
//                                and may dangle once a value is erased or
//                                merged (their contract); strong ones are
//                                users with an effect, never erased or
//                                merged. A dead sea.segment goes too (no
//                                pointer refers to its object).
//   8. sea-split-critical-edges  BMC form, last: after every pass that
//                                changes the CFG (expand-memset and
//                                merge-asserts add blocks; canonicalize
//                                erases some)
// No separate dead-code pass: canonicalize (runRegionDCE,
// eraseUnreachableBlocks) and cse (trivially dead ops) already erase what
// one would, under the same switch.
// Placement and expand-memset still fold sizes / lengths themselves through
// the folders (foldConstant, PassUtils.h) when run standalone without sccp.
// Each pass states its contract (accepted input, postcondition, errors) at
// the top of its file.
//
//===----------------------------------------------------------------------===//

#include "sea/Transforms/Passes.h"
#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"

#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Transforms/Passes.h"

using namespace mlir;

namespace sea {

Operation *findValueOfType(ProgramOp program, function_ref<bool(Type)> bad) {
  Operation *found = nullptr;
  program.walk([&](Operation *op) {
    bool hit = llvm::any_of(op->getResultTypes(), bad);
    for (Region &r : op->getRegions())
      for (Block &b : r)
        hit = hit || llvm::any_of(b.getArgumentTypes(), bad);
    if (!hit)
      return WalkResult::advance();
    found = op;
    return WalkResult::interrupt();
  });
  return found;
}

// Asks the op folders (which evaluate by evalPureOp), operands first; the
// IR is not changed.
static std::optional<APInt> foldConstant(Value v, unsigned depth) {
  Operation *def = v.getDefiningOp();
  if (!def || depth > 64 || def->getNumResults() != 1 ||
      !isa<IntegerType>(v.getType()))
    return std::nullopt;
  SmallVector<Attribute> operands;
  for (Value o : def->getOperands()) {
    std::optional<APInt> c = foldConstant(o, depth + 1);
    operands.push_back(c ? IntegerAttr::get(o.getType(), *c) : Attribute());
  }
  SmallVector<OpFoldResult> folded;
  if (failed(def->fold(operands, folded)) || folded.size() != 1)
    return std::nullopt;
  if (auto a = dyn_cast_if_present<IntegerAttr>(dyn_cast<Attribute>(folded[0])))
    return a.getValue();
  if (auto r = dyn_cast<Value>(folded[0]); r && r != v)
    return foldConstant(r, depth + 1); // select of an arm
  return std::nullopt;
}

std::optional<APInt> foldConstant(Value v) { return foldConstant(v, 0); }

Value offsetIndex(OpBuilder &b, Location loc, Value base, Value off) {
  if (isa<PtrType>(base.getType()))
    return PtrAddOp::create(b, loc, base.getType(), base, off);
  return AddrAddOp::create(b, loc, base.getType(), base, off);
}

Value offsetIndex(OpBuilder &b, Location loc, Value base, uint64_t i) {
  if (i == 0)
    return base;
  unsigned w = bitWidthOf(base.getType());
  auto iW = IntegerType::get(b.getContext(), w);
  // the offset modulo 2^w: addresses wrap (an access wider than the
  // address space revisits cells; APInt(w, i) asserts on i >= 2^w)
  uint64_t m = w >= 64 ? i : (i & ((uint64_t(1) << w) - 1));
  return offsetIndex(
      b, loc, base,
      ConstOp::create(b, loc, iW, IntegerAttr::get(iW, APInt(w, m))));
}

Value ptrIndexOffsetValue(OpBuilder &b, PtrIndexOp p) {
  Location loc = p.getLoc();
  unsigned n = bitWidthOf(p.getBase().getType());
  auto it = IntegerType::get(b.getContext(), n);
  auto cst = [&](int64_t v) -> Value {
    APInt a(64, uint64_t(v), /*isSigned=*/true);
    return ConstOp::create(b, loc, it,
                           IntegerAttr::get(it, n < 64 ? a.trunc(n) : a));
  };
  Value off;
  for (auto [k, i] : llvm::enumerate(p.getIndices())) {
    Value x = i;
    if (bitWidthOf(i.getType()) < n)
      x = SExtOp::create(b, loc, it, i);
    Value t = MulOp::create(b, loc, x, cst(p.getStrides()[k]));
    off = off ? Value(AddOp::create(b, loc, off, t)) : t;
  }
  // a constant-only node (no index): the constant itself
  if (!off)
    return cst(p.getOffset());
  if (p.getOffset() != 0)
    off = AddOp::create(b, loc, off, cst(p.getOffset()));
  return off;
}

LogicalResult postconditionFailed(StringRef pass, const Twine &what,
                                  Operation *at) {
  diag::error((pass + ".postcondition").str(),
              "internal error: " + pass + " left " + what, at->getLoc());
  return failure();
}

namespace {
struct VCLowerOptions : public PassPipelineOptions<VCLowerOptions> {
  Option<bool> fold{*this, "fold",
                    llvm::cl::desc("Run sccp first (sea-vc --lower does); "
                                   "fold=false keeps every op's location, "
                                   "for location-matching oracles"),
                    llvm::cl::init(true)};
  Option<bool> opt{*this, "opt",
                   llvm::cl::desc("cse after sccp, canonicalize + cse after "
                                  "the lowerings (sea-vc --lower-opt); "
                                  "opt=false keeps every op (with fold=false: "
                                  "every op's location)"),
                   llvm::cl::init(true)};
};
} // namespace

void registerPasses() {
  registerPass([] { return createPlaceStaticPass(); });
  registerPass([] { return createScalarizeStructsPass(); });
  registerPass([] { return createExpandMemSetPass(); });
  registerPass([] { return createSplitCriticalEdgesPass(); });
  registerPass([] { return createWidenMemoryPass(); });
  registerPass([] { return createApplyTrackingPass(); });
  registerPass([] { return createLowerTrackingPass(); });
  registerPass([] { return createLowerFatPointersPass(); });
  registerPass([] { return createDropFreshMemHavocPass(); });
  registerPass([] { return createSplitWideAccessPass(); });
  registerPass([] { return createMergeAssertsPass(); });
  registerPass([] { return createSinkAssumesPass(); });
  registerPass([] { return createLowerPtrIndexPass(); });
  registerPass([] { return createCheckAlignPass(); });
  registerPass([] { return createCheckPtrIndexPass(); });
  registerPass([] { return createExpandIntrinsicsPass(); });
  registerPass([] { return createWrapFlagsPass(); });
  registerPass([] { return createPrintStrengthenPass(); });
  registerPass([] { return createPrintWeakenPass(); });
  registerPass([] { return createTestInsertPrintsPass(); });
  PassPipelineRegistration<VCLowerOptions>(
      "sea-vc-lower",
      "The lowering sea-vc --lower runs on a placed program (abstract "
      "programs: run sea-place-static first)",
      [](OpPassManager &pm, const VCLowerOptions &o) {
        buildVCLoweringPipeline(pm, false, false, o.fold, 0, true, false, false,
                                false, 0, o.opt);
      });
}

void addPrintRefsPass(OpPassManager &pm, PrintRefs refs) {
  if (refs == PrintRefs::Weak)
    pm.nest<ProgramOp>().addPass(createPrintWeakenPass());
  else if (refs == PrintRefs::Strong)
    pm.nest<ProgramOp>().addPass(createPrintStrengthenPass());
}

void buildVCLoweringPipeline(OpPassManager &pm, bool abstract,
                             bool nativeMulOverflow, bool fold,
                             unsigned widenMemory, bool expandMemSet,
                             bool widenReport, bool keepByteWide,
                             bool checkAlign, unsigned checkPtrIndex,
                             bool opt) {
  OpPassManager &prog = pm.nest<ProgramOp>();
  prog.addPass(createApplyTrackingPass());
  if (fold)
    prog.addPass(createSCCPPass());
  if (opt)
    prog.addPass(createCSEPass());
  prog.addPass(createScalarizeStructsPass());
  if (abstract) {
    // object metadata, while pointers still know their objects
    prog.addPass(createDropFreshMemHavocPass());
    prog.addPass(createLowerTrackingPass());
    prog.addPass(createLowerFatPointersPass());
    prog.addPass(createPlaceStaticPass());
  }
  // the A-ALIGN discharge job: every `align` promise asserted while the
  // promises are all still there (widening relies on them, splitting drops
  // them)
  if (checkAlign)
    prog.addPass(createCheckAlignPass());
  // the GEP promise job: every promise asserted at the accesses that use it
  if (checkPtrIndex)
    prog.addPass(createCheckPtrIndexPass(checkPtrIndex == 2));
  // after the metadata lowerings (their shadow / companion memories widen
  // too) and placement (constant addresses prove alignment)
  if (widenMemory) {
    auto widen = createWidenMemoryPass();
    if (failed(
            widen->initializeOptions(("k=" + llvm::Twine(widenMemory) +
                                      (widenReport ? " report=true" : ""))
                                         .str(),
                                     [](const Twine &) { return failure(); })))
      llvm::report_fatal_error("sea-widen-memory: bad k");
    prog.addPass(std::move(widen));
  }
  // symbolic memset / memmove lengths: a bounded expansion with an explicit
  // assume (n <= K) for the array encoding; not with memory-operation terms
  // (sea-vc --seam), which are exact for every length
  if (expandMemSet)
    prog.addPass(createExpandMemSetPass());
  // multi-assert: BMC form (one assert before the one halt)
  prog.addPass(createMergeAssertsPass());
  prog.addPass(createSplitWideAccessPass(keepByteWide));
  prog.addPass(createExpandIntrinsicsPass(nativeMulOverflow));
  if (opt) {
    prog.addPass(createCanonicalizerPass());
    prog.addPass(createCSEPass());
  }
  // last: canonicalize changes the CFG (erases blocks)
  prog.addPass(createSplitCriticalEdgesPass());
  // sea.ptr.index stays: the VC encodes it (docs/design/PtrIndex.md).
  // sea-lower-ptr-index provides a standalone arithmetic lowering.
}

} // namespace sea
