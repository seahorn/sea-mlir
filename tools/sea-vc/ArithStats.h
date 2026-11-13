// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ArithStats.h - the arithmetic of the query, by shape ------*- C++
//-*-===//
//
// sea-vc --arith-stats: the multiplications, divisions and remainders of the
// query the solver gets (bvmul, bvudiv, bvsdiv, bvurem, bvsrem, bvsmod and the
// mul overflow predicates of umulo / smulo), each distinct node once, by
// operator, width and operand kind:
//   numeral   an operand is a numeral
//   ite-num   else an operand is an ite tree whose leaves are all numerals
//             (a constant per path)
//   ite-sym   else an operand is an ite tree with a non-numeral leaf
//   nonconst  two non-constant operands, neither an ite
// An ite tree is seen through extract / sign / zero extension. For the ite
// kinds, the distinct conditions of the operands' ite trees. A product by a
// path-merged value (ite-num, ite-sym) is cheap per path and can be expensive
// for a bit-blasting solver as one term; nonconst is a genuine nonlinear
// product.
//
// Per source location: the ops whose node has an origin (sea/VC/Origins.h:
// the sea op that made it, followed through simplify, the boundary and the
// Ackermann lowering); a node with several origins is counted at each; a node
// without one is unattributed (made or rewritten by a later stage).
//
//===----------------------------------------------------------------------===//
#ifndef SEA_VC_TOOL_ARITHSTATS_H
#define SEA_VC_TOOL_ARITHSTATS_H

#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprOpBv.hh"
#include "sea/Expr/TypeChecker.hh"
#include "sea/Interp/Interpreter.h"
#include "sea/Support/Output.h"
#include "sea/VC/Origins.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <map>
#include <set>
#include <string>
#include <tuple>

namespace sea {

class ArithStats {
public:
  enum Kind { Numeral, IteNum, IteSym, NonConst };
  static const char *kindName(Kind k) {
    switch (k) {
    case Numeral:
      return "numeral";
    case IteNum:
      return "ite-num";
    case IteSym:
      return "ite-sym";
    case NonConst:
      return "nonconst";
    }
    return "?";
  }

  /// the census of \p query, attributed by \p origins
  ArithStats(const expr::Expr &query, const ExprOrigins &origins) {
    using namespace expr;
    llvm::DenseSet<const ENode *> seen;
    std::vector<ENode *> stack{query.get()};
    while (!stack.empty()) {
      ENode *n = stack.back();
      stack.pop_back();
      if (!seen.insert(n).second)
        continue;
      for (size_t i = 0; i < n->arity(); ++i)
        stack.push_back(n->arg(i));
      if (!isArith(n) || n->arity() < 2)
        continue;
      count(n, origins);
    }
  }

  /// the report: arith.total, arith.op per (op, width, kind), arith.loc
  /// per (location, op, width, kind)
  void emit(llvm::function_ref<void(const out::Record &)> emitRec) const {
    {
      out::Record rec("arith-total");
      rec.num("ops", ops)
          .num("attributed", ops - unattributed)
          .num("unattributed", unattributed)
          .num("locations", locs.size())
          .num("shared", shared);
      rec.text = "arith.total: ops=" + std::to_string(ops) +
                 " attributed=" + std::to_string(ops - unattributed) +
                 " unattributed=" + std::to_string(unattributed) +
                 " locations=" + std::to_string(locs.size()) +
                 " shared=" + std::to_string(shared);
      emitRec(rec);
    }
    for (const auto &[key, g] : groups) {
      const auto &[op, w, k] = key;
      out::Record rec("arith-op");
      rec.str("op", op).num("width", w).str("operands", kindName(k));
      rec.num("count", g.count).num("unattributed", g.unattributed);
      rec.text = "arith.op: op=" + op + " width=" + std::to_string(w) +
                 " operands=" + kindName(k) +
                 " count=" + std::to_string(g.count) +
                 " unattributed=" + std::to_string(g.unattributed);
      if (k == IteNum || k == IteSym) {
        rec.num("conds", g.conds.size()).num("conds_max", g.condsMax);
        rec.text += " conds=" + std::to_string(g.conds.size()) +
                    " conds-max=" + std::to_string(g.condsMax);
      }
      emitRec(rec);
    }
    for (const auto &[key, g] : perLoc) {
      const auto &[l, op, w, k] = key;
      out::Record rec("arith-loc");
      rec.str("op", op).num("width", w).str("operands", kindName(k));
      rec.num("count", g.count);
      rec.text = "arith.loc: op=" + op + " width=" + std::to_string(w) +
                 " operands=" + kindName(k) +
                 " count=" + std::to_string(g.count);
      if (k == IteNum || k == IteSym) {
        rec.num("conds_max", g.condsMax);
        rec.text += " conds-max=" + std::to_string(g.condsMax);
      }
      addLocFields(rec, "loc", locs.find(l)->second);
      rec.text += " loc=" + (l.empty() ? std::string("-") : l);
      emitRec(rec);
    }
  }

private:
  struct Group {
    uint64_t count = 0, unattributed = 0, condsMax = 0;
    llvm::DenseSet<const expr::ENode *> conds;
  };
  using Key = std::tuple<std::string, unsigned, Kind>;
  std::map<Key, Group> groups;
  std::map<std::tuple<std::string, std::string, unsigned, Kind>, Group> perLoc;
  /// location text -> a location with it (the jsonl fields)
  std::map<std::string, mlir::Location> locs;
  uint64_t ops = 0, unattributed = 0, shared = 0;
  /// ite trees: per root, all leaves numerals
  llvm::DenseMap<const expr::ENode *, bool> numLeaves;

