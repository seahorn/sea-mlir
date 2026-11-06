// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ScalarizeStructs.cpp - structs into their leaves ---------*- C++ -*-===//
//
// Replaces every struct value by its leaves (the non-struct fields, nested
// structs flattened in field order): struct.create / extract / insert become
// plain wiring, a struct select becomes a select per leaf, and a struct
// block argument becomes one argument per leaf (appended to the block's
// arguments; branch operands follow). The interpreter executes both forms,
// which is what lets the fuzzer validate this pass.
//
// Contract.
//   Accepts: struct values in blocks reachable from the entry; struct
//     values used only by struct ops, struct selects, branches and strong
//     sea.print ops (which then show the leaves, the struct type kept as
//     their `type`; docs/design/Print.md).
//   Produces: no value of struct type (checked). Leaves keep the order of
//     StructType::getNumLeaves / getLeafOffset; a struct block argument's
//     leaves are appended to the block's arguments in argument order, and
//     each branch appends the leaves of its struct operands in the same
//     order. sea.struct.nondet keeps its site: one sea.nondet of all its
//     integer / address leaves (low bits first), pointer leaves null.
//   Errors: scalarize.struct-use (another op uses a struct),
//     scalarize.unreachable (struct values in an unreachable block).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/RegionGraphTraits.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/PostOrderIterator.h"

using namespace mlir;

namespace sea {
namespace {

class ScalarizeStructsPass
    : public PassWrapper<ScalarizeStructsPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ScalarizeStructsPass)

  StringRef getArgument() const final { return "sea-scalarize-structs"; }
  StringRef getDescription() const final {
    return "Replace struct values by their fields";
  }

  void runOnOperation() override;
};

/// The leaf types of `t`, in order.
static void leafTypes(Type t, SmallVectorImpl<Type> &out) {
  if (auto s = dyn_cast<StructType>(t)) {
    for (Type f : s.getFields())
      leafTypes(f, out);
    return;
  }
  out.push_back(t);
}

static bool isStruct(Value v) { return isa<StructType>(v.getType()); }

