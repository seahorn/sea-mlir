// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- PrintRefs.cpp - convert prints between weak and strong ---*- C++ -*-===//
//
// Print ops (docs/design/Print.md) refer to what they show weakly (by name:
// keeps nothing alive) or strongly (SSA operands: kept alive through every
// pass). These passes convert a program's prints once, before a lowering,
// so a producer emits one form and a tool chooses (sea-vc / sea-run
// --print-refs). ctac makes the same choice at parse time
// (--weak-is-strong).
//
// sea-print-strengthen (weak -> strong). Each name resolves to the value
// with that name (sea::printNamesOf) whose definition dominates the print
// and is dominated by every other such definition (the closest); a memory
// name to the region (Mem-SSA chain) of a value with that name, and then
// to the region's version current at the print (the last version defined
// before it in its block, else a block argument of the region, else the
// same in the immediate dominator). The type must agree as at run time (a
// pointer and an address of one width do). A print with a name that does
// not resolve stays weak: print.strengthen-unresolved (a warning, never an
// error; a weak print is a valid print).
//
// sea-print-weaken (strong -> weak). Each operand becomes a name: the
// region of a memory is `nd.<site>` when it starts at a sea.mem.nondet; a
// value of a nondet op is `nd.<site>`; else the name of its NameLoc when
// no other value of the program has it; else the value gets a fresh NameLoc
// `print.<n>` (wrapping its location; this renames its VC symbol, it does
// not change the query's meaning). A strong print of a struct's leaves
// becomes a print scope with one weak print per leaf (same output lines).
//
// Contract (both). Accepts any program. Changes only print ops (and, for
// weaken, value locations). Produces: strengthen: every resolvable print
// strong; weaken: no print with operands (checked).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Dominance.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/StringMap.h"

using namespace mlir;

namespace sea {
namespace {

/// Memory regions: memory values joined along def-use (an update and the
/// version it consumes, a branch operand and its block argument).
class Regions {
public:
  explicit Regions(ProgramOp program) {
    auto isMem = [](Value v) { return isa<MemType>(v.getType()); };
    program.walk([&](Operation *op) {
      for (Value r : op->getResults())
        if (isMem(r))
          classes.insert(r.getAsOpaquePointer());
      for (Region &region : op->getRegions())
        for (Block &b : region)
          for (BlockArgument a : b.getArguments())
            if (isMem(a))
              classes.insert(a.getAsOpaquePointer());
    });
    program.walk([&](Operation *op) {
      if (isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp, ResetModifiedOp>(op))
        unite(op->getOperand(0), op->getResult(0));
      if (auto br = dyn_cast<BranchOpInterface>(op))
        for (unsigned i = 0, n = op->getNumSuccessors(); i < n; ++i) {
          SuccessorOperands ops = br.getSuccessorOperands(i);
          for (unsigned j = 0, m = ops.size(); j < m; ++j)
            if (Value in = ops[j]; in && isMem(in))
              unite(in, op->getSuccessor(i)->getArgument(j));
        }
    });
  }
  bool same(Value a, Value b) {
    return classes.isEquivalent(a.getAsOpaquePointer(), b.getAsOpaquePointer());
  }
  /// The sea.mem.nondet the region of `v` starts at, if any.
  MemNondetOp start(Value v) {
    for (auto it = classes.findLeader(v.getAsOpaquePointer());
         it != classes.member_end(); ++it)
      if (auto mn =
              Value::getFromOpaquePointer(*it).getDefiningOp<MemNondetOp>())
        return mn;
    return nullptr;
  }

private:
  void unite(Value a, Value b) {
    classes.unionSets(a.getAsOpaquePointer(), b.getAsOpaquePointer());
  }
  llvm::EquivalenceClasses<const void *> classes;
};

/// Do an expected type and a value's type agree (as the interpreter checks)?
bool typesAgree(Type expected, Type actual) {
  if (expected == actual)
    return true;
  return isa<AddrType, PtrType>(expected) && isa<AddrType, PtrType>(actual) &&
         bitWidthOf(expected) == bitWidthOf(actual);
}

class PrintStrengthenPass
    : public PassWrapper<PrintStrengthenPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PrintStrengthenPass)
  StringRef getArgument() const final { return "sea-print-strengthen"; }
  StringRef getDescription() const final {
    return "Weak prints (names) -> strong prints (SSA operands)";
  }
  void runOnOperation() override;
};

