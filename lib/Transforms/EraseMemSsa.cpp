// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- EraseMemSsa.cpp - one memory's SSA -> a seaimp region -------------===//
//
// docs/design/SeaImp.md, "Erasing memory SSA". The inverse of the region
// part of sea-lower-seaimp: the Mem-SSA chain of the program's memory
// becomes one seaimp memory variable `%mem` and region accesses on it, the
// memory block arguments and the branch operands that feed them go away.
// sea-erase-mem-ssa followed by sea-lower-seaimp gives an equivalent
// program: every access reads and writes the version it did before.
//
// The chain must be linear: at every op that reads or consumes a version,
// that version is the current one (the last one made on every path from
// the entry), and a block has at most one memory argument. The current
// version is a forward dataflow over the CFG (per block: the memory
// argument, else the version all reachable predecessors leave, else none;
// roots and updates make a new one). A load of an earlier version, a
// version consumed twice on one path, or a join without a memory argument
// whose predecessors leave different versions that are read later are
// refused (erase-mem-ssa.non-linear). Unreachable blocks are rewritten but
// not checked (nothing executes them).
//
// Rewriting, op by op:
//   - sea.load / store / memset / memmove / memhavoc (no `tracked`) ->
//     the seaimp region op (`inplace` is dropped: the lowering puts it back,
//     and the check above is what makes it true);
//   - a tracked update and sea.mem.reset_modified -> seaimp.get, the op on
//     the value, seaimp.set of its result (seaimp has no `tracked`);
//   - any other op that reads a version (sea.mem.is_modified, a strong
//     sea.print.mem, a sea.load in a sea.box.all body) -> it reads a
//     seaimp.get put before it (before the top-level op that holds it);
//   - the root: the only root, a sea.mem.nondet in the entry block, becomes
//     the variable (its name and site; the lowering makes the same
//     sea.mem.nondet at the start of the entry block). Any other root
//     (sea.mem.const, a sea.mem.nondet elsewhere, several roots) stays and
//     is followed by a seaimp.set; the variable is then "mem" with a fresh
//     site, and its initial value is never read;
//   - sea.alloca whose pointer indexes the memory -> seaimp.alloca %mem.
//
// Contract.
//   Accepts: a program with at most one memory (one class of memory values
//     connected by def-use: an update and the version it consumes, a branch
//     operand and its block argument); none: unchanged. Refused, with an
//     error: several memories (erase-mem-ssa.regions: one region only), a
//     non-linear chain (erase-mem-ssa.non-linear), a memory value made by an
//     op that is no root or update, or in a nested region
//     (erase-mem-ssa.unsupported).
//   Produces: no memory block argument; every remaining memory value is a
//     seaimp.get result or is used only by seaimp.set (checked:
//     sea-erase-mem-ssa.postcondition).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Dialect/SeaImp/SeaImpOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/SetVector.h"

using namespace mlir;

namespace sea {
namespace {

constexpr StringLiteral kPass = "sea-erase-mem-ssa";

class EraseMemSsaPass
    : public PassWrapper<EraseMemSsaPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(EraseMemSsaPass)

  StringRef getArgument() const final { return "sea-erase-mem-ssa"; }
  StringRef getDescription() const final {
    return "Turn the memory SSA of a program's one memory into a seaimp "
           "memory variable and region accesses (inverse of the region part "
           "of sea-lower-seaimp)";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<SeaDialect, imp::SeaImpDialect>();
  }

