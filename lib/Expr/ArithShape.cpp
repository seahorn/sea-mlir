// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ArithShape.cpp - arithmetic nodes and their operands' shape --------===//

#include "sea/Expr/ArithShape.hh"
#include "sea/Expr/ExprOpBv.hh"

#include <utility>
#include <vector>

namespace expr {

ArithKind arithKind(const ENode *n) {
  if (isOpX<BMUL>(n) || isOpX<UMUL_NO_OVERFLOW>(n) ||
      isOpX<SMUL_NO_OVERFLOW>(n) || isOpX<SMUL_NO_UNDERFLOW>(n))
    return ArithKind::Mul;
  if (isOpX<BUDIV>(n) || isOpX<BSDIV>(n) || isOpX<BUREM>(n) ||
      isOpX<BSREM>(n) || isOpX<BSMOD>(n))
    return ArithKind::Div;
  return ArithKind::None;
}

void valueTreeKids(ENode *n, llvm::SmallVectorImpl<ENode *> &ks) {
  if (isOpX<ITE>(n)) {
    ks.push_back(n->arg(1));
    ks.push_back(n->arg(2));
  } else if (isOpX<BCONCAT>(n)) {
    for (size_t i = 0; i < n->arity(); ++i)
      ks.push_back(n->arg(i));
  } else if (isOpX<BEXTRACT>(n)) {
    ks.push_back(n->arg(2));
  } else if (isOpX<BSEXT>(n) || isOpX<BZEXT>(n) || isOpX<BNEG>(n) ||
             isOpX<BNOT>(n)) {
    ks.push_back(n->arg(0));
  }
}

ValueTrees::Info ValueTrees::info(ENode *root) {
  std::vector<std::pair<ENode *, bool>> stack{{root, false}};
  llvm::SmallVector<ENode *, 4> ks;
  while (!stack.empty()) {
    auto [n, expanded] = stack.back();
    if (infoMemo.count(n)) {
      stack.pop_back();
      continue;
    }
    ks.clear();
    valueTreeKids(n, ks);
    if (!expanded && !ks.empty()) {
      stack.back().second = true;
      for (ENode *k : ks)
        if (!infoMemo.count(k))
          stack.push_back({k, false});
      continue;
    }
    stack.pop_back();
    Info t;
    if (ks.empty()) {
      t.allNum = op::bv::is_bvnum(Expr(n));
    } else {
      t.ite = isOpX<ITE>(n);
      for (ENode *k : ks) {
        const Info &kt = infoMemo.find(k)->second;
        t.ite |= kt.ite;
        t.allNum &= kt.allNum;
      }
    }
    infoMemo[n] = t;
  }
  return infoMemo.find(root)->second;
}

void ValueTrees::conds(ENode *root, llvm::DenseSet<const ENode *> &conds) {
  llvm::DenseSet<const ENode *> seen;
  std::vector<ENode *> stack{root};
  llvm::SmallVector<ENode *, 4> ks;
  while (!stack.empty()) {
    ENode *n = stack.back();
    stack.pop_back();
    if (!seen.insert(n).second)
      continue;
    if (isOpX<ITE>(n))
      conds.insert(n->arg(0));
    ks.clear();
    valueTreeKids(n, ks);
    stack.insert(stack.end(), ks.begin(), ks.end());
  }
}

bool ValueTrees::iteBelow(ENode *root) {
  std::vector<std::pair<ENode *, bool>> st{{root, false}};
  while (!st.empty()) {
    auto [n, done] = st.back();
    st.pop_back();
    if (iteMemo.count(n))
      continue;
    if (isOpX<ITE>(n)) {
      iteMemo[n] = true;
      continue;
    }
    if (!done) {
      st.push_back({n, true});
      for (size_t i = 0; i < n->arity(); ++i)
        if (!iteMemo.count(n->arg(i)))
          st.push_back({n->arg(i), false});
      continue;
    }
    bool h = false;
    for (size_t i = 0; i < n->arity() && !h; ++i)
      h = iteMemo.find(n->arg(i))->second;
    iteMemo[n] = h;
  }
  return iteMemo.find(root)->second;
}

} // namespace expr
