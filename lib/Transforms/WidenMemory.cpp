// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- WidenMemory.cpp - byte memory -> aligned k-byte words ----*- C++ -*-===//
//
// Byte memory is the reference layout, but it costs the solver: every wide
// value is split into bytes on store and recombined on load. A memory all
// of whose accesses are whole, aligned k-byte words can use k-byte cells
// instead (`!sea.mem<I, i(8k), aligned>`): same behaviours, fewer cells.
// The word size is chosen per memory, from its accesses.
//
// A memory is a class of memory values connected by def-use: a version and
// the update that consumes it, a branch operand and its block argument, a
// memmove's source and destination (whose types must match). A class is
// widened only if every access in it is provably k-aligned:
//   - load / store: the value is a multiple of k bytes wide;
//   - every address is k-aligned: the op's `align` >= k (LLVM's promise),
//     or structurally (an allocation / global aligned to >= k, plus
//     constant offsets that are multiples of k; select / block arguments
//     whose inputs all are; a sea.segment (placed object) whose base is);
//   - memset / memmove / memhavoc: the length is a constant multiple of k
//     (run sccp first: sea-vc-lower does).
// Otherwise the class stays bytes; `report` says why, per class.
//
// Contract.
//   Accepts: any program; only exact byte memories (i8 cells, byte
//     addresses) are candidates. A strong sea.print.mem does not stop a
//     memory from widening (the interpreter reads bytes out of words).
//   Produces: every member of a widened class has type
//     !sea.mem<I, i(8k), aligned> (checked); no op changes (memset's value
//     is still one byte, repeated per word).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace sea {
namespace {

class WidenMemoryPass
    : public PassWrapper<WidenMemoryPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WidenMemoryPass)

  WidenMemoryPass() = default;
  WidenMemoryPass(const WidenMemoryPass &other) : PassWrapper(other) {}

  StringRef getArgument() const final { return "sea-widen-memory"; }
  StringRef getDescription() const final {
    return "Give byte memories whose accesses are all k-aligned whole words "
           "k-byte cells (aligned word layout)";
  }

  void runOnOperation() override;

  Option<unsigned> wordBytes{*this, "k",
                             llvm::cl::desc("Word size in bytes (2, 4, 8)"),
                             llvm::cl::init(8)};
  Option<bool> report{*this, "report",
                      llvm::cl::desc("Remark, per memory, whether it was "
                                     "widened and if not why"),
                      llvm::cl::init(false)};

private:
  Value find(Value v) {
    Value &p = parent[v];
    if (!p)
      p = v;
    if (p == v)
      return v;
    return p = find(p);
  }
  void unite(Value a, Value b) { parent[find(a)] = find(b); }
  bool aligned(Value addr, std::optional<uint64_t> align);
  bool provenAligned(Value v, unsigned depth);
  bool constMultiple(Value len);

  llvm::DenseMap<Value, Value> parent;
  llvm::DenseMap<Value, bool> alignedCache;
};

bool WidenMemoryPass::constMultiple(Value len) {
  auto c = len.getDefiningOp<ConstOp>();
  auto i = c ? dyn_cast<IntegerAttr>(c.getValue()) : IntegerAttr();
  return i && i.getValue().urem(wordBytes) == 0;
}

bool WidenMemoryPass::aligned(Value addr, std::optional<uint64_t> align) {
  if (align && *align >= wordBytes)
    return true; // a power of two, so a multiple of k
  return provenAligned(addr, 0);
}