void PrintStrengthenPass::runOnOperation() {
  ProgramOp program = getOperation();
  bool any = false;
  program.walk([&](Operation *op) {
    if (auto p = dyn_cast<PrintOp>(op))
      any |= p.getRef().has_value();
    if (auto p = dyn_cast<PrintMemOp>(op))
      any |= !p.isStrong();
  });
  if (!any)
    return;
  llvm::StringMap<SmallVector<Value>> byName;
  auto add = [&](Value v) {
    for (const std::string &n : printNamesOf(v))
      byName[n].push_back(v);
  };
  program.walk([&](Operation *op) {
    for (Value r : op->getResults())
      add(r);
    for (Region &r : op->getRegions())
      for (Block &b : r)
        for (BlockArgument a : b.getArguments())
          add(a);
  });
  DominanceInfo dom(program);
  Regions regions(program);

  // the closest value named `name` whose definition dominates `at`
  auto resolve = [&](StringRef name, Operation *at) -> Value {
    Value best;
    for (Value v : byName.lookup(name)) {
      if (!dom.properlyDominates(v, at))
        continue;
      if (!best) {
        best = v;
        continue;
      }
      // v is closer if best's definition dominates v's
      Operation *vDef = v.getDefiningOp();
      if (vDef ? dom.properlyDominates(best, vDef)
               : dom.dominates(best.getParentBlock(), v.getParentBlock()) &&
                     best.getParentBlock() != v.getParentBlock())
        best = v;
    }
    return best;
  };
  // the version of `mem`'s region current at `at`
  auto current = [&](Value mem, Operation *at) -> Value {
    Block *block = at->getBlock();
    Operation *from = at;
    while (block) {
      for (Operation *op = from ? from->getPrevNode() : &block->back(); op;
           op = op->getPrevNode())
        for (Value r : op->getResults())
          if (isa<MemType>(r.getType()) && regions.same(r, mem))
            return r;
      for (BlockArgument a : block->getArguments())
        if (isa<MemType>(a.getType()) && regions.same(a, mem))
          return a;
      DominanceInfoNode *node = dom.getNode(block);
      if (!node || !node->getIDom())
        return nullptr;
      block = node->getIDom()->getBlock();
      from = nullptr;
    }
    return nullptr;
  };
  auto unresolved = [&](Operation *op, StringRef name) {
    diag::warn("print.strengthen-unresolved",
               "sea-print-strengthen: the name '" + name +
                   "' does not resolve to a value at the print; it stays "
                   "weak",
               op->getLoc());
  };

  program.walk([&](Operation *op) {
    if (auto p = dyn_cast<PrintOp>(op)) {
      if (!p.getRef())
        return;
      Value v = resolve(*p.getRef(), op);
      Type t = *p.getType();
      bool agree = v && (isa<StructType>(t) ? t == v.getType()
                                            : typesAgree(t, v.getType()));
      if (!agree)
        return unresolved(op, *p.getRef());
      p.getArgsMutable().assign(v);
      p.removeRefAttr();
      p.removeTypeAttr();
      return;
    }
    auto pm = dyn_cast<PrintMemOp>(op);
    if (!pm || pm.isStrong())
      return;
    Value region = resolve(*pm.getMem(), op);
    if (!region) // a memory name: any version of the region
      for (Value v : byName.lookup(*pm.getMem()))
        if (isa<MemType>(v.getType()))
          region = v;
    Value mem = region && isa<MemType>(region.getType()) ? current(region, op)
                                                         : Value();
    if (!mem)
      return unresolved(op, *pm.getMem());
    Value addr = resolve(*pm.getAddr(), op);
    if (!addr || addr.getType() != cast<MemType>(mem.getType()).getIndex())
      return unresolved(op, *pm.getAddr());
    SmallVector<Value> args{mem, addr};
    if (pm.getCountRef()) {
      Value n = resolve(*pm.getCountRef(), op);
      if (!n || !isa<IntegerType>(n.getType()))
        return unresolved(op, *pm.getCountRef());
      args.push_back(n);
    }
    pm.getArgsMutable().assign(args);
    pm.removeMemAttr();
    pm.removeAddrAttr();
    pm.removeCountRefAttr();
  });
}

class PrintWeakenPass
    : public PassWrapper<PrintWeakenPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PrintWeakenPass)
  StringRef getArgument() const final { return "sea-print-weaken"; }
  StringRef getDescription() const final {
    return "Strong prints (SSA operands) -> weak prints (names)";
  }
  void runOnOperation() override;
};