  void runOnOperation() override;
};

bool isRoot(Operation *op) { return isa<MemNondetOp, MemConstOp>(op); }

/// The memory operand an update consumes, or null if `op` is no update.
Value consumedMem(Operation *op) {
  if (auto s = dyn_cast<StoreOp>(op))
    return s.getMem();
  if (auto s = dyn_cast<MemSetOp>(op))
    return s.getMem();
  if (auto s = dyn_cast<MemMoveOp>(op))
    return s.getMem();
  if (auto s = dyn_cast<MemHavocOp>(op))
    return s.getMem();
  if (auto s = dyn_cast<ResetModifiedOp>(op))
    return s.getMem();
  return {};
}

/// Union-find over memory values.
struct Classes {
  DenseMap<Value, Value> parent;
  Value find(Value v) {
    auto it = parent.find(v);
    if (it == parent.end()) {
      parent[v] = v;
      return v;
    }
    if (it->second == v)
      return v;
    Value r = find(it->second);
    parent[v] = r;
    return r;
  }
  void unite(Value a, Value b) { parent[find(a)] = find(b); }
};

/// The current memory version at a point.
struct State {
  enum Kind { Unset, None, Version, Conflict } kind = Unset;
  Value version;
  static State none() { return {None, {}}; }
  static State of(Value v) { return {Version, v}; }
  bool operator==(const State &o) const {
    return kind == o.kind && version == o.version;
  }
  bool operator!=(const State &o) const { return !(*this == o); }
  State join(const State &o) const {
    if (kind == Unset)
      return o;
    if (o.kind == Unset || *this == o)
      return *this;
    return {Conflict, {}};
  }
};

bool isMem(Value v) { return isa<MemType>(v.getType()); }

/// The memory values `op` and the ops nested in it read, in operand order
/// (duplicates once). For an update, its consumed operand is included.
llvm::SetVector<Value> memUses(Operation *op) {
  llvm::SetVector<Value> uses;
  op->walk([&](Operation *inner) {
    for (Value v : inner->getOperands())
      if (isMem(v) && v.getParentRegion() == op->getParentRegion())
        uses.insert(v);
  });
  return uses;
}

class Eraser {
public:
  explicit Eraser(ProgramOp program) : program(program) {}
  /// failure: an error was reported, nothing changed.
  LogicalResult run();

private:
  LogicalResult collect();
  /// The block's memory argument (the class has one memory), or null.
  BlockArgument memArg(Block *b);
  State transfer(Block *b, State in, bool check, LogicalResult &ok);
  void rewrite();