bool WidenMemoryPass::provenAligned(Value v, unsigned depth) {
  if (depth > 32)
    return false;
  auto it = alignedCache.find(v);
  if (it != alignedCache.end())
    return it->second;
  alignedCache[v] = false; // cycles (loops) prove nothing
  uint64_t k = wordBytes;
  bool r = false;
  Operation *def = v.getDefiningOp();
  if (auto a = dyn_cast_if_present<AllocaOp>(def))
    r = a.getAlign() % k == 0;
  else if (auto m = dyn_cast_if_present<MallocOp>(def))
    r = m.getAlign() % k == 0;
  else if (auto g = dyn_cast_if_present<GlobalOp>(def))
    r = g.getAlign() % k == 0;
  else if (auto p = dyn_cast_if_present<PtrAddOp>(def))
    r = constMultiple(p.getOffset()) && provenAligned(p.getPtr(), depth + 1);
  else if (auto p = dyn_cast_if_present<PtrIndexOp>(def))
    // every stride and the constant multiples of k: the offset is
    r = llvm::all_of(p.getStrides(),
                     [&](int64_t s) { return uint64_t(s) % k == 0; }) &&
        uint64_t(p.getOffset()) % k == 0 &&
        provenAligned(p.getBase(), depth + 1);
  else if (auto p = dyn_cast_if_present<AddrAddOp>(def))
    r = constMultiple(p.getOffset()) && provenAligned(p.getAddr(), depth + 1);
  else if (auto f = dyn_cast_if_present<AddrFromIntOp>(def))
    r = constMultiple(f.getOperand());
  else if (auto f = dyn_cast_if_present<AddrFromBitsOp>(def))
    r = constMultiple(f.getOperand());
  else if (auto sg = dyn_cast_if_present<SegmentOp>(def))
    // A segment's numeric base proves alignment directly, without relying
    // only on the object's `align` attribute.
    r = sg.getBase().urem(k) == 0;
  else if (auto s = dyn_cast_if_present<SelectOp>(def))
    r = provenAligned(s.getTrueValue(), depth + 1) &&
        provenAligned(s.getFalseValue(), depth + 1);
  else if (auto arg = dyn_cast<BlockArgument>(v)) {
    Block *b = arg.getOwner();
    r = !b->isEntryBlock() && !b->hasNoPredecessors();
    for (auto it = b->pred_begin(); r && it != b->pred_end(); ++it) {
      auto br = cast<BranchOpInterface>((*it)->getTerminator());
      SuccessorOperands ops = br.getSuccessorOperands(it.getSuccessorIndex());
      Value in = ops[arg.getArgNumber()];
      r = in && provenAligned(in, depth + 1);
    }
  }
  alignedCache[v] = r;
  return r;
}