void PrintWeakenPass::runOnOperation() {
  ProgramOp program = getOperation();
  bool any = false;
  program.walk([&](Operation *op) {
    if (auto p = dyn_cast<PrintOp>(op))
      any |= p.isStrong();
    if (auto p = dyn_cast<PrintMemOp>(op))
      any |= p.isStrong();
  });
  if (!any)
    return;
  MLIRContext *ctx = &getContext();
  llvm::StringMap<unsigned> uses; // values per name
  auto count = [&](Value v) {
    for (const std::string &n : printNamesOf(v))
      ++uses[n];
  };
  program.walk([&](Operation *op) {
    for (Value r : op->getResults())
      count(r);
    for (Region &r : op->getRegions())
      for (Block &b : r)
        for (BlockArgument a : b.getArguments())
          count(a);
  });
  unsigned fresh = 0;
  Regions regions(program);
  auto nameOf = [&](Value v) -> std::string {
    SmallVector<std::string, 2> names = printNamesOf(v);
    for (const std::string &n : names) // nd.<site> first: stable
      if (StringRef(n).starts_with("nd."))
        return n;
    if (!names.empty() && uses.lookup(names.front()) == 1)
      return names.front();
    std::string n;
    do
      n = "print." + std::to_string(fresh++);
    while (uses.count(n));
    uses[n] = 1;
    Location loc = NameLoc::get(StringAttr::get(ctx, n), v.getLoc());
    if (Operation *def = v.getDefiningOp())
      def->setLoc(NameLoc::get(StringAttr::get(ctx, n), def->getLoc()));
    else
      cast<BlockArgument>(v).setLoc(loc);
    return n;
  };
  SmallVector<Operation *> erase;
  program.walk([&](Operation *op) {
    OpBuilder b(op);
    if (auto p = dyn_cast<PrintOp>(op)) {
      if (!p.isStrong())
        return;
      if (!p.getType()) {
        Value v = p.getArgs()[0];
        p.setRefAttr(b.getStringAttr(nameOf(v)));
        p.setTypeAttr(TypeAttr::get(v.getType()));
        p.getArgsMutable().clear();
        return;
      }
      // a struct by its leaves: a scope, one print per leaf
      PrintScopeOp::create(b, op->getLoc(), p.getTagAttr());
      for (auto [i, v] : llvm::enumerate(p.getArgs())) {
        std::string leaf = p.getNames()
                               ? cast<StringAttr>((*p.getNames())[i]).str()
                               : std::to_string(i);
        auto leafPrint = PrintOp::create(
            b, op->getLoc(), ValueRange{}, b.getStringAttr(leaf),
            b.getStringAttr(nameOf(v)), TypeAttr::get(v.getType()),
            p.getFmtAttr(), ArrayAttr());
        (void)leafPrint;
      }
      PrintEndOp::create(b, op->getLoc(), p.getTagAttr());
      erase.push_back(op);
      return;
    }
    auto pm = dyn_cast<PrintMemOp>(op);
    if (!pm || !pm.isStrong())
      return;
    Value mem = pm.getMemValue();
    std::string memName;
    if (MemNondetOp start = regions.start(mem))
      memName = "nd." + std::to_string(start.getSite());
    else
      memName = nameOf(mem);
    pm.setMemAttr(b.getStringAttr(memName));
    pm.setAddrAttr(b.getStringAttr(nameOf(pm.getAddrValue())));
    if (Value n = pm.getCountValue())
      pm.setCountRefAttr(b.getStringAttr(nameOf(n)));
    pm.getArgsMutable().clear();
  });
  for (Operation *op : erase)
    op->erase();
  WalkResult strong = program.walk([](Operation *op) {
    if (isPrintOp(op) && op->getNumOperands())
      return WalkResult::interrupt();
    return WalkResult::advance();
  });
  if (strong.wasInterrupted() &&
      failed(
          postconditionFailed("sea-print-weaken", "a strong print", program)))
    return signalPassFailure();
}

} // namespace

std::unique_ptr<Pass> createPrintStrengthenPass() {
  return std::make_unique<PrintStrengthenPass>();
}
std::unique_ptr<Pass> createPrintWeakenPass() {
  return std::make_unique<PrintWeakenPass>();
}

} // namespace sea
