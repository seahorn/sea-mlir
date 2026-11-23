// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sea/Expr/Smt/LogicGuard.h"

#include "sea/Expr/ExprOpBinder.hh"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <sstream>

using namespace expr;
using namespace expr::op;

namespace sea::solver {

std::optional<LogicTheories> parseLogic(llvm::StringRef name,
                                        std::string &why) {
  LogicTheories t;
  t.name = name.str();
  if (name == "ALL") {
    t.quantifiers = t.arrays = t.uf = t.bv = t.ints = t.reals = t.nonlinear =
        true;
    return t;
  }
  llvm::StringRef r = name;
  t.quantifiers = !r.consume_front("QF_");
  bool any = false;
  if (r.consume_front("AX") || r.consume_front("A"))
    t.arrays = any = true;
  if (r.consume_front("UF"))
    t.uf = any = true;
  if (r.consume_front("BV"))
    t.bv = any = true;
  if (r == "IDL" || r == "RDL") {
    why = "difference logic (" + r.str() +
          ") is not checked by the logic guard (it does not check the "
          "difference-logic form of the terms)";
    return std::nullopt;
  }
  if (!r.empty()) {
    if (r.consume_front("N"))
      t.nonlinear = true;
    else if (!r.consume_front("L")) {
      why = "not a logic the logic guard has a table for (ALL, or [QF_]"
            "[A|AX][UF][BV][LIA|LRA|LIRA|NIA|NRA|NIRA])";
      return std::nullopt;
    }
    if (r == "IA")
      t.ints = true;
    else if (r == "RA")
      t.reals = true;
    else if (r == "IRA")
      t.ints = t.reals = true;
    else {
      why = "not a logic the logic guard has a table for (ALL, or [QF_]"
            "[A|AX][UF][BV][LIA|LRA|LIRA|NIA|NRA|NIRA])";
      return std::nullopt;
    }
    any = true;
  }
  if (!any) {
    why = "names no theory (ALL, or [QF_][A|AX][UF][BV][LIA|LRA|LIRA|NIA|NRA|"
          "NIRA])";
    return std::nullopt;
  }
  return t;
}

namespace {
/// A numeral of the arithmetic (an Int / Real numeral, or its negation)
bool isArithNumeral(const Expr &e) {
  if (isOpX<MPZ>(e) || isOpX<MPQ>(e))
    return true;
  return isOpX<UN_MINUS>(e) && e->arity() == 1 && isArithNumeral(e->arg(0));
}

std::string show(const Expr &e) {
  std::ostringstream os;
  os << *e;
  std::string s = os.str();
  if (s.size() > 120)
    s = s.substr(0, 117) + "...";
  return s;
}
} // namespace

std::string LogicScan::check(const Expr &root) {
  const LogicTheories &L = m_logic;
  llvm::SmallVector<Expr, 32> todo{root};
  std::string bad;
  // the nodes marked seen by this call: unmarked again when the term is
  // refused (their arguments may not have been scanned)
  llvm::SmallVector<const ENode *, 32> added;
  // \p admitted: whether the logic admits it; else \p what is the answer
  auto require = [&](bool admitted, llvm::function_ref<std::string()> what) {
    if (!admitted && bad.empty())
      bad = what();
    return admitted;
  };
  while (!todo.empty() && bad.empty()) {
    Expr e = todo.pop_back_val();
    if (!m_seen.insert(e.get()).second)
      continue;
    added.push_back(e.get());
    const Operator &op = e->op();
    // the arguments to scan: all, unless the case says otherwise
    size_t from = 0, to = e->arity();
    switch (op.getFamilyId()) {
    case OpFamilyId::Terminal:
      if (isOpX<MPZ>(e))
        require(L.ints, [&] {
          return std::string("an Int numeral (" + show(e) + ")");
        });
      else if (isOpX<MPQ>(e))
        require(L.reals, [&] {
          return std::string("a Real numeral (" + show(e) + ")");
        });
      else if (isOpX<BVSORT>(e))
        require(L.bv, [&] {
          return std::string("a bit-vector sort (" + show(e) + ")");
        });
      else
        require(false, [&] {
          return std::string("a terminal the logic guard cannot classify (" +
                             show(e) + ")");
        });
      break;
    case OpFamilyId::BoolOp:
      break;
    case OpFamilyId::CompareOp:
      if (!isOpX<EQ>(e) && !isOpX<NEQ>(e))
        require(L.ints || L.reals, [&] {
          return std::string("an arithmetic comparison (" + op.name() + ")");
        });
      break;
    case OpFamilyId::NumericOp: {
      if (!require(L.ints || L.reals, [&] {
            return std::string("an arithmetic operator (" + op.name() + ")");
          }))
        break;
      if (isOpX<MULT>(e)) {
        unsigned free = 0;
        for (auto *a : llvm::make_range(e->args_begin(), e->args_end()))
          free += !isArithNumeral(Expr(a));
        require(L.nonlinear || free < 2, [&] {
          return std::string("a nonlinear product (" + show(e) + ")");
        });
      } else if (isOpX<DIV>(e) || isOpX<IDIV>(e) || isOpX<MOD>(e) ||
                 isOpX<REM>(e)) {
        require(L.nonlinear || (e->arity() == 2 && isArithNumeral(e->arg(1))),
                [&] {
                  return std::string("a division by a non-numeral (" + show(e) +
                                     ")");
                });
      } else if (!isOpX<PLUS>(e) && !isOpX<MINUS>(e) && !isOpX<UN_MINUS>(e)) {
        require(false, [&] {
          return std::string("an arithmetic operator the logic guard cannot "
                             "classify (" +
                             op.name() + ")");
        });
      }
      break;
    }
    case OpFamilyId::SimpleTypeOp:
      if (isOpX<INT_TY>(e))
        require(L.ints, [&] { return std::string("the Int sort"); });
      else if (isOpX<REAL_TY>(e))
        require(L.reals, [&] { return std::string("the Real sort"); });
      else if (isOpX<ARRAY_TY>(e))
        require(L.arrays,
                [&] { return std::string("an array sort (" + show(e) + ")"); });
      else if (!isOpX<BOOL_TY>(e))
        require(false, [&] {
          return std::string("a sort the logic guard cannot classify (" +
                             op.name() + ")");
        });
      break;
    case OpFamilyId::ArrayOp:
      if (isOpX<SELECT>(e) || isOpX<STORE>(e) || isOpX<CONST_ARRAY>(e) ||
          isOpX<ARRAY_DEFAULT>(e) || isOpX<ARRAY_MAP>(e))
        require(L.arrays, [&] {
          return std::string("an array term (" + op.name() + ")");
        });
      else
        require(false, [&] {
          return std::string(
              "an array operator the logic guard cannot classify (" +
              op.name() + ")");
        });
      break;
    case OpFamilyId::BvOp:
      // the conversions between bit-vectors and Int carry an Int
      if (isOpX<INT2BV>(e) || isOpX<BV2INT>(e))
        require(L.ints, [&] {
          return std::string("an Int / bit-vector conversion (" + op.name() +
                             ")");
        });
      require(L.bv, [&] {
        return std::string("a bit-vector term (" + op.name() + ")");
      });
      // extract: its bounds are terminals of the operator, not terms
      if (isOpX<BEXTRACT>(e))
        from = to - 1;
      break;
    case OpFamilyId::BindOp:
      if (isOpX<BIND>(e)) {
        if (bv::is_bvnum(e)) {
          require(L.bv, [&] {
            return std::string("a bit-vector numeral (" + show(e) + ")");
          });
          to = 0;
        } else if (bind::isBVar(e)) {
          from = 1; // its sort
        } else
          require(false, [&] {
            return std::string("a binding the logic guard cannot classify (" +
                               show(e) + ")");
          });
      } else if (isOpX<FDECL>(e)) {
        if (bind::domainSz(e) > 0)
          require(L.uf, [&] {
            std::ostringstream os;
            os << *bind::fname(e);
            return "an uninterpreted function '" + os.str() + "' of arity " +
                   std::to_string(bind::domainSz(e));
          });
        from = 1; // the sorts, not the name
      } else if (isOpX<FAPP>(e)) {
        // a lambda's application: a select
        if (!bind::isFdecl(bind::fname(e)))
          require(L.arrays, [&] {
            return std::string("an application of a lambda (" + show(e) + ")");
          });
      } else
        require(false, [&] {
          return std::string(
              "a binding operator the logic guard cannot classify (" +
              op.name() + ")");
        });
      break;
    case OpFamilyId::BinderOp:
      if (isOpX<FORALL>(e) || isOpX<EXISTS>(e))
        require(L.quantifiers, [&] {
          return std::string("a quantifier (" + op.name() + ")");
        });
      else if (isOpX<LAMBDA>(e))
        require(L.arrays,
                [&] { return std::string("a lambda (an array term)"); });
      else
        require(false, [&] {
          return std::string("a binder the logic guard cannot classify (" +
                             op.name() + ")");
        });
      break;
    default:
      require(false, [&] {
        return std::string("an operator the logic guard cannot classify (" +
                           op.name() + ")");
      });
      break;
    }
    for (size_t i = from; i < to; ++i)
      todo.push_back(e->arg(i));
  }
  if (!bad.empty()) {
    for (const ENode *n : added)
      m_seen.erase(n);
    return bad;
  }
  // a node is remembered only while a root holding it is kept
  m_roots.push_back(root);
  return bad;
}

} // namespace sea::solver
