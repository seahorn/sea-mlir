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

ExprOrigins
ExprOrigins::along(const Expr &pre, const Expr &post,
                   llvm::function_ref<Expr(const Expr &)> kidImage) const {
  ExprOrigins res;
  if (!pre || !post)
    return res;
  // 1. pairs (pre node, post node) at the same position with the same
  // operator and arity; a pre node is paired once (its first position)
  llvm::DenseMap<const ENode *, ENode *> pairOf;
  std::vector<ENode *> order;
  std::vector<std::pair<ENode *, ENode *>> stack{{pre.get(), post.get()}};
  while (!stack.empty()) {
    auto [a, b] = stack.back();
    stack.pop_back();
    if (pairOf.count(a))
      continue;
    if (!(a->op() == b->op()) || a->arity() != b->arity())
      continue;
    pairOf[a] = b;
    order.push_back(a);
    for (size_t i = 0; i < a->arity(); ++i)
      stack.push_back({a->arg(i), b->arg(i)});
  }
  // 2. a pair is attributed when its nodes are one node, or every kid of
  // the post node is the pre kid itself, the pre kid's pair or its known
  // image (kidImage): a node whose
  // operator matches by chance over other kids gets nothing (missing, not
  // wrong)
  for (ENode *a : order) {
    llvm::ArrayRef<mlir::Location> ls = of(a);
    if (ls.empty())
      continue;
    ENode *b = pairOf.find(a)->second;
    bool ok = a == b;
    if (!ok) {
      ok = true;
      for (size_t i = 0; ok && i < a->arity(); ++i) {
        ENode *ka = a->arg(i), *kb = b->arg(i);
        auto it = pairOf.find(ka);
        ok = ka == kb || (it != pairOf.end() && it->second == kb);
        if (!ok && kidImage)
          if (Expr ki = kidImage(Expr(ka)))
            ok = ki.get() == kb;
      }
    }
    if (!ok)
      continue;
    for (mlir::Location l : ls)
      res.add(Expr(b), l);
  }
  return res;
}

} // namespace sea
