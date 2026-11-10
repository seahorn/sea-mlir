// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ReachTargets.cpp - the reach targets of a program --------*- C++ -*-===//

#include "sea/Interp/ReachTargets.h"

#include "sea/Interp/Interpreter.h"

#include "mlir/IR/Matchers.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"

using namespace mlir;

namespace sea {

namespace {
/// `f.c:3:5@g.c:9:2`: the position, then the call sites; empty without a
/// file position.
std::string positionId(Location loc) {
  SourcePositions p = positionsOf(loc);
  std::string id;
  for (auto [i, f] : llvm::enumerate(p)) {
    id += i ? "@" : "";
    // a position in the .mlir file itself (an op the exporter left without
    // a source position): its file by basename, so that copies of one
    // program in other directories give the same ids
    StringRef file = f.getFilename().getValue();
    if (file.ends_with(".mlir"))
      id += (llvm::sys::path::filename(file) + ":" + Twine(f.getLine()) + ":" +
             Twine(f.getColumn()))
                .str();
    else
      id += fileLocText(f);
  }
  return id;
}
} // namespace

std::vector<ReachTarget> collectReachTargets(ProgramOp program,
                                             ReachKind kind) {
  std::vector<ReachTarget> targets;
  auto add = [&](Operation *op, StringRef name, Location loc, std::string id) {
    targets.push_back({std::move(id), name.str(), op, loc});
  };
  for (Block &b : program.getBody()) {
    if (kind == ReachKind::Blocks) {
      Operation *named = nullptr;
      for (Operation &op : b)
        if (!positionsOf(op.getLoc()).empty()) {
          named = &op;
          break;
        }
      Operation *top = &b.front();
      add(top, "block", named ? named->getLoc() : top->getLoc(),
          named ? positionId(named->getLoc()) : "");
      continue;
    }
    for (Operation &op : b)
      if (isa<AssumeOp, AssertOp, UnreachableOp>(op) ||
          isReachSiteMarker(&op)) {
        add(&op, op.getName().getStringRef(), op.getLoc(),
            positionId(op.getLoc()));
        ReachTarget &t = targets.back();
        if (auto a = op.getAttrOfType<IntegerAttr>(kReachSiteAttr))
          t.site = a.getInt();
        if (isa<AssumeOp, AssertOp>(op))
          t.trueCheck = matchPattern(op.getOperand(0), m_One());
        if (auto p = dyn_cast<PrintOp>(op))
          t.of = p.getTag().str();
        // the target before it in this block: T's next target
        if (targets.size() > 1 && targets[targets.size() - 2].op &&
            targets[targets.size() - 2].op->getBlock() == &b)
          targets[targets.size() - 2].nextSite = t.site;
      }
  }
  // R_T's own target: the id it stands for, taken by no other
  llvm::StringSet<> taken;
  std::vector<bool> fixed(targets.size(), false);
  for (auto [i, t] : llvm::enumerate(targets))
    if (auto id = t.op->getAttrOfType<StringAttr>(kReachTargetAttr)) {
      t.id = id.str();
      taken.insert(t.id);
      fixed[i] = true;
    }
  // the failure of an assert (kAssertFailMarker): the id of the check of
  // its site plus `.fail`, numbered with no other target
  std::vector<bool> fail(targets.size(), false);
  for (auto [i, t] : llvm::enumerate(targets))
    if (auto p = dyn_cast<PrintOp>(t.op);
        p && !fixed[i] && p.getTag() == kAssertFailMarker)
      fail[i] = fixed[i] = true;
  // unlocated: by order; a repeated id: #<n> from the second on
  unsigned unlocated = 0;
  llvm::StringMap<unsigned> seen;
  for (auto [i, t] : llvm::enumerate(targets)) {
    if (fixed[i])
      continue;
    std::string base = t.id;
    do {
      if (base.empty()) {
        t.id = "unlocated#" + std::to_string(unlocated++);
        continue;
      }
      unsigned n = ++seen[base];
      t.id = n > 1 ? base + "#" + std::to_string(n) : base;
    } while (taken.contains(t.id));
  }
  llvm::DenseMap<int64_t, std::string> siteId;
  for (auto [i, t] : llvm::enumerate(targets))
    if (!fail[i] && t.site >= 0)
      siteId.try_emplace(t.site, t.id);
  for (auto [i, t] : llvm::enumerate(targets))
    if (fail[i]) {
      auto it = siteId.find(t.site);
      std::string base = it != siteId.end() ? it->second : t.id;
      t.id = (base.empty() ? "unlocated" : base) + ".fail";
    }
  return targets;
}

std::vector<Operation *> reachTargetOps(llvm::ArrayRef<ReachTarget> targets) {
  std::vector<Operation *> ops;
  for (const ReachTarget &t : targets)
    ops.push_back(t.op);
  return ops;
}

} // namespace sea