void ScalarizeStructsPass::runOnOperation() {
  Region &body = getOperation().getBody();
  OpBuilder b(&getContext());
  llvm::DenseMap<Value, SmallVector<Value>> leaves;
  auto leavesOf = [&](Value v) -> SmallVector<Value> {
    if (isStruct(v))
      return leaves.lookup(v);
    return {v};
  };

  // Unreachable blocks are outside the domain (the exporter emits reachable
  // blocks only); reverse post-order below would skip them.
  llvm::DenseSet<Block *> reachable;
  for (Block *block : llvm::ReversePostOrderTraversal<Block *>(&body.front()))
    reachable.insert(block);
  for (Block &block : body) {
    if (reachable.contains(&block))
      continue;
    auto structTy = [](Type t) { return isa<StructType>(t); };
    bool hasStruct = llvm::any_of(block.getArgumentTypes(), structTy);
    for (Operation &op : block)
      hasStruct = hasStruct || llvm::any_of(op.getResultTypes(), structTy);
    if (hasStruct) {
      diag::error("scalarize.unreachable",
                  "sea-scalarize-structs: struct values in a block that is "
                  "not reachable from the entry are not supported",
                  block.empty() ? getOperation().getLoc()
                                : block.front().getLoc());
      return signalPassFailure();
    }
  }

  // 1. A struct block argument gets one new argument per leaf, appended.
  for (Block &block : body) {
    unsigned n = block.getNumArguments();
    for (unsigned i = 0; i < n; ++i) {
      BlockArgument arg = block.getArgument(i);
      if (!isStruct(arg))
        continue;
      SmallVector<Type> types;
      leafTypes(arg.getType(), types);
      for (Type t : types)
        leaves[arg].push_back(block.addArgument(t, arg.getLoc()));
    }
  }

  // 2. Struct ops, in reverse post-order (definitions before uses; struct
  // values reach other blocks only through block arguments, done above).
  bool ok = true;
  SmallVector<Operation *> dead;
  for (Block *block : llvm::ReversePostOrderTraversal<Block *>(&body.front()))
    for (Operation &op : *block) {
      b.setInsertionPoint(&op);
      if (auto sn = dyn_cast<StructNondetOp>(op)) {
        // The same nondet bit-vector (same site), split into the leaves.
        Location loc = sn.getLoc();
        Value bits;
        if (unsigned w = sn.getBitsWidth())
          bits = NondetOp::create(b, loc, b.getIntegerType(w),
                                  sn.getCalleeAttr(), sn.getSiteAttr());
        SmallVector<Type> types;
        leafTypes(sn.getType(), types);
        unsigned at = 0;
        for (Type t : types) {
          Value leaf;
          if (isa<PtrType>(t)) {
            leaf = PtrNullOp::create(b, loc, t);
          } else {
            unsigned w = bitWidthOf(t);
            leaf = ExtractOp::create(b, loc, b.getIntegerType(w), bits,
                                     b.getI64IntegerAttr(at));
            if (isa<AddrType>(t))
              leaf = AddrFromIntOp::create(b, loc, t, leaf);
            at += w;
          }
          leaves[sn.getResult()].push_back(leaf);
        }
      } else if (auto c = dyn_cast<StructCreateOp>(op)) {
        for (Value f : c.getFields())
          leaves[c.getResult()].append(leavesOf(f));
      } else if (auto e = dyn_cast<StructExtractOp>(op)) {
        SmallVector<Value> all = leaves.lookup(e.getInput());
        unsigned at = e.getInput().getType().getLeafOffset(e.getIndex());
        if (isStruct(e.getResult())) {
          unsigned n = cast<StructType>(e.getType()).getNumLeaves();
          leaves[e.getResult()] =
              SmallVector<Value>(ArrayRef<Value>(all).slice(at, n));
        } else {
          e.getResult().replaceAllUsesWith(all[at]);
        }
      } else if (auto ins = dyn_cast<StructInsertOp>(op)) {
        SmallVector<Value> all = leaves.lookup(ins.getInput());
        unsigned at = ins.getInput().getType().getLeafOffset(ins.getIndex());
        SmallVector<Value> v = leavesOf(ins.getValue());
        for (unsigned i = 0; i < v.size(); ++i)
          all[at + i] = v[i];
        leaves[ins.getResult()] = all;
      } else if (auto sel = dyn_cast<SelectOp>(op); sel && isStruct(sel)) {
        SmallVector<Value> t = leaves.lookup(sel.getTrueValue()),
                           f = leaves.lookup(sel.getFalseValue());
        for (auto [x, y] : llvm::zip_equal(t, f))
          leaves[sel.getResult()].push_back(SelectOp::create(
              b, sel.getLoc(), x.getType(), sel.getCondition(), x, y));
      } else if (isa<BranchOpInterface>(op)) {
        continue; // step 3
      } else if (auto pr = dyn_cast<PrintOp>(op); pr && pr.isStrong() &&
                                                  !pr.getType() &&
                                                  isStruct(pr.getArgs()[0])) {
        // a strong print of a struct shows its leaves (the struct type
        // stays as the `type` attribute, for the field names)
        Value v = pr.getArgs()[0];
        pr.setTypeAttr(TypeAttr::get(v.getType()));
        pr.getArgsMutable().assign(leaves.lookup(v));
        continue;
      } else {
        for (Value v : op.getOperands())
          if (isStruct(v)) {
            diag::error(
                "scalarize.struct-use",
                "sea-scalarize-structs: " + op.getName().getStringRef() +
                    " uses a struct value",
                op.getLoc());
            ok = false;
          }
        continue;
      }
      dead.push_back(&op);
    }
  if (!ok)
    return signalPassFailure();

  // 3. Branches pass the leaves of struct operands after their operands,
  // matching step 1; then struct operands and arguments go.
  for (Block &block : body) {
    auto br = dyn_cast<BranchOpInterface>(block.getTerminator());
    if (!br)
      continue;
    for (unsigned i = 0; i < br->getNumSuccessors(); ++i) {
      MutableOperandRange args =
          br.getSuccessorOperands(i).getMutableForwardedOperands();
      SmallVector<Value> flat;
      SmallVector<Value> extra;
      for (Value v : ValueRange(args)) {
        if (isStruct(v))
          extra.append(leaves.lookup(v));
        else
          flat.push_back(v);
      }
      if (extra.empty() && flat.size() == args.size())
        continue;
      flat.append(extra);
      args.assign(flat);
    }
  }
  for (Operation *op : dead)
    op->dropAllReferences();
  for (Operation *op : dead)
    op->erase();
  for (Block &block : body) {
    llvm::BitVector structArgs(block.getNumArguments());
    for (BlockArgument arg : block.getArguments())
      if (isStruct(arg))
        structArgs.set(arg.getArgNumber());
    block.eraseArguments(structArgs);
  }
  if (Operation *op = findValueOfType(
          getOperation(), [](Type t) { return isa<StructType>(t); }))
    if (failed(postconditionFailed("sea-scalarize-structs",
                                   "a value of struct type", op)))
      return signalPassFailure();
}

} // namespace

std::unique_ptr<Pass> createScalarizeStructsPass() {
  return std::make_unique<ScalarizeStructsPass>();
}

} // namespace sea
