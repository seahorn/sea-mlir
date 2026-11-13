// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Origins.cpp - VC terms -> the sea ops that made them -----*- C++ -*-===//

#include "sea/VC/Origins.h"

#include "llvm/ADT/DenseSet.h"

using namespace expr;

namespace sea {

void ExprOrigins::add(const Expr &n, mlir::Location loc) {
  auto [it, fresh] = map.try_emplace(n.get());
  if (fresh)
    keep.push_back(n);
  if (!llvm::is_contained(it->second, loc))
    it->second.push_back(loc);
}

llvm::ArrayRef<mlir::Location> ExprOrigins::of(const ENode *n) const {
  auto it = map.find(n);
  if (it == map.end())
    return {};
  return it->second;
}

ExprOrigins
ExprOrigins::through(llvm::function_ref<Expr(const Expr &)> img) const {
  ExprOrigins res;
  // in the order the nodes were added (deterministic location order)
  for (const Expr &n : keep)
    if (Expr m = img(n))
      for (mlir::Location l : of(n.get()))
        res.add(m, l);
  return res;
}

ExprOrigins ExprOrigins::along(const Expr &pre, const Expr &post) const {
  ExprOrigins res;
  if (!pre || !post)
    return res;
  // pairs (pre node, post node) at the same position; a pre node is paired
  // once (its first position)
  llvm::DenseSet<const ENode *> seen;
  std::vector<std::pair<ENode *, ENode *>> stack{{pre.get(), post.get()}};
  while (!stack.empty()) {
    auto [a, b] = stack.back();
    stack.pop_back();
    if (!seen.insert(a).second)
      continue;
    if (!(a->op() == b->op()) || a->arity() != b->arity())
      continue;
    for (mlir::Location l : of(a))
      res.add(Expr(b), l);
    for (size_t i = 0; i < a->arity(); ++i)
      stack.push_back({a->arg(i), b->arg(i)});
  }
  return res;
}

} // namespace sea