  ProgramOp program;
  SmallVector<Value> memValues;
  SmallVector<Operation *> roots;
  MemType memType;
  DenseMap<Block *, State> in, out;
};

LogicalResult Eraser::collect() {
  Region &body = program.getBody();
  Classes classes;
  for (Block &b : body) {
    for (BlockArgument a : b.getArguments())
      if (isMem(a)) {
        memValues.push_back(a);
        classes.find(a);
      }
    for (Operation &op : b) {
      for (Value r : op.getResults()) {
        if (!isMem(r))
          continue;
        memValues.push_back(r);
        classes.find(r);
        if (isRoot(&op)) {
          roots.push_back(&op);
        } else if (Value m = consumedMem(&op)) {
          classes.unite(r, m);
        } else {
          diag::error("erase-mem-ssa.unsupported",
                      Twine(kPass) + ": " + op.getName().getStringRef() +
                          " makes a memory value and is no root or update",
                      op.getLoc());
          return failure();
        }
      }
      // memory values made in nested regions (sea.box.all bodies read
      // memory, never write it)
      for (Region &r : op.getRegions()) {
        WalkResult w = r.walk([&](Operation *inner) {
          for (Value v : inner->getResults())
            if (isMem(v))
              return WalkResult::interrupt();
          for (Region &ir : inner->getRegions())
            for (Block &ib : ir)
              for (BlockArgument a : ib.getArguments())
                if (isMem(a))
                  return WalkResult::interrupt();
          return WalkResult::advance();
        });
        bool nestedArg = llvm::any_of(r.getBlocks(), [](Block &nb) {
          return llvm::any_of(nb.getArguments(),
                              [](BlockArgument a) { return isMem(a); });
        });
        if (w.wasInterrupted() || nestedArg) {
          diag::error("erase-mem-ssa.unsupported",
                      Twine(kPass) +
                          ": a memory value is made in a nested region of " +
                          op.getName().getStringRef(),
                      op.getLoc());
          return failure();
        }
      }
      if (auto br = dyn_cast<BranchOpInterface>(&op)) {
        for (OpOperand &o : op.getOpOperands()) {
          if (!isMem(o.get()))
            continue;
          std::optional<BlockArgument> arg =
              br.getSuccessorBlockArgument(o.getOperandNumber());
          if (!arg) {
            diag::error("erase-mem-ssa.unsupported",
                        Twine(kPass) + ": a memory operand of " +
                            op.getName().getStringRef() +
                            " that is no successor operand",
                        op.getLoc());
            return failure();
          }
          classes.unite(o.get(), *arg);
        }
      }
    }
  }
  if (memValues.empty())
    return success();
  llvm::SmallDenseSet<Value> reps;
  for (Value v : memValues)
    reps.insert(classes.find(v));
  if (reps.size() > 1) {
    diag::error("erase-mem-ssa.regions",
                Twine(kPass) + ": the program has " + Twine(reps.size()) +
                    " memories (independent Mem-SSA chains); only one "
                    "region is supported",
                program.getLoc());
    return failure();
  }
  memType = cast<MemType>(memValues.front().getType());
  return success();
}

BlockArgument Eraser::memArg(Block *b) {
  for (BlockArgument a : b->getArguments())
    if (isMem(a))
      return a;
  return {};
}

State Eraser::transfer(Block *b, State cur, bool check, LogicalResult &ok) {
  auto require = [&](Operation *op, Value v) {
    if (!check || failed(ok) || cur == State::of(v))
      return;
    diag::error("erase-mem-ssa.non-linear",
                Twine(kPass) + ": " + op->getName().getStringRef() +
                    " uses a memory version that is not the current one "
                    "(the memory SSA is not one linear chain: an earlier "
                    "version read, or a version consumed twice)",
                op->getLoc());
    ok = failure();
  };
  for (Operation &op : *b) {
    if (isRoot(&op)) {
      cur = State::of(op.getResult(0));
      continue;
    }
    for (Value v : memUses(&op))
      require(&op, v);
    if (consumedMem(&op))
      cur = State::of(op.getResult(0));
  }
  return cur;
}

LogicalResult Eraser::run() {
  if (failed(collect()))
    return failure();
  if (memValues.empty())
    return success();

  Region &body = program.getBody();
  for (Block &b : body)
    if (llvm::count_if(b.getArguments(),
                       [](BlockArgument a) { return isMem(a); }) > 1) {
      diag::error("erase-mem-ssa.non-linear",
                  Twine(kPass) +
                      ": a block has two memory arguments (two versions of "
                      "one memory live at once)",
                  b.front().getLoc());
      return failure();
    }

  // the current version: iterate to the fixpoint
  LogicalResult ignored = success();
  bool changed = true;
  while (changed) {
    changed = false;
    for (Block &b : body) {
      State s;
      if (b.isEntryBlock())
        s = State::none();
      else if (BlockArgument a = memArg(&b))
        s = State::of(a);
      else
        for (Block *p : b.getPredecessors())
          s = s.join(out.lookup(p));
      if (s.kind == State::Unset)
        continue; // not reached (yet)
      State o = transfer(&b, s, /*check=*/false, ignored);
      if (in.lookup(&b) != s || out.lookup(&b) != o) {
        in[&b] = s;
        out[&b] = o;
        changed = true;
      }
    }
  }
  LogicalResult ok = success();
  for (Block &b : body) {
    auto it = in.find(&b);
    if (it == in.end())
      continue; // unreachable
    (void)transfer(&b, it->second, /*check=*/true, ok);
    if (failed(ok))
      return failure();
  }

  rewrite();

  // postcondition
  for (Block &b : body)
    if (BlockArgument a = memArg(&b))
      return postconditionFailed(kPass, "a memory block argument",
                                 a.getOwner()->getParentOp());
  for (Block &b : body)
    for (Operation &op : b)
      for (Value r : op.getResults())
        if (isMem(r) && !isa<imp::GetOp>(op) &&
            !llvm::all_of(r.getUsers(),
                          [](Operation *u) { return isa<imp::SetOp>(u); }))
          return postconditionFailed(kPass,
                                     "a memory value not read through the "
                                     "variable",
                                     &op);
  return success();
}

void Eraser::rewrite() {
  Region &body = program.getBody();
  Block &entry = body.front();
  OpBuilder b(&entry, entry.begin());
  MLIRContext *ctx = program.getContext();

  // the variable: the root it replaces, else "mem" with a fresh site
  Operation *consumed = nullptr;
  if (roots.size() == 1 && isa<MemNondetOp>(roots.front()) &&
      roots.front()->getBlock() == &entry)
    consumed = roots.front();
  StringAttr name;
  IntegerAttr site;
  if (auto nd = dyn_cast_or_null<MemNondetOp>(consumed)) {
    name = nd.getNameAttr();
    site = nd.getSiteAttr();
  } else {
    uint64_t next = 0;
    program.walk([&](NondetSiteOpInterface s) {
      next = std::max<uint64_t>(next, s.getSite() + 1);
    });
    name = StringAttr::get(ctx, "mem");
    site = b.getI64IntegerAttr(next);
  }
  auto varType = imp::VarType::get(ctx, memType);
  Location varLoc = consumed ? consumed->getLoc() : program.getLoc();
  Value var = imp::VarOp::create(b, varLoc, varType, name, site);

  SmallVector<Operation *> erase;
  if (consumed)
    erase.push_back(consumed);
  auto getMem = [&](OpBuilder &ob, Location loc) {
    return imp::GetOp::create(ob, loc, memType, var).getResult();
  };

  SmallVector<Operation *> ops;
  for (Block &blk : body)
    for (Operation &op : blk)
      ops.push_back(&op);
  for (Operation *op : ops) {
    if (op == var.getDefiningOp() || op == consumed)
      continue;
    OpBuilder ob(op);
    Location loc = op->getLoc();
    if (isRoot(op)) {
      ob.setInsertionPointAfter(op);
      imp::SetOp::create(ob, loc, var, op->getResult(0));
      continue;
    }
    if (auto ld = dyn_cast<LoadOp>(op)) {
      Value r = imp::LoadOp::create(ob, loc, ld.getType(), var, ld.getAddr(),
                                    ld.getAlignAttr())
                    .getResult();
      ld.getResult().replaceAllUsesWith(r);
      erase.push_back(op);
      continue;
    }
    if (auto st = dyn_cast<StoreOp>(op); st && !st.getTracked()) {
      imp::StoreOp::create(ob, loc, var, st.getAddr(), st.getValue(),
                           st.getAlignAttr());
      erase.push_back(op);
      continue;
    }
    if (auto ms = dyn_cast<MemSetOp>(op); ms && !ms.getTracked()) {
      imp::MemSetOp::create(ob, loc, var, ms.getAddr(), ms.getValue(),
                            ms.getLen(), ms.getAlignAttr());
      erase.push_back(op);
      continue;
    }
    if (auto mv = dyn_cast<MemMoveOp>(op); mv && !mv.getTracked()) {
      imp::MemMoveOp::create(ob, loc, var, mv.getAddr(), var, mv.getSrcAddr(),
                             mv.getLen(), mv.getAlignAttr(),
                             mv.getSrcAlignAttr());
      erase.push_back(op);
      continue;
    }
    if (auto mh = dyn_cast<MemHavocOp>(op); mh && !mh.getTracked()) {
      imp::MemHavocOp::create(ob, loc, var, mh.getAddr(), mh.getLen(),
                              mh.getSiteAttr());
      erase.push_back(op);
      continue;
    }
    if (auto al = dyn_cast<AllocaOp>(op);
        al && al.getType() == memType.getIndex()) {
      Value r = imp::AllocaOp::create(ob, loc, al.getType(), var, al.getSize(),
                                      al.getAlignAttr(), al.getSiteAttr(),
                                      al.getLabelAttr())
                    .getResult();
      al.getResult().replaceAllUsesWith(r);
      erase.push_back(op);
      continue;
    }
    // any other use of a version: branch operands are dropped below;
    // every other operand (of the op or of an op nested in it) reads a get
    // put before the op. A remaining update gets a set of its result.
    auto br = dyn_cast<BranchOpInterface>(op);
    Value g;
    op->walk([&](Operation *inner) {
      for (OpOperand &o : inner->getOpOperands()) {
        if (!isMem(o.get()) ||
            o.get().getParentRegion() != op->getParentRegion())
          continue;
        if (inner == op && br &&
            br.getSuccessorBlockArgument(o.getOperandNumber()))
          continue;
        if (!g)
          g = getMem(ob, loc);
        o.set(g);
      }
    });
    if (consumedMem(op)) {
      ob.setInsertionPointAfter(op);
      imp::SetOp::create(ob, loc, var, op->getResult(0));
    }
  }

  // drop the memory successor operands and block arguments
  for (Block &blk : body) {
    auto br = dyn_cast<BranchOpInterface>(blk.getTerminator());
    if (!br)
      continue;
    for (unsigned s = 0, e = blk.getTerminator()->getNumSuccessors(); s < e;
         ++s) {
      SuccessorOperands so = br.getSuccessorOperands(s);
      for (int i = so.getForwardedOperands().size() - 1; i >= 0; --i)
        if (isMem(so.getForwardedOperands()[i]))
          so.erase(i);
    }
  }
  for (Block &blk : body)
    if (BlockArgument a = memArg(&blk)) {
      // its readers and consumers now go through the variable
      a.dropAllUses();
      blk.eraseArgument(a.getArgNumber());
    }
  for (Operation *op : erase)
    op->dropAllDefinedValueUses();
  for (Operation *op : llvm::reverse(erase))
    op->erase();
}

void EraseMemSsaPass::runOnOperation() {
  Eraser e(getOperation());
  if (failed(e.run()))
    signalPassFailure();
}

} // namespace

std::unique_ptr<Pass> createEraseMemSsaPass() {
  return std::make_unique<EraseMemSsaPass>();
}

} // namespace sea