void WidenMemoryPass::runOnOperation() {
  uint64_t k = wordBytes;
  if (k < 2 || !llvm::isPowerOf2_64(k)) {
    getOperation().emitError("sea-widen-memory: k must be a power of two "
                             ">= 2, got ")
        << k;
    return signalPassFailure();
  }
  parent.clear();
  alignedCache.clear();

  // Classes: every memory value, joined along def-use.
  SmallVector<Value> members;
  auto isMem = [](Value v) { return isa<MemType>(v.getType()); };
  getOperation().walk([&](Operation *op) {
    for (Value r : op->getResults())
      if (isMem(r)) {
        members.push_back(r);
        find(r);
      }
    for (Region &region : op->getRegions())
      for (Block &b : region)
        for (BlockArgument a : b.getArguments())
          if (isMem(a)) {
            members.push_back(a);
            find(a);
          }
  });
  getOperation().walk([&](Operation *op) {
    if (isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp, ResetModifiedOp>(op))
      unite(op->getOperand(0), op->getResult(0));
    if (auto mv = dyn_cast<MemMoveOp>(op))
      unite(mv.getSrc(), mv.getMem());
    if (auto br = dyn_cast<BranchOpInterface>(op))
      for (unsigned i = 0, n = op->getNumSuccessors(); i < n; ++i) {
        Block *dest = op->getSuccessor(i);
        SuccessorOperands ops = br.getSuccessorOperands(i);
        for (unsigned j = 0, m = ops.size(); j < m; ++j)
          if (Value in = ops[j]; in && isMem(in))
            unite(in, dest->getArgument(j));
      }
  });

  // Why each class cannot be widened (first reason found), and its ops.
  llvm::DenseMap<Value, std::string> refused;
  llvm::DenseMap<Value, Operation *> where;
  auto refuse = [&](Value member, const Twine &why, Operation *at) {
    Value c = find(member);
    if (!refused.count(c)) {
      refused[c] = why.str();
      where[c] = at;
    }
  };
  for (Value v : members) {
    auto t = cast<MemType>(v.getType());
    bool byteAddr = isa<PtrType>(t.getIndex()) ||
                    cast<AddrType>(t.getIndex()).getUnit() == 8;
    if (t.isWordLayout() || t.getCell().getWidth() != 8 || !byteAddr)
      refuse(v, "not a byte memory", v.getDefiningOp());
  }
  uint64_t bits = 8 * k;
  getOperation().walk([&](Operation *op) {
    if (auto ld = dyn_cast<LoadOp>(op)) {
      if (bitWidthOf(ld.getType()) % bits != 0)
        refuse(ld.getMem(),
               "a load narrower than (or not a multiple of) a "
               "word",
               op);
      else if (!aligned(ld.getAddr(), ld.getAlign()))
        refuse(ld.getMem(), "a load not provably word-aligned", op);
    } else if (auto st = dyn_cast<StoreOp>(op)) {
      if (bitWidthOf(st.getValue().getType()) % bits != 0)
        refuse(st.getMem(),
               "a store narrower than (or not a multiple of) a "
               "word",
               op);
      else if (!aligned(st.getAddr(), st.getAlign()))
        refuse(st.getMem(), "a store not provably word-aligned", op);
    } else if (auto ms = dyn_cast<MemSetOp>(op)) {
      if (!constMultiple(ms.getLen()))
        refuse(ms.getMem(),
               "a memset length not a constant multiple of a "
               "word",
               op);
      else if (!aligned(ms.getAddr(), ms.getAlign()))
        refuse(ms.getMem(), "a memset not provably word-aligned", op);
    } else if (auto mv = dyn_cast<MemMoveOp>(op)) {
      if (!constMultiple(mv.getLen()))
        refuse(mv.getMem(),
               "a memmove length not a constant multiple of a "
               "word",
               op);
      else if (!aligned(mv.getAddr(), mv.getAlign()) ||
               !aligned(mv.getSrcAddr(), mv.getSrcAlign()))
        refuse(mv.getMem(), "a memmove not provably word-aligned", op);
    } else if (auto mh = dyn_cast<MemHavocOp>(op)) {
      if (!constMultiple(mh.getLen()))
        refuse(mh.getMem(),
               "a memhavoc length not a constant multiple of a "
               "word",
               op);
      else if (!aligned(mh.getAddr(), std::nullopt))
        refuse(mh.getMem(), "a memhavoc not provably word-aligned", op);
    } else if (isa<PrintMemOp>(op)) {
      // a strong print reads bytes out of whatever cells the memory has (no
      // alignment needed): never a reason to keep bytes
    } else if (!isa<BranchOpInterface, MemNondetOp>(op)) {
      for (Value o : op->getOperands())
        if (isMem(o))
          refuse(o, "a use by " + op->getName().getStringRef(), op);
    }
  });

  // Rewrite the widened classes, and report.
  MLIRContext *ctx = &getContext();
  llvm::DenseSet<Value> reported;
  for (Value v : members) {
    Value c = find(v);
    if (refused.count(c)) {
      if (report && reported.insert(c).second)
        emitRemark(where[c] ? where[c]->getLoc() : v.getLoc())
            << "sea-widen-memory: memory kept as bytes: " << refused[c];
      continue;
    }
    auto t = cast<MemType>(v.getType());
    v.setType(MemType::get(ctx, t.getIndex(), IntegerType::get(ctx, bits),
                           MemLayout::aligned));
    if (report && reported.insert(c).second)
      emitRemark(v.getLoc())
          << "sea-widen-memory: memory widened to " << k << "-byte words";
  }
  for (Value v : members) {
    Value c = find(v);
    auto t = cast<MemType>(v.getType());
    if (!refused.count(c) && (t.getCellUnits() != k || !t.isWordLayout())) {
      (void)postconditionFailed("sea-widen-memory",
                                "a member of a widened memory not widened",
                                getOperation());
      return signalPassFailure();
    }
  }
}

} // namespace

std::unique_ptr<Pass> createWidenMemoryPass() {
  return std::make_unique<WidenMemoryPass>();
}

} // namespace sea
