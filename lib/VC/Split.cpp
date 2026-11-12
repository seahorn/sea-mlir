// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Split.cpp - case splitting of a VC (sea-vc --split) ------*- C++ -*-===//

#include "sea/VC/Split.h"

#include "sea/Expr/SeaMRules.hh"
#include "sea/Interp/Interpreter.h"
#include "sea/VC/Names.h"
#include "sea/VC/OpSem.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <sstream>

using namespace mlir;
using namespace expr;

namespace sea {

static bool fileMatches(StringRef path, StringRef file) {
  if (!path.ends_with(file))
    return false;
  // a whole path component: `a.h` does not match `data.h`
  return path.size() == file.size() ||
         path[path.size() - file.size() - 1] == '/' || file.starts_with("/");
}

LogicalResult resolveSplitTargets(ProgramOp program,
                                  ArrayRef<std::string> selectors,
                                  SplitTarget::Kind kind,
                                  std::vector<SplitTarget> &out,
                                  std::string &error) {
  ValueNames names(program);
  const bool isBool = kind == SplitTarget::Kind::Bool;
  auto kindOk = [&](Value v) {
    if (isBool)
      return v.getType().isInteger(1);
    return isa<AddrType>(v.getType());
  };
  for (const std::string &sel : selectors) {
    std::vector<SplitTarget> found;
    auto addValue = [&](Value v) {
      found.push_back({kind, v, nullptr, names.name(v).str(), sel});
    };
    StringRef s(sel);
    if (s.starts_with("@")) {
      auto [fileText, lineText] = s.drop_front().rsplit(':');
      StringRef file = fileText;
      unsigned line = 0;
      if (file.empty() || lineText.getAsInteger(10, line)) {
        error = "split selector '" + sel + "': want @<file>:<line>";
        return failure();
      }
      program.walk([&](Operation *op) {
        for (Value r : op->getResults()) {
          if (!kindOk(r))
            continue;
          for (FileLineColLoc p : positionsOf(op->getLoc()))
            if (p.getLine() == line &&
                fileMatches(p.getFilename().getValue(), file)) {
              addValue(r);
              break;
            }
        }
      });
    } else if (s.starts_with("^")) {
      if (!isBool) {
        error = "split selector '" + sel +
                "': a block guard is Boolean (--split, not --split-leaves)";
        return failure();
      }
      for (Block &b : program.getBody())
        if (names.name(&b).getValue() == s) {
          if (&b == &program.getBody().front()) {
            error = "split selector '" + sel +
                    "': the entry block always executes (no guard to split)";
            return failure();
          }
          found.push_back({kind, Value(), &b, s.str(), sel});
        }
    } else {
      program.walk([&](Operation *op) {
        for (Value r : op->getResults())
          if (names.name(r).getValue() == s)
            addValue(r);
      });
      for (Block &b : program.getBody())
        for (BlockArgument a : b.getArguments())
          if (names.name(a).getValue() == s)
            addValue(a);
      for (SplitTarget &t : found)
        if (!kindOk(t.value)) {
          std::string ty;
          llvm::raw_string_ostream os(ty);
          os << t.value.getType();
          error = "split selector '" + sel + "': " + t.name + " has type " +
                  ty + (isBool ? ", not i1" : ", not an address");
          return failure();
        }
    }
    if (found.empty()) {
      error = "split selector '" + sel + "' finds no " +
              (isBool ? "i1 value or block" : "address value") +
              " in the encoded program (names are those of the lowered "
              "program)";
      return failure();
    }
    if (found.size() > 1) {
      error = "split selector '" + sel + "' is ambiguous (" +
              std::to_string(found.size()) + " matches:";
      for (size_t i = 0; i < found.size() && i < 6; ++i)
        error += " " + found[i].name;
      error += found.size() > 6 ? " ...)" : ")";
      return failure();
    }
    for (const SplitTarget &t : out)
      if (t.value == found[0].value && t.block == found[0].block) {
        error = "split selector '" + sel + "' names " + t.name +
                " again (selector '" + t.selector + "')";
        return failure();
      }
    out.push_back(found[0]);
  }
  return success();
}

bool SplitRun::leafCases(Expr t, SeaOpSem &sem, unsigned maxPaths,
                         std::vector<SplitCase> &out, std::string &why) {
  ExprFactory &efac = t->efac();
  const bool fromBits = isOpX<PTR_FROM_BITS>(t);
  Expr tree = fromBits ? Expr(t->arg(0)) : t;
  if (!isOpX<ITE>(tree)) {
    std::ostringstream os;
    os << "the term is " << tree->op().name()
       << (fromBits ? " under ptr-from-bits" : "")
       << ", not an ite (or ptr-from-bits of an ite)";
    why = os.str();
    return false;
  }
  // the leaves in first-occurrence order, each with its path conditions
  std::vector<Expr> leaves;
  std::map<ENode *, ExprVector> paths;
  unsigned npaths = 0;
  struct Item {
    Expr e;
    ExprVector lits;
  };
  std::vector<Item> stack{{tree, {}}};
  while (!stack.empty()) {
    Item it = std::move(stack.back());
    stack.pop_back();
    if (isOpX<ITE>(it.e)) {
      Item els{Expr(it.e->arg(2)), it.lits};
      els.lits.push_back(op::boolop::lneg(Expr(it.e->arg(0))));
      it.lits.push_back(Expr(it.e->arg(0)));
      // then-side first (pushed last)
      stack.push_back(std::move(els));
      stack.push_back({Expr(it.e->arg(1)), std::move(it.lits)});
      continue;
    }
    if (++npaths > maxPaths) {
      why = "the ite tree has more than " + std::to_string(maxPaths) +
            " paths (--split-max-paths)";
      return false;
    }
    auto [p, fresh] = paths.try_emplace(it.e.get());
    if (fresh)
      leaves.push_back(it.e);
    p->second.push_back(op::boolop::land(it.lits, efac));
  }
  expr::SeaMRewriter *rw = sem.seamRewriter();
  for (const Expr &l : leaves) {
    SplitCase c;
    c.cond = op::boolop::lor(paths[l.get()], efac);
    c.value = fromBits ? Expr(mk<PTR_FROM_BITS>(l)) : l;
    // from-bits(to-bits p) and the like: the rewriter's normal form, as
    // for any term the encoding builds
    if (rw && fromBits)
      c.value = rw->rewrite(c.value);
    std::string text = "leaf";
    if (rw)
      if (auto v = rw->rules().view(c.value)) {
        std::ostringstream os;
        os << *v->base;
        text += ":" + os.str();
      }
    if (text == "leaf")
      text += ":unknown(" + std::string(c.value->op().name()) + ")";
    text += "/paths=" + std::to_string(paths[l.get()].size());
    c.text = text;
    out.push_back(std::move(c));
  }
  return true;
}

unsigned SplitRun::choose(unsigned level, unsigned n) {
  unsigned c = level < prefix.size() ? prefix[level] : 0;
  if (c >= n && why.empty())
    why = "level " + std::to_string(level) + " has " + std::to_string(n) +
          " case(s) in this run but case " + std::to_string(c) +
          " was planned (the encoding is not deterministic)";
  return c < n ? c : 0;
}

void SplitRun::onValue(Value v, SeaOpSem &sem, OpSemContext &ctx) {
  auto it = byValue.find(v);
  if (it == byValue.end())
    return;
  const SplitTarget &t = targets[it->second];
  Expr reg = sem.mkSymbReg(v);
  Expr term = ctx.read(reg);
  SplitLevel lvl;
  lvl.target = it->second;
  lvl.term = term;
  lvl.ptr = isa<AddrType>(v.getType()) && sem.ptrSorted();
  if (t.kind == SplitTarget::Kind::Bool) {
    for (unsigned b : {1u, 0u}) {
      Expr num = sem.numeral(llvm::APInt(1, b));
      lvl.cases.push_back({num, mk<EQ>(term, num), b ? "1" : "0"});
    }
  } else {
    std::string w;
    if (!leafCases(term, sem, maxTreePaths, lvl.cases, w)) {
      if (why.empty())
        why = t.name + " (" + t.selector + "): " + w;
      return;
    }
  }
  if (plant == Plant::DropCase && lvl.cases.size() > 1)
    lvl.cases.pop_back();
  lvl.chosen = choose(lvls.size(), lvl.cases.size());
  if (plant == Plant::WrongValue && lvl.cases.size() > 1)
    lvl.cases[lvl.chosen].value =
        lvl.cases[(lvl.chosen + 1) % lvl.cases.size()].value;
  ctx.values().write(reg, lvl.cases[lvl.chosen].value);
  lvls.push_back(std::move(lvl));
}

Expr SplitRun::onGuard(Block *b, Expr g, OpSemContext &ctx) {
  auto it = byBlock.find(b);
  if (it == byBlock.end())
    return g;
  ExprFactory &efac = ctx.efac();
  SplitLevel lvl;
  lvl.target = it->second;
  lvl.term = g;
  lvl.cases.push_back({mk<TRUE>(efac), g, "true"});
  lvl.cases.push_back({mk<FALSE>(efac), op::boolop::lneg(g), "false"});
  lvl.chosen = choose(lvls.size(), 2);
  Expr v = lvl.cases[lvl.chosen].value;
  lvls.push_back(std::move(lvl));
  return v;
}

void SplitRun::install(SeaOpSem &sem, OpSemContext &ctx) {
  ctx.defHook = [this, &sem, &ctx](Value v) { onValue(v, sem, ctx); };
  ctx.guardHook = [this, &ctx](Block *b, Expr g) { return onGuard(b, g, ctx); };
}

Expr SplitRun::partCondition(ExprFactory &efac) const {
  if (lvls.empty())
    return Expr();
  ExprVector conj;
  for (const SplitLevel &l : lvls)
    if (l.cases.size() > 1)
      conj.push_back(l.cases[l.chosen].cond);
  return op::boolop::land(conj, efac);
}

std::string SplitRun::choiceText() const {
  std::string s;
  for (const SplitLevel &l : lvls) {
    if (!s.empty())
      s += ",";
    s += targets[l.target].name + "=" + l.cases[l.chosen].text + "#" +
         std::to_string(l.chosen) + "/" + std::to_string(l.cases.size());
  }
  return s.empty() ? "-" : s;
}

} // namespace sea
