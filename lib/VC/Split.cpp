// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Split.cpp - case splitting of a VC (sea-vc --split) ------*- C++ -*-===//

#include "sea/VC/Split.h"

#include "sea/Expr/SeaMRules.hh"
#include "sea/Interp/Interpreter.h"
#include "sea/Support/Diag.h"
#include "sea/VC/Names.h"
#include "sea/VC/OpSem.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace mlir;
using namespace expr;

namespace sea {

bool isLeavesType(Type t) {
  if (isa<AddrType>(t))
    return true;
  auto i = dyn_cast<IntegerType>(t);
  return i && i.getWidth() > 1;
}

static bool fileMatches(StringRef path, StringRef file) {
  if (!path.ends_with(file))
    return false;
  // a whole path component: `a.h` does not match `data.h`
  return path.size() == file.size() ||
         path[path.size() - file.size() - 1] == '/' || file.starts_with("/");
}

/// `<name> <op> <type>[ col=<c>][ <file:line:col>]` of a value, for the
/// selector errors: enough to tell the matches of one line apart.
static std::string describeMatch(Value v, StringRef name,
                                 std::optional<FileLineColLoc> at,
                                 bool withPos) {
  std::string text;
  llvm::raw_string_ostream os(text);
  os << name << " ";
  if (Operation *def = v.getDefiningOp())
    os << def->getName().getStringRef();
  else
    os << "block-arg";
  os << " " << v.getType();
  if (at) {
    if (withPos)
      os << " at=" << fileLocText(*at);
    else
      os << " col=" << at->getColumn();
  }
  return text;
}

/// The longest list of matches an error shows (the rest are counted).
static constexpr size_t kMaxListed = 32;

static const char *kNamesHint =
    " (names are those of the lowered program: sea-vc --print-lowered=<file> "
    "writes it)";

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
    return isLeavesType(v.getType());
  };
  const char *kindText = isBool ? "i1 value" : "address or integer value";
  for (const std::string &sel : selectors) {
    std::vector<SplitTarget> found;
    // per match: the position the selector matched (@file:line)
    std::vector<std::optional<FileLineColLoc>> foundAt;
    auto addValue = [&](Value v, std::optional<FileLineColLoc> at) {
      found.push_back({kind, v, nullptr, names.name(v).str(), sel});
      foundAt.push_back(at);
    };
    // @file:line that finds nothing: the results of other types on the
    // line, and the nearest lines of the file with results of the kind
    std::vector<std::pair<Value, FileLineColLoc>> otherOnLine;
    std::map<unsigned, unsigned> kindLines;
    std::set<std::string> files;
    bool fileSeen = false, moreFiles = false;
    // @file:line[:col]: the parsed parts (for the errors)
    StringRef file;
    unsigned line = 0, col = 0;
    StringRef s(sel);
    if (s.starts_with("@")) {
      auto [fileText, lineText] = s.drop_front().rsplit(':');
      file = fileText;
      // @<file>:<line>:<col>: the last two components are numbers
      {
        auto [f2, l2] = fileText.rsplit(':');
        unsigned ln = 0;
        if (!l2.empty() && f2 != fileText && !l2.getAsInteger(10, ln) &&
            !f2.empty()) {
          file = f2;
          line = ln;
          if (lineText.getAsInteger(10, col) || col == 0) {
            error = "split selector '" + sel + "': want @<file>:<line>[:<col>]";
            return failure();
          }
        }
      }
      if (file.empty() || (!col && lineText.getAsInteger(10, line))) {
        error = "split selector '" + sel + "': want @<file>:<line>[:<col>]";
        return failure();
      }
      // the matches by class, in order of preference: --split-leaves:
      // address op results, address block arguments, integer op results,
      // integer block arguments; --split: op results, block arguments. The
      // first non-empty class is the selector's (a line that named one
      // address before integers and block arguments were targets still
      // names it)
      std::vector<std::pair<Value, FileLineColLoc>> byClass[4];
      auto classOf = [&](Value r) {
        unsigned c = isa<BlockArgument>(r) ? 1 : 0;
        if (!isBool && !isa<AddrType>(r.getType()))
          c += 2;
        return c;
      };
      // an op result or a block argument (a join's merged value) with a
      // position on the line (and column)
      auto visit = [&](Value r, Location loc) {
        bool matched = false;
        for (FileLineColLoc p : positionsOf(loc)) {
          StringRef path = p.getFilename().getValue();
          if (!fileMatches(path, file)) {
            // the first 8 distinct files, and whether there are more
            if (files.size() < 8)
              files.insert(path.str());
            else if (!files.count(path.str()))
              moreFiles = true;
            continue;
          }
          fileSeen = true;
          if (!kindOk(r)) {
            // once per result (it may have two positions on the line)
            if (p.getLine() == line && !matched) {
              otherOnLine.push_back({r, p});
              matched = true;
            }
            continue;
          }
          ++kindLines[p.getLine()];
          if (p.getLine() == line && (!col || p.getColumn() == col) &&
              !matched) {
            byClass[classOf(r)].push_back({r, p});
            matched = true;
          }
        }
      };
      program.walk([&](Block *b) {
        for (BlockArgument a : b->getArguments())
          visit(a, a.getLoc());
      });
      program.walk([&](Operation *op) {
        for (Value r : op->getResults())
          visit(r, op->getLoc());
      });
      for (auto &cls : byClass)
        if (!cls.empty()) {
          for (auto &[r, p] : cls)
            addValue(r, p);
          break;
        }
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
          foundAt.push_back(std::nullopt);
        }
    } else {
      program.walk([&](Operation *op) {
        for (Value r : op->getResults())
          if (names.name(r).getValue() == s)
            addValue(r, fileLocOf(op->getLoc()));
      });
      for (Block &b : program.getBody())
        for (BlockArgument a : b.getArguments())
          if (names.name(a).getValue() == s)
            addValue(a, fileLocOf(a.getLoc()));
      for (SplitTarget &t : found)
        if (!kindOk(t.value)) {
          std::string ty;
          llvm::raw_string_ostream os(ty);
          os << t.value.getType();
          error = "split selector '" + sel + "': " + t.name + " has type " +
                  ty +
                  (isBool ? ", not i1"
                          : ", not an address or an integer wider than i1");
          return failure();
        }
    }
    if (found.empty()) {
      error = "split selector '" + sel + "' finds no " +
              (isBool ? "i1 value or block"
                      : "address or integer (iN, N > 1) value") +
              " in the encoded program" + kNamesHint;
      if (s.starts_with("@")) {
        if (!fileSeen) {
          error += "\n  no op has a position in a file ending with '" +
                   file.str() + "'";
          if (!files.empty()) {
            error += " (files:";
            for (const std::string &f : files)
              error += " " + f;
            error += moreFiles ? " ...)" : ")";
          }
        }
        if (!otherOnLine.empty()) {
          error += "\n  on line " + std::to_string(line) + ", not of the kind:";
          for (size_t i = 0; i < otherOnLine.size() && i < kMaxListed; ++i)
            error += std::string(i ? ";" : "") + " " +
                     describeMatch(otherOnLine[i].first,
                                   names.name(otherOnLine[i].first),
                                   otherOnLine[i].second, false);
          if (otherOnLine.size() > kMaxListed)
            error += "; ... and " +
                     std::to_string(otherOnLine.size() - kMaxListed) + " more";
        }
        if (!kindLines.empty()) {
          // the three nearest lines on each side
          std::vector<std::pair<unsigned, unsigned>> near;
          auto hi = kindLines.lower_bound(line);
          auto lo = hi;
          for (unsigned k = 0; k < 3 && lo != kindLines.begin(); ++k)
            near.push_back(*--lo);
          for (unsigned k = 0; k < 3 && hi != kindLines.end(); ++k, ++hi)
            near.push_back(*hi);
          llvm::sort(near);
          error += std::string("\n  nearest lines with ") + kindText +
                   "s (line:count):";
          for (auto [l, n] : near)
            error += " " + std::to_string(l) + ":" + std::to_string(n);
        }
      }
      return failure();
    }
    if (found.size() > 1) {
      error = "split selector '" + sel + "' is ambiguous (" +
              std::to_string(found.size()) +
              " matches; pick one by name, or by @<file>:<line>:<col>)" +
              kNamesHint + ":";
      for (size_t i = 0; i < found.size() && i < kMaxListed; ++i)
        error +=
            "\n  " + (found[i].block
                          ? found[i].name + " block"
                          : describeMatch(found[i].value, found[i].name,
                                          foundAt[i], !s.starts_with("@")));
      if (found.size() > kMaxListed)
        error += "\n  ... and " + std::to_string(found.size() - kMaxListed) +
                 " more";
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
                         std::vector<SplitCase> &out, std::string &why,
                         bool isAddr) {
  ExprFactory &efac = t->efac();
  const bool fromBits = isOpX<PTR_FROM_BITS>(t);
  Expr tree = fromBits ? Expr(t->arg(0)) : t;
  if (!isOpX<ITE>(tree)) {
    std::ostringstream os;
    os << "the term is " << tree->op().name()
       << (fromBits ? " under ptr-from-bits" : "")
       << (isAddr ? ", not an ite (or ptr-from-bits of an ite)"
                  : ", not an ite");
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
    unsigned w = 0;
    if (!isAddr) {
      // an integer leaf: its numeral (unsigned), or the leaf's operator
      if (op::bv::isBvNum(c.value, w)) {
        text += ":" + op::bv::toMpz(c.value).to_string();
      } else if (bind::isFapp(c.value) && c.value->arity() == 1) {
        // a constant (a nondet value): its name
        std::ostringstream os;
        os << *c.value;
        text += ":" + os.str();
      } else
        text += ":term(" + std::string(c.value->op().name()) + ")";
    } else if (rw)
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

/// `v` is a leaf of the ite tree of `t` (under at most one
/// PTR_FROM_BITS), or the rewriter's normal form of ptr-from-bits of one.
static bool isLeafValue(Expr t, Expr v, SeaOpSem &sem) {
  const bool fromBits = isOpX<PTR_FROM_BITS>(t);
  Expr tree = fromBits ? Expr(t->arg(0)) : t;
  expr::SeaMRewriter *rw = sem.seamRewriter();
  std::set<ENode *> seen;
  std::vector<Expr> stack{tree};
  while (!stack.empty()) {
    Expr e = stack.back();
    stack.pop_back();
    if (!seen.insert(e.get()).second)
      continue;
    if (isOpX<ITE>(e)) {
      stack.push_back(Expr(e->arg(1)));
      stack.push_back(Expr(e->arg(2)));
      continue;
    }
    if (e == v)
      return true;
    if (fromBits) {
      Expr w = Expr(mk<PTR_FROM_BITS>(e));
      if (w == v || (rw && rw->rewrite(w) == v))
        return true;
    }
  }
  return false;
}

unsigned SplitRun::choose(unsigned level, unsigned n) {
  unsigned c = level < prefix.size() ? prefix[level] : 0;
  if (c >= n && why.empty()) {
    isUnstable = true;
    why = "level " + std::to_string(level) + " has " + std::to_string(n) +
          " case(s) in this run but case " + std::to_string(c) +
          " was planned (the encoding is not deterministic)";
  }
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
  const bool isAddr = isa<AddrType>(v.getType());
  lvl.ptr = isAddr && sem.ptrSorted();
  if (t.kind == SplitTarget::Kind::Bool) {
    for (unsigned b : {1u, 0u}) {
      Expr num = sem.numeral(llvm::APInt(1, b));
      lvl.cases.push_back({num, mk<EQ>(term, num), b ? "1" : "0"});
    }
  } else {
    std::string w;
    if (!leafCases(term, sem, maxTreePaths, lvl.cases, w, isAddr)) {
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
  if (t.kind == SplitTarget::Kind::Leaves) {
    // (X) at the pointer level, by construction: the value is a leaf of the
    // term's ite tree, or the rewriter's normal form of ptr-from-bits of one
    if (!isLeafValue(term, lvl.cases[lvl.chosen].value, sem)) {
      if (why.empty())
        why = t.name + " (" + t.selector +
              "): the chosen value is not a leaf of the term's ite tree "
              "(a bug in the case split construction)";
      return;
    }
    ++nStructural;
  }
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

void SplitCensus::install(SeaOpSem &sem, OpSemContext &ctx) {
  ctx.defHook = [this, &sem, &ctx](Value v) {
    if (isa<AddrType>(v.getType())) {
      Expr t = ctx.read(sem.mkSymbReg(v));
      Expr tree = isOpX<PTR_FROM_BITS>(t) ? Expr(t->arg(0)) : t;
      if (isOpX<ITE>(tree)) {
        SplitAddrCandidate c;
        c.value = v;
        c.term = t;
        addrs.push_back(std::move(c));
      }
      return;
    }
    if (!bools)
      return;
    if (isLeavesType(v.getType())) {
      Expr t = ctx.read(sem.mkSymbReg(v));
      if (isOpX<ITE>(t)) {
        SplitIntCandidate c;
        c.value = v;
        c.term = t;
        ints.push_back(std::move(c));
      }
      return;
    }
    if (!v.getType().isInteger(1))
      return;
    Expr t = ctx.read(sem.mkSymbReg(v));
    // a constant has nothing to split
    if (op::bv::isBvNum(t) || isOpX<TRUE>(t) || isOpX<FALSE>(t))
      return;
    SplitBoolCandidate c;
    c.value = v;
    c.keys = {t, sem.toBool(t)};
    boolCands.push_back(std::move(c));
  };
  if (bools)
    ctx.guardHook = [this](Block *b, Expr g) {
      SplitBoolCandidate c;
      c.block = b;
      c.keys = {g};
      boolCands.push_back(std::move(c));
      return g;
    };
}

namespace {
/// Per DAG node, the sorted ids collected from the node and its DAG
/// (iterative post-order, memoized): `own` adds the node's own ids.
class DagIds {
public:
  using Own = std::function<void(ENode *, std::vector<unsigned> &)>;
  explicit DagIds(Own own) : own(std::move(own)) {}
  const std::vector<unsigned> &of(ENode *root) {
    std::vector<std::pair<ENode *, bool>> st{{root, false}};
    while (!st.empty()) {
      auto [n, done] = st.back();
      st.pop_back();
      if (memo.count(n))
        continue;
      if (!done) {
        st.push_back({n, true});
        for (size_t i = 0; i < n->arity(); ++i)
          if (!memo.count(n->arg(i)))
            st.push_back({n->arg(i), false});
        continue;
      }
      std::vector<unsigned> ids, u;
      own(n, ids);
      llvm::sort(ids);
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
      // the arguments' sets are sorted: merge (linear), no sort
      for (size_t i = 0; i < n->arity(); ++i) {
        const std::vector<unsigned> &a = memo[n->arg(i)];
        if (a.empty())
          continue;
        if (ids.empty()) {
          ids = a;
          continue;
        }
        u.clear();
        std::set_union(ids.begin(), ids.end(), a.begin(), a.end(),
                       std::back_inserter(u));
        ids.swap(u);
      }
      memo[n] = std::move(ids);
    }
    return memo[root];
  }

private:
  Own own;
  // node-based: references stay valid as it grows
  std::unordered_map<ENode *, std::vector<unsigned>> memo;
};

DagIds::Own idsOf(const std::unordered_multimap<ENode *, unsigned> &m) {
  return [&m](ENode *n, std::vector<unsigned> &ids) {
    auto [lo, hi] = m.equal_range(n);
    for (auto it = lo; it != hi; ++it)
      ids.push_back(it->second);
  };
}

/// A Boolean connective, or its bv1 form (from a Boolean or bv1 root, the
/// arguments are Boolean or bv1 too).
bool isConnective(ENode *n) {
  auto bit1 = [](ENode *e) {
    unsigned w = 0;
    return op::bv::isBvNum(Expr(e), w) && w == 1;
  };
  return isOpX<AND>(n) || isOpX<OR>(n) || isOpX<XOR>(n) || isOpX<NEG>(n) ||
         isOpX<IMPL>(n) || isOpX<IFF>(n) || isOpX<ITE>(n) || isOpX<BAND>(n) ||
         isOpX<BOR>(n) || isOpX<BXOR>(n) || isOpX<BNOT>(n) ||
         (isOpX<EQ>(n) && (bit1(n->arg(0)) || bit1(n->arg(1))));
}

/// The atoms of a Boolean (or bv1) formula: its subterms below the
/// connectives (isConnective), numerals excluded; false when there are more
/// than `max`.
bool atomsOf(ENode *f, size_t max, std::set<ENode *> &out) {
  std::set<ENode *> seen;
  std::vector<ENode *> st{f};
  while (!st.empty()) {
    ENode *n = st.back();
    st.pop_back();
    if (!seen.insert(n).second)
      continue;
    if (isConnective(n)) {
      for (size_t i = 0; i < n->arity(); ++i)
        st.push_back(n->arg(i));
      continue;
    }
    if (op::bv::isBvNum(Expr(n)) || isOpX<TRUE>(n) || isOpX<FALSE>(n))
      continue;
    out.insert(n);
    if (out.size() > max)
      return false;
  }
  return true;
}

void unite(std::vector<unsigned> &into, const std::vector<unsigned> &b) {
  if (b.empty())
    return;
  if (into.empty()) {
    into = b;
    return;
  }
  std::vector<unsigned> u;
  std::set_union(into.begin(), into.end(), b.begin(), b.end(),
                 std::back_inserter(u));
  into.swap(u);
}
} // namespace

void SplitCensus::attribute(SeaOpSem &sem, ArrayRef<ENode *> openOverlaps,
                            unsigned maxPaths) {
  // an address candidate is behind a question whose pointers contain its
  // term or one of its leaves without an object (SeaM's leaf pairs split
  // an ite pointer: the open pair holds the unknown leaf, not the ite)
  // (by their normal forms: the query was normalized after them)
  std::unordered_multimap<ENode *, unsigned> candOf;
  ExprVector keep;
  std::vector<Expr> normTerm(addrs.size());
  auto addCand = [&](const Expr &e, unsigned i) {
    keep.push_back(sem.simplify(e));
    candOf.emplace(keep.back().get(), i);
  };
  for (unsigned i = 0; i < addrs.size(); ++i) {
    SplitAddrCandidate &c = addrs[i];
    addCand(c.term, i);
    normTerm[i] = keep.back();
    std::string why;
    if (!SplitRun::leafCases(c.term, sem, maxPaths, c.cases, why)) {
      // a candidate without cases: listed with leaves=0, never picked
      c.cases.clear();
      diag::warn("vc.split-max-paths",
                 "a split candidate (an ite-valued address) is counted "
                 "without its leaves (leaves=0, unknown-leaves=0; only its "
                 "term attributes questions): " +
                     why);
    }
    for (const SplitCase &k : c.cases)
      if (!sem.seamRewriter()->rules().view(k.value)) {
        addCand(k.value, i);
        ++c.unknownLeaves;
      }
  }
  DagIds addrIds([&](ENode *n, std::vector<unsigned> &ids) {
    auto [lo, hi] = candOf.equal_range(n);
    for (auto it = lo; it != hi; ++it)
      ids.push_back(it->second);
  });
  // the term alone (not its leaves): the question holds the ite itself
  std::unordered_multimap<ENode *, unsigned> termOf;
  for (unsigned i = 0; i < addrs.size(); ++i)
    termOf.emplace(normTerm[i].get(), i);
  DagIds termIds([&](ENode *n, std::vector<unsigned> &ids) {
    auto [lo, hi] = termOf.equal_range(n);
    for (auto it = lo; it != hi; ++it)
      ids.push_back(it->second);
  });
  // the Boolean candidates in each node's DAG, and in the ite conditions
  // of each node's DAG
  std::unordered_multimap<ENode *, unsigned> boolOf;
  for (unsigned j = 0; j < boolCands.size(); ++j)
    for (const Expr &k : boolCands[j].keys) {
      keep.push_back(sem.simplify(k));
      boolOf.emplace(keep.back().get(), j);
    }
  DagIds boolIds([&](ENode *n, std::vector<unsigned> &ids) {
    auto [lo, hi] = boolOf.equal_range(n);
    for (auto it = lo; it != hi; ++it)
      ids.push_back(it->second);
  });
  DagIds condIds([&](ENode *n, std::vector<unsigned> &ids) {
    if (isOpX<ITE>(n)) {
      const std::vector<unsigned> &b = boolIds.of(n->arg(0));
      ids.insert(ids.end(), b.begin(), b.end());
    }
  });
  std::vector<std::vector<unsigned>> termBools(addrs.size());
  if (bools && !boolCands.empty())
    for (unsigned i = 0; i < addrs.size(); ++i) {
      termBools[i] = condIds.of(normTerm[i].get());
      for (unsigned j : termBools[i])
        ++boolCands[j].addrs;
    }
  for (ENode *n : openOverlaps) {
    std::vector<unsigned> ids = addrIds.of(n->arg(0));
    unite(ids, addrIds.of(n->arg(2)));
    for (unsigned i : ids) {
      ++addrs[i].open;
      if (ids.size() == 1)
        ++addrs[i].own;
    }
    if (direct) {
      std::vector<unsigned> ts = termIds.of(n->arg(0));
      unite(ts, termIds.of(n->arg(2)));
      for (unsigned i : ts)
        ++addrs[i].direct;
    }
    if (!bools || boolCands.empty())
      continue;
    std::vector<unsigned> bs = condIds.of(n->arg(0));
    unite(bs, condIds.of(n->arg(2)));
    for (unsigned i : ids)
      unite(bs, termBools[i]);
    for (unsigned j : bs)
      ++boolCands[j].open;
  }
}

uint64_t SplitCensus::attributeReads(SeaOpSem &sem, const Expr &solverQuery) {
  // the keys in the solver's terms: normalized as the query was, then
  // through the boundary (compositional: a subterm of the query translates
  // to the translation of the subterm)
  ExprVector keep;
  auto solverKey = [&](const Expr &k) -> ENode * {
    std::string why;
    Expr t = sem.solverTerm(sem.simplify(k), why);
    if (!t)
      return nullptr;
    keep.push_back(t);
    return t.get();
  };
  std::unordered_multimap<ENode *, unsigned> addrOf, boolOf;
  for (unsigned i = 0; i < addrs.size(); ++i)
    if (ENode *n = solverKey(addrs[i].term))
      addrOf.emplace(n, i);
  for (unsigned j = 0; j < boolCands.size(); ++j)
    for (const Expr &k : boolCands[j].keys)
      if (ENode *n = solverKey(k))
        boolOf.emplace(n, j);
  DagIds addrIds(idsOf(addrOf));
  DagIds boolIds(idsOf(boolOf));
  DagIds condIds([&](ENode *n, std::vector<unsigned> &ids) {
    if (isOpX<ITE>(n)) {
      const std::vector<unsigned> &b = boolIds.of(n->arg(0));
      ids.insert(ids.end(), b.begin(), b.end());
    }
  });
  // the candidates that are an ite condition themselves (not a part of one)
  DagIds exactIds([&](ENode *n, std::vector<unsigned> &ids) {
    if (isOpX<ITE>(n)) {
      auto [lo, hi] = boolOf.equal_range(n->arg(0));
      for (auto it = lo; it != hi; ++it)
        ids.push_back(it->second);
    }
  });
  // candidates with the same term (two ops compute one condition): listed
  // together, by the one with the most uses that reach a select condition,
  // a branch condition or an address (forward through i1 connectives: and,
  // or, xor, i1 select, i1 block arguments), then the first: a split
  // replaces the value of that op only
  if (bools) {
    auto selectUses = [](const SplitBoolCandidate &c) {
      unsigned n = 0;
      if (!c.value)
        return n;
      llvm::DenseSet<Value> seen;
      std::vector<Value> work{c.value};
      auto follow = [&](Value v) {
        if (v.getType().isInteger(1) && seen.insert(v).second)
          work.push_back(v);
      };
      while (!work.empty()) {
        Value v = work.back();
        work.pop_back();
        for (OpOperand &u : v.getUses()) {
          Operation *op = u.getOwner();
          if (auto sel = dyn_cast<SelectOp>(op)) {
            if (sel.getType().isInteger(1))
              follow(sel.getResult());
            else if (u.getOperandNumber() == 0)
              ++n;
          } else if (isa<AndOp, OrOp, XorOp>(op)) {
            follow(op->getResult(0));
          } else if (auto cb = dyn_cast<CondBrOp>(op);
                     cb && u.getOperandNumber() == 0) {
            ++n;
          } else if (auto br = dyn_cast<BranchOpInterface>(op)) {
            if (std::optional<BlockArgument> a =
                    br.getSuccessorBlockArgument(u.getOperandNumber()))
              follow(*a);
          } else if (llvm::any_of(op->getResultTypes(),
                                  [](Type t) { return isa<AddrType>(t); })) {
            ++n;
          }
        }
      }
      return n;
    };
    std::unordered_map<ENode *, unsigned> repOf;
    std::vector<unsigned> uses(boolCands.size());
    for (unsigned j = 0; j < boolCands.size(); ++j) {
      uses[j] = selectUses(boolCands[j]);
      if (ENode *n = solverKey(boolCands[j].keys.front())) {
        auto [it, fresh] = repOf.try_emplace(n, j);
        if (!fresh && uses[j] > uses[it->second])
          it->second = j;
      }
    }
    for (unsigned j = 0; j < boolCands.size(); ++j)
      if (ENode *n = solverKey(boolCands[j].keys.front()))
        if (repOf[n] != j)
          boolCands[j].sameAs = repOf[n];
  }
  // the other candidates in a candidate's Boolean structure (an and of two
  // compares holds both; not what its atoms read), each term once
  if (bools)
    for (unsigned j = 0; j < boolCands.size(); ++j) {
      std::set<unsigned> in;
      std::set<ENode *> seen;
      std::vector<ENode *> st;
      for (const Expr &k : boolCands[j].keys)
        if (ENode *n = solverKey(k))
          st.push_back(n);
      while (!st.empty()) {
        ENode *n = st.back();
        st.pop_back();
        if (!seen.insert(n).second)
          continue;
        auto [lo, hi] = boolOf.equal_range(n);
        for (auto it = lo; it != hi; ++it) {
          unsigned i = it->second;
          int first = boolCands[i].sameAs < 0 ? int(i) : boolCands[i].sameAs;
          int self = boolCands[j].sameAs < 0 ? int(j) : boolCands[j].sameAs;
          if (first != self)
            in.insert(first);
        }
        // Boolean connectives, and their bv1 forms (from a Boolean or bv1
        // root, the arguments are Boolean or bv1 too)
        if (isConnective(n))
          for (size_t i = 0; i < n->arity(); ++i)
            st.push_back(n->arg(i));
      }
      boolCands[j].covers = in.size();
    }
  // every distinct read
  uint64_t reads = 0;
  std::unordered_set<ENode *> seen;
  std::vector<ENode *> stack{solverQuery.get()};
  while (!stack.empty()) {
    ENode *n = stack.back();
    stack.pop_back();
    if (!seen.insert(n).second)
      continue;
    for (size_t i = 0; i < n->arity(); ++i)
      stack.push_back(n->arg(i));
    if (!isOpX<SELECT>(n) || op::bv::isBvNum(Expr(n->arg(1))))
      continue;
    ++reads;
    for (unsigned i : addrIds.of(n->arg(1)))
      ++addrs[i].reads;
    if (bools) {
      for (unsigned j : condIds.of(n->arg(1)))
        ++boolCands[j].reads;
      for (unsigned j : exactIds.of(n->arg(1)))
        ++boolCands[j].condReads;
    }
  }
  return reads;
}

/// The most atoms of a condition the arithmetic attribution compares (an
/// untuned bound on its cost): a larger condition attributes nothing.
static constexpr size_t kMaxArithAtoms = 64;

ArithKind arithKind(const ENode *n) {
  if (isOpX<BMUL>(n) || isOpX<SMUL_NO_OVERFLOW>(n) ||
      isOpX<UMUL_NO_OVERFLOW>(n) || isOpX<SMUL_NO_UNDERFLOW>(n))
    return ArithKind::Mul;
  if (isOpX<BUDIV>(n) || isOpX<BSDIV>(n) || isOpX<BUREM>(n) ||
      isOpX<BSREM>(n) || isOpX<BSMOD>(n))
    return ArithKind::Div;
  return ArithKind::None;
}

uint64_t SplitCensus::attributeArith(SeaOpSem &sem, const Expr &solverQuery,
                                     unsigned maxPaths) {
  // the keys in the solver's terms, as attributeReads makes them
  ExprVector keep;
  auto solverKey = [&](const Expr &k) -> ENode * {
    std::string why;
    Expr t = sem.solverTerm(sem.simplify(k), why);
    if (!t)
      return nullptr;
    keep.push_back(t);
    return t.get();
  };
  std::unordered_multimap<ENode *, unsigned> intOf, boolOf;
  // per atom, the integer candidates with it in a condition of their tree
  // (in the solver's terms), and each candidate's number of atoms
  std::unordered_map<ENode *, std::vector<unsigned>> intsOfAtom;
  // the atoms of the conditions of an ite spine, memoized over the DAG (a
  // chain of merges shares its spine): sorted, at most kMaxArithAtoms
  // (more: none, `over`)
  struct Atoms {
    std::vector<ENode *> atoms;
    bool over = false;
  };
  std::unordered_map<ENode *, Atoms> spineMemo;
  auto spineOf = [&](ENode *root) -> const Atoms & {
    std::vector<std::pair<ENode *, bool>> st{{root, false}};
    while (!st.empty()) {
      auto [n, done] = st.back();
      st.pop_back();
      if (spineMemo.count(n))
        continue;
      if (!isOpX<ITE>(n)) {
        spineMemo[n];
        continue;
      }
      if (!done) {
        st.push_back({n, true});
        for (unsigned k : {1u, 2u})
          if (!spineMemo.count(n->arg(k)))
            st.push_back({n->arg(k), false});
        continue;
      }
      Atoms r;
      std::set<ENode *> own;
      if (!atomsOf(n->arg(0), kMaxArithAtoms, own)) {
        r.over = true;
      } else {
        r.atoms.assign(own.begin(), own.end());
        for (unsigned k : {1u, 2u}) {
          const Atoms &c = spineMemo[n->arg(k)];
          if (c.over) {
            r.over = true;
            break;
          }
          std::vector<ENode *> u;
          std::set_union(r.atoms.begin(), r.atoms.end(), c.atoms.begin(),
                         c.atoms.end(), std::back_inserter(u));
          r.atoms = std::move(u);
          if (r.atoms.size() > kMaxArithAtoms) {
            r.over = true;
            break;
          }
        }
      }
      if (r.over)
        r.atoms.clear();
      spineMemo[n] = std::move(r);
    }
    return spineMemo[root];
  };
  // integer candidates with one term are one line (sameAs: the first)
  std::unordered_map<ENode *, unsigned> firstOf;
  for (unsigned i = 0; i < ints.size(); ++i) {
    SplitIntCandidate &c = ints[i];
    ENode *key = solverKey(c.term);
    if (!key)
      continue;
    auto [f, fresh] = firstOf.try_emplace(key, i);
    if (!fresh) {
      c.sameAs = f->second;
      continue;
    }
    intOf.emplace(key, i);
    // the conditions of its ite spine (the normalized tree: its conditions
    // are what the query's ites hold)
    const Atoms &a = spineOf(key);
    if (!a.over)
      for (ENode *x : a.atoms)
        intsOfAtom[x].push_back(i);
  }
  for (unsigned j = 0; j < boolCands.size(); ++j)
    for (const Expr &k : boolCands[j].keys)
      if (ENode *n = solverKey(k))
        boolOf.emplace(n, j);
  // feeds: the integer candidates in each candidate's term, as encoded
  {
    std::unordered_multimap<ENode *, unsigned> rawOf;
    for (unsigned i = 0; i < ints.size(); ++i)
      rawOf.emplace(ints[i].term.get(), i);
    DagIds rawIds(idsOf(rawOf));
    for (unsigned i = 0; i < ints.size(); ++i)
      for (unsigned j : rawIds.of(ints[i].term.get()))
        if (ints[j].term != ints[i].term)
          ++ints[j].feeds;
  }
  DagIds directIds(idsOf(intOf));
  // an ite whose condition's atoms are all atoms of an integer candidate's
  // conditions: the candidate's split decides it (its cases fix those
  // conditions, up to the paths they share)
  DagIds intIds([&](ENode *n, std::vector<unsigned> &ids) {
    auto [lo, hi] = intOf.equal_range(n);
    for (auto it = lo; it != hi; ++it)
      ids.push_back(it->second);
    if (!isOpX<ITE>(n) || intsOfAtom.empty())
      return;
    std::set<ENode *> atoms;
    if (!atomsOf(n->arg(0), kMaxArithAtoms, atoms) || atoms.empty())
      return;
    std::map<unsigned, size_t> hits;
    for (ENode *a : atoms) {
      auto it = intsOfAtom.find(a);
      if (it == intsOfAtom.end())
        return;
      for (unsigned i : it->second)
        ++hits[i];
    }
    for (auto [i, k] : hits)
      if (k == atoms.size())
        ids.push_back(i);
  });
  DagIds boolIds(idsOf(boolOf));
  DagIds condIds([&](ENode *n, std::vector<unsigned> &ids) {
    if (isOpX<ITE>(n)) {
      const std::vector<unsigned> &b = boolIds.of(n->arg(0));
      ids.insert(ids.end(), b.begin(), b.end());
    }
  });
  uint64_t nodes = 0;
  std::unordered_set<ENode *> seen;
  std::vector<ENode *> stack{solverQuery.get()};
  while (!stack.empty()) {
    ENode *n = stack.back();
    stack.pop_back();
    if (!seen.insert(n).second)
      continue;
    for (size_t i = 0; i < n->arity(); ++i)
      stack.push_back(n->arg(i));
    ArithKind k = arithKind(n);
    if (k == ArithKind::None)
      continue;
    ++nodes;
    // each candidate once per node, whichever operands hold it
    std::vector<unsigned> is, ds, bs;
    for (size_t i = 0; i < n->arity(); ++i) {
      unite(is, intIds.of(n->arg(i)));
      unite(ds, directIds.of(n->arg(i)));
      if (bools)
        unite(bs, condIds.of(n->arg(i)));
    }
    for (unsigned i : ds)
      ++ints[i].arithDirect;
    for (unsigned i : is) {
      ++ints[i].arith;
      ++(k == ArithKind::Mul ? ints[i].arithMul : ints[i].arithDiv);
    }
    for (unsigned j : bs) {
      ++boolCands[j].arith;
      ++(k == ArithKind::Mul ? boolCands[j].arithMul : boolCands[j].arithDiv);
    }
  }
  // the leaves of the candidates that control arithmetic: distinct non-ite
  // nodes below the ite spine, counted up to maxPaths (more: capped, 0)
  for (SplitIntCandidate &c : ints) {
    if (!c.arith || c.sameAs >= 0)
      continue;
    std::unordered_set<ENode *> seen, leaves;
    std::vector<ENode *> st{c.term.get()};
    while (!st.empty() && !c.leavesCapped) {
      ENode *n = st.back();
      st.pop_back();
      if (!seen.insert(n).second)
        continue;
      if (!isOpX<ITE>(n)) {
        leaves.insert(n);
        c.leavesCapped = leaves.size() > maxPaths;
        continue;
      }
      st.push_back(n->arg(2));
      st.push_back(n->arg(1));
    }
    c.leaves = c.leavesCapped ? 0 : leaves.size();
  }
  return nodes;
}

IteCensus::IteCensus(ProgramOp program) {
  // the backward slice of the arithmetic ops' operands, through integer and
  // i1 values (op operands, and the branch operands of block arguments)
  std::vector<mlir::Value> work;
  auto mark = [&](mlir::Value v) {
    if (isa<IntegerType>(v.getType()) && feedsArith.insert(v).second)
      work.push_back(v);
  };
  program.walk([&](Operation *op) {
    if (isa<MulOp, UDivOp, SDivOp, URemOp, SRemOp, UMulOOp, SMulOOp, SDivOOp>(
            op))
      for (mlir::Value o : op->getOperands())
        mark(o);
  });
  while (!work.empty()) {
    mlir::Value v = work.back();
    work.pop_back();
    if (auto a = dyn_cast<BlockArgument>(v)) {
      Block *b = a.getOwner();
      for (Block *pred : b->getPredecessors())
        if (auto br = dyn_cast<BranchOpInterface>(pred->getTerminator()))
          for (unsigned s = 0; s < br->getNumSuccessors(); ++s)
            if (br->getSuccessor(s) == b)
              mark(br.getSuccessorOperands(s)[a.getArgNumber()]);
      continue;
    }
    Operation *def = v.getDefiningOp();
    // not through memory: a load's value comes from its memory operand
    if (!def || isa<LoadOp>(def))
      continue;
    for (mlir::Value o : def->getOperands())
      mark(o);
  }
}

bool IteCensus::ground(ENode *root, const expr::BaseRegistry &bases) {
  std::vector<std::pair<ENode *, bool>> st{{root, false}};
  while (!st.empty()) {
    auto [n, done] = st.back();
    st.pop_back();
    if (groundMemo.count(n))
      continue;
    // a free constant; an object's base (--seam: obj.<name>) is a fixed
    // address
    if (bind::isFapp(Expr(n))) {
      groundMemo[n] = bases.isBase(Expr(n));
      continue;
    }
    if (!done) {
      st.push_back({n, true});
      for (size_t i = 0; i < n->arity(); ++i)
        if (!groundMemo.count(n->arg(i)))
          st.push_back({n->arg(i), false});
      continue;
    }
    bool g = true;
    for (size_t i = 0; i < n->arity() && g; ++i)
      g = groundMemo[n->arg(i)];
    groundMemo[n] = g;
  }
  return groundMemo[root];
}

const IteCensus::TreeSets &IteCensus::treeOf(const Expr &root) {
  auto add = [](std::vector<ENode *> &into, const std::vector<ENode *> &b,
                bool &capped) {
    std::vector<ENode *> u;
    std::set_union(into.begin(), into.end(), b.begin(), b.end(),
                   std::back_inserter(u));
    if (u.size() > kTreeCap) {
      u.resize(kTreeCap);
      capped = true;
    }
    into = std::move(u);
  };
  std::vector<std::pair<ENode *, bool>> st{{root.get(), false}};
  while (!st.empty()) {
    auto [n, done] = st.back();
    st.pop_back();
    if (treeMemo.count(n))
      continue;
    if (!done) {
      st.push_back({n, true});
      for (unsigned k : {1u, 2u})
        if (isOpX<ITE>(n->arg(k)) && !treeMemo.count(n->arg(k)))
          st.push_back({n->arg(k), false});
      continue;
    }
    TreeSets r;
    r.conds = {n->arg(0)};
    std::set<ENode *> atoms;
    atomsOf(n->arg(0), SIZE_MAX, atoms);
    for (ENode *a : atoms)
      if (guards.count(a))
        r.guards.push_back(a);
    for (unsigned k : {1u, 2u}) {
      ENode *c = n->arg(k);
      if (!isOpX<ITE>(c)) {
        add(r.leaves, {c}, r.capped);
        continue;
      }
      const TreeSets &cs = treeMemo[c];
      r.capped |= cs.capped;
      add(r.leaves, cs.leaves, r.capped);
      add(r.conds, cs.conds, r.capped);
      add(r.guards, cs.guards, r.capped);
    }
    keepTrees.push_back(Expr(n));
    treeMemo[n] = std::move(r);
  }
  return treeMemo[root.get()];
}

bool IteCensus::hasIte(ENode *root) {
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
      h = iteMemo[n->arg(i)];
    iteMemo[n] = h;
  }
  return iteMemo[root];
}

void IteCensus::install(SeaOpSem &sem, OpSemContext &ctx) {
  auto prevGuard = ctx.guardHook;
  ctx.guardHook = [this, prevGuard](Block *b, Expr g) {
    keep.push_back(g);
    guards.insert(g.get());
    return prevGuard ? prevGuard(b, g) : g;
  };
  auto prevDef = ctx.defHook;
  ctx.defHook = [this, prevDef, &sem, &ctx](mlir::Value v) {
    if (prevDef)
      prevDef(v);
    if (auto ld = dyn_cast_or_null<LoadOp>(v.getDefiningOp())) {
      ++allLoads;
      Expr a = ctx.read(sem.mkSymbReg(ld.getAddr()));
      if (!ground(a.get(), sem.baseRegistry())) {
        const bool fromBits = isOpX<PTR_FROM_BITS>(a);
        Expr t = fromBits ? Expr(a->arg(0)) : a;
        loads.push_back({isOpX<ITE>(t)     ? "ite"
                         : fromBits        ? "from-bits"
                         : hasIte(t.get()) ? "ite-index"
                                           : "symbolic",
                         ld});
      }
    }
    const char *kind = nullptr;
    mlir::Type ty = v.getType();
    if (isa<MemType>(ty))
      kind = "mem";
    else if (isa<AddrType>(ty))
      kind = "addr";
    else if (isLeavesType(ty) && feedsArith.contains(v))
      kind = "int";
    if (!kind)
      return;
    Expr t = ctx.read(sem.mkSymbReg(v));
    if (isOpX<PTR_FROM_BITS>(t))
      t = Expr(t->arg(0));
    if (!isOpX<ITE>(t))
      return;
    // the tree as a DAG: distinct leaves, conditions, and the guards
    // among the conditions' atoms (memoized per ite node, capped)
    const TreeSets &ts = treeOf(t);
    Value r{kind, v};
    r.leaves = ts.leaves.size();
    r.conds = ts.conds.size();
    r.guardAtoms = ts.guards.size();
    r.capped = ts.capped;
    values.push_back(r);
  };
}

} // namespace sea
