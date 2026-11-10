// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- VCGen.cpp - VC generation for loop-free sea programs -----*- C++ -*-===//

#include "sea/VC/VCGen.h"

#include "sea/Expr/ExprAPInt.hh"
#include "sea/Support/Diag.h"

#include "mlir/IR/RegionGraphTraits.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/StringMap.h"

#include <chrono>
#include <sstream>

using namespace mlir;
using namespace expr;
using namespace expr::op;

namespace sea {

LogicalResult verifyBmcForm(ProgramOp program) {
  bool ok = true;
  Region &body = program.getBody();

  // Unaligned word memory is encoded as if every access matched the cell
  // stored there (the ignore-alignment model): an assumption, so say so.
  program.walk([&](MemNondetOp op) {
    if (op.getType().getLayout() == MemLayout::unaligned)
      diag::warn("vc.unaligned-layout",
                 "unaligned word memory: the VC assumes every access matches "
                 "the cell stored at its address (accesses that overlap "
                 "differently are not modeled)",
                 op.getLoc());
  });

  SmallVector<HaltOp> halts;
  program.walk([&](HaltOp h) { halts.push_back(h); });
  SmallVector<AssertOp> asserts;
  program.walk([&](AssertOp a) { asserts.push_back(a); });
  // no halt and no assert: every path ends in sea.unreachable (folding
  // blocked them all), so the assertion cannot fail; the statically safe
  // form (isStaticallySafe)
  const bool blocked = halts.empty() && asserts.empty();
  if (halts.size() != 1 && !blocked) {
    program.emitError("BMC form: expected exactly one sea.halt, found ")
        << halts.size();
    ok = false;
  }

  if (asserts.size() != 1 && !blocked) {
    program.emitError("BMC form: expected exactly one sea.assert, found ")
        << asserts.size();
    ok = false;
  } else if (halts.size() == 1 &&
             nextNonPrintOp(asserts.front()) != halts.front().getOperation()) {
    asserts.front().emitError(
        "BMC form: sea.assert must be immediately before sea.halt");
    ok = false;
  }

  for (Block &b : body) {
    if (auto cbr = dyn_cast<CondBrOp>(b.getTerminator()))
      if (cbr.getTrueDest() == cbr.getFalseDest()) {
        cbr.emitError("BMC form: sea.cond_br targets must be distinct");
        ok = false;
      }
    // A critical edge goes from a block with several successors to a block
    // with several predecessors.
    unsigned npreds = std::distance(b.pred_begin(), b.pred_end());
    if (npreds > 1)
      for (Block *p : b.getPredecessors())
        if (p->getNumSuccessors() > 1) {
          p->getTerminator()->emitError("BMC form: critical edge to a block "
                                        "with several predecessors");
          ok = false;
        }
  }

  // Loop-free: no edge closes a cycle (DFS colors).
  llvm::DenseMap<Block *, int> color; // 1 on stack, 2 done
  std::function<bool(Block *)> dfs = [&](Block *b) {
    color[b] = 1;
    for (Block *s : b->getSuccessors()) {
      if (color.lookup(s) == 1) {
        b->getTerminator()->emitError("BMC form: the program has a loop");
        return false;
      }
      if (!color.count(s) && !dfs(s))
        return false;
    }
    color[b] = 2;
    return true;
  };
  if (!body.empty() && !dfs(&body.front()))
    ok = false;

  return success(ok);
}

bool isStaticallySafe(ProgramOp program) {
  Region &body = program.getBody();
  if (body.empty())
    return true;
  llvm::DenseSet<Block *> reached;
  SmallVector<Block *> work{&body.front()};
  while (!work.empty()) {
    Block *b = work.pop_back_val();
    if (!reached.insert(b).second)
      continue;
    if (isa<HaltOp>(b->getTerminator()))
      return false;
    for (Block *s : b->getSuccessors())
      work.push_back(s);
  }
  return true;
}

Expr VCGen::computePathCondForBb(Block &bb,
                                 const llvm::DenseSet<Block *> &reached,
                                 OpSemContext &ctx) {
  ExprFactory &efac = ctx.efac();
  // -- predecessors reachable from the entry (others never execute)
  SmallVector<Block *> preds;
  for (Block *p : bb.getPredecessors())
    if (reached.contains(p))
      preds.push_back(p);

  ExprVector edges;
  for (Block *pred : preds)
    edges.push_back(ctx.read(sem.mkSymbReg(pred)));

  // -- update constant representing current bb
  Expr bbV = ctx.havoc(sem.mkSymbReg(&bb));

  // -- an edge into bb is (SRC && DST). SeaHorn's VCGen also introduces an
  // -- edge variable for critical edges; BMC form has none (verifyBmcForm),
  // -- so every edge into a join leaves a block with one successor.
  for (unsigned i = 0, sz = preds.size(); i < sz; ++i) {
    assert((sz == 1 || preds[i]->getNumSuccessors() == 1) &&
           "critical edge: the program is not in BMC form");
    edges[i] = mk<AND>(bbV, edges[i]);
  }

  // -- b_j -> (b1 & e_{1,j} | b2 & e_{2,j} | ...)
  ctx.addSide(mk<IMPL>(bbV, boolop::lor(edges, efac)));

  // -- relate predecessors and conditions under which control flows from them
  for (unsigned i = 0, sz = preds.size(); i < sz; ++i) {
    Expr savedPc = ctx.getPathCond();
    ctx.setPathCond(edges[i]);
    sem.execBr(*preds[i], bb, ctx);
    ctx.setPathCond(savedPc);
  }

  // -- block arguments (PHI nodes): new values
  if (bb.getNumArguments() > 0) {
    std::vector<SmallVector<Expr>> vals;
    for (unsigned j = 0, sz = preds.size(); j < sz; ++j) {
      vals.push_back(sem.execPhi(bb, *preds[j], ctx));
      assert(vals.back().size() == bb.getNumArguments());
    }
    for (BlockArgument arg : bb.getArguments()) {
      unsigned i = arg.getArgNumber();
      Expr reg = sem.mkSymbReg(arg);
      if (join == JoinEncoding::Ite) {
        // -- the edges are disjoint and one holds whenever bb executes:
        // -- the last edge is the default
        Expr val = vals.back()[i];
        for (unsigned j = preds.size() - 1; j > 0; --j)
          val = boolop::lite(edges[j - 1], vals[j - 1][i], val);
        ctx.write(reg, val);
      } else {
        Expr phi = ctx.havoc(reg);
        // a pointer joins by pointer equality (S2g; Equality refuses ptr(w))
        const bool ptrArg = sem.ptrSorted() && isa<AddrType>(arg.getType());
        for (unsigned j = 0, sz = preds.size(); j < sz; ++j)
          ctx.addSide(
              boolop::limp(edges[j], ptrArg ? Expr(mk<PTR_EQ>(phi, vals[j][i]))
                                            : Expr(mk<EQ>(phi, vals[j][i]))));
      }
    }
  }
  // -- the pointer companions of memory arguments (PtrSort S6), joined
  // -- like the memories (ite)
  if (bb.getNumArguments() > 0 && join == JoinEncoding::Ite)
    sem.joinCompanions(bb, preds, edges, ctx);
  return bbV;
}

LogicalResult VCGen::genVc(ProgramOp program, OpSemContext &ctx) {
  // A read can be pushed through a join only if the joined memory is an ite
  // (docs/design/SeaM.md, "Depends on the ite join encoding").
  if (sem.seam() && join != JoinEncoding::Ite) {
    diag::error("vc.seam-join",
                "SeaM needs --vc-join=ite: under eq a joined memory is a "
                "fresh constant and the rewriter cannot read through it",
                program.getLoc());
    return failure();
  }
  Block *entry = &program.getBody().front();
  llvm::ReversePostOrderTraversal<Block *> rpot(entry);
  llvm::DenseSet<Block *> reached(rpot.begin(), rpot.end());
  Expr trueE = mk<TRUE>(ctx.efac());

  using Clock = std::chrono::steady_clock;
  auto secondsSince = [](Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
  };
  bool isEntry = true;
  for (Block *bb : rpot) {
    Expr bbV;
    if (isEntry) {
      // -- initialize bb-exec register; the entry block is unconditional
      bbV = ctx.havoc(sem.mkSymbReg(bb));
      ctx.setPathCond(trueE);
      auto t0 = Clock::now();
      if (failed(sem.exec(*bb, ctx)))
        return failure();
      execTime += secondsSince(t0);
    } else {
      auto t0 = Clock::now();
      bbV = computePathCondForBb(*bb, reached, ctx);
      joinTime += secondsSince(t0);
      t0 = Clock::now();
      if (failed(sem.exec(*bb, ctx.pc(bbV))))
        return failure();
      execTime += secondsSince(t0);
    }
    isEntry = false;
    guards[bb] = bbV;
    // -- reachability (setReach): neither end of a path constrains the
    // -- blocks before it
    if (reach)
      continue;
    // -- the unique exit is asserted to always be reachable
    if (isa<HaltOp>(bb->getTerminator()))
      ctx.addSide(bbV);
    // -- a block ending in sea.unreachable cannot execute
    if (isa<UnreachableOp>(bb->getTerminator()))
      ctx.addSide(boolop::lneg(bbV));
  }
  return success();
}

Expr mkQuery(OpSemContext &ctx) {
  ExprFactory &efac = ctx.efac();
  ExprVector conj(ctx.side().begin(), ctx.side().end());
  ExprVector props(ctx.guarantees().begin(), ctx.guarantees().end());
  conj.push_back(boolop::lneg(boolop::land(props, efac)));
  return boolop::land(conj, efac);
}

LogicalResult verifyUniqueNames(Expr e, std::string &clash) {
  ExprSet consts;
  filter(e, bind::IsConst(), std::inserter(consts, consts.begin()));
  llvm::StringMap<Expr> byName;
  for (Expr c : consts) {
    std::ostringstream os;
    os << *bind::fname(bind::fname(c));
    auto [it, inserted] = byName.try_emplace(os.str(), c);
    if (!inserted && it->second != c) {
      clash = os.str();
      return failure();
    }
  }
  return success();
}

} // namespace sea