  static bool isArith(expr::ENode *n) {
    using namespace expr;
    return isOpX<BMUL>(n) || isOpX<BUDIV>(n) || isOpX<BSDIV>(n) ||
           isOpX<BUREM>(n) || isOpX<BSREM>(n) || isOpX<BSMOD>(n) ||
           isOpX<UMUL_NO_OVERFLOW>(n) || isOpX<SMUL_NO_OVERFLOW>(n) ||
           isOpX<SMUL_NO_UNDERFLOW>(n);
  }
  static expr::ENode *strip(expr::ENode *n) {
    using namespace expr;
    while (isOpX<BEXTRACT>(n) || isOpX<BSEXT>(n) || isOpX<BZEXT>(n))
      n = n->arg(0);
    return n;
  }
  /// the width of an operand (a bit-vector term); one checker, its cache
  /// shared by the census
  expr::TypeChecker tc;
  unsigned widthOf(expr::ENode *n) {
    return expr::op::bv::width(tc.typeOf(expr::Expr(n)));
  }
  /// \p n is an ite tree (through extensions): its conditions into \p conds;
  /// true if every leaf is a numeral
  static bool iteTree(expr::ENode *root,
                      llvm::DenseSet<const expr::ENode *> &conds) {
    using namespace expr;
    bool allNum = true;
    llvm::DenseSet<const ENode *> seen;
    std::vector<ENode *> stack{root};
    while (!stack.empty()) {
      ENode *n = strip(stack.back());
      stack.pop_back();
      if (!seen.insert(n).second)
        continue;
      if (isOpX<ITE>(n)) {
        conds.insert(n->arg(0));
        stack.push_back(n->arg(1));
        stack.push_back(n->arg(2));
      } else if (!op::bv::is_bvnum(Expr(n))) {
        allNum = false;
      }
    }
    return allNum;
  }

  void count(expr::ENode *n, const ExprOrigins &origins) {
    using namespace expr;
    Kind k = NonConst;
    llvm::DenseSet<const ENode *> conds;
    bool anyNum = false, anyIteNum = false, anyIte = false;
    for (size_t i = 0; i < n->arity(); ++i) {
      ENode *a = n->arg(i);
      if (op::bv::is_bvnum(Expr(a))) {
        anyNum = true;
      } else if (isOpX<ITE>(strip(a))) {
        anyIte = true;
        anyIteNum |= iteTree(a, conds);
      }
    }
    if (anyNum)
      k = Numeral;
    else if (anyIteNum)
      k = IteNum;
    else if (anyIte)
      k = IteSym;
    if (k == Numeral || k == NonConst)
      conds.clear();
    // the operands' width (the predicates are Boolean)
    unsigned w = widthOf(n->arg(0));
    std::string op = n->op().name();
    Group &g = groups[{op, w, k}];
    ++ops;
    ++g.count;
    g.conds.insert(conds.begin(), conds.end());
    g.condsMax = std::max<uint64_t>(g.condsMax, conds.size());
    llvm::ArrayRef<mlir::Location> ls = origins.of(n);
    if (ls.empty()) {
      ++unattributed;
      ++g.unattributed;
      return;
    }
    // one count per distinct location text
    std::set<std::string> texts;
    for (mlir::Location l : ls) {
      std::string t = formatLoc(l);
      if (!texts.insert(t).second)
        continue;
      locs.emplace(t, l);
      Group &lg = perLoc[{t, op, w, k}];
      ++lg.count;
      lg.condsMax = std::max<uint64_t>(lg.condsMax, conds.size());
    }
    if (texts.size() > 1)
      ++shared;
  }
};

} // namespace sea

#endif // SEA_VC_TOOL_ARITHSTATS_H
