// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- EqAudit.h - which undecided equalities were decidable ---*- C++ -*-===//
//
// sea-vc --eq-audit.
// The equalities the rewriting and the Ackermann lowering left undecided are
// what the solver has to search; each slow job so far was one whose
// undecided equalities were decidable by a rule we lacked. The audit samples
// them per source and asks the solver, per item, for the lowest level of
// facts that decides it:
//   L0  nothing          a missing exact rewrite (bit-vector identity)
//   L1  + the layout     obj.k = <numeral> (a fact of the VC)
//   L2  + A-INB          the in-bounds premises of the accesses involved
//   undecided            none of these: the solver's search (or a path
//                        fact, level L3, not audited yet)
// Sources: `ack` the Ackermann constraint pairs (index pair; the indices may
// mention the fresh read constants, which are left free: a pair decided only
// through what a nested read holds counts as undecided); `row-store` a read
// over a store whose address compare stayed unknown (row.store: the store
// address and the read index); `row-memop` a read over a memory-operation
// term left as one ite (row.mcopy / row.mset / row.mhavoc: the membership
// (j - d) <u n, decided in or out). A sample of N items per source, evenly
// spaced (deterministic). Each check has a timeout; a timeout is `unknown`.
// The report is line-oriented (`eq-audit:` and `eq-audit-shape:` lines,
// docs in sea-vc --agent); shapes abstract objects (O; N for a size-0
// object), numerals (K), fresh read constants (R) and other constants (X),
// depth-limited, so that one missing rule shows as one frequent shape.
//
//===----------------------------------------------------------------------===//
#ifndef SEA_VC_TOOL_EQAUDIT_H
#define SEA_VC_TOOL_EQAUDIT_H

#include "sea/Expr/BaseRegistry.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/Smt/Solver.hh"
#include "sea/Support/Stats.h"
#include "sea/Support/Timer.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace sea {

class EqAudit {
public:
  enum Level { L0 = 0, L1, L2, Undecided, Unknown, Vacuous, Unstated, NLevels };
  static const char *levelName(unsigned l) {
    static const char *names[] = {"L0",      "L1",      "L2",      "undecided",
                                  "unknown", "vacuous", "unstated"};
    return names[l];
  }

  /// One item: an equality a = b (pair) or a predicate p (membership); the
  /// layout and A-INB premises for L1 / L2 (null: cannot be stated).
  struct Item {
    expr::Expr a, b; ///< a pair (b null: a is a Boolean predicate)
    expr::Expr layout, inb;
    uint64_t example; ///< a firing id or read pair number, for the report
  };

  EqAudit(sea::solver::Solver &s, const expr::BaseRegistry &bases,
          unsigned shapeDepth = 3)
      : m_solver(s), m_bases(bases), m_depth(shapeDepth) {}

  /// With an rlimit: told, for an unknown check that took the given seconds,
  /// whether it was the rlimit (counted as unknown-rlimit per source)
  void setRlimitHit(std::function<bool(double)> f) {
    m_rlimitHit = std::move(f);
  }

  /// Audits \p it for \p source; returns its level.
  Level audit(llvm::StringRef source, const Item &it) {
    using namespace expr;
    Src &src = m_src[source.str()];
    uint64_t before = m_rlimitUnknown;
    Level lvl = classify(it);
    ++src.count[lvl];
    src.unknownRlimit += m_rlimitUnknown > before;
    std::string key = shapeOf(it);
    auto &sh = src.shapes[lvl][key];
    if (sh.first++ == 0) {
      sh.second = it.example;
      std::ostringstream os;
      os << *it.a;
      if (it.b)
        os << "  =?=  " << *it.b;
      std::string t = os.str();
      if (t.size() > m_textMax)
        t = t.substr(0, m_textMax) + "...";
      src.texts[lvl][key] = t;
    }
    return lvl;
  }

  /// Totals seen per source (before sampling), for the report.
  void noteTotal(llvm::StringRef source, uint64_t n) {
    m_src[source.str()].total = n;
  }

  void report(stats::Report &r, unsigned topShapes) const {
    for (const auto &[name, src] : m_src) {
      uint64_t sampled = 0;
      for (uint64_t c : src.count)
        sampled += c;
      stats::Line &l = r.line("eq-audit")
                           .kv("source", name)
                           .kv("items", src.total)
                           .kv("sampled", sampled);
      for (unsigned lv = 0; lv < NLevels; ++lv)
        l.kv(levelName(lv), src.count[lv]);
      if (m_rlimitHit)
        l.kv("unknown-rlimit", src.unknownRlimit);
      for (unsigned lv = 0; lv < NLevels; ++lv) {
        std::vector<std::pair<uint64_t, const std::string *>> v;
        for (const auto &[k, ce] : src.shapes[lv])
          v.push_back({ce.first, &k});
        std::sort(v.begin(), v.end(), [](auto &x, auto &y) {
          return x.first != y.first ? x.first > y.first : *x.second < *y.second;
        });
        for (size_t i = 0; i < v.size() && i < topShapes; ++i) {
          r.line("eq-audit-shape")
              .kv("source", name)
              .kv("level", levelName(lv))
              .kv("count", v[i].first)
              .kv("example", src.shapes[lv].at(*v[i].second).second)
              .kv("shape", *v[i].second);
          r.line("eq-audit-example").value(src.texts[lv].at(*v[i].second));
        }
      }
    }
  }

  /// The counters as BRUNCH_STAT lines only (no text line):
  /// eq-audit.<source>.<level> = count.
  void counters(stats::Report &r) const {
    for (const auto &[name, src] : m_src)
      for (unsigned l = 0; l < NLevels; ++l)
        r.line("eq-audit." + name + "." + levelName(l))
            .value(src.count[l])
            .brunch(stats::Brunch::Value)
            .brunchOnly();
  }

private:
  struct Src {
    uint64_t total = 0;
    uint64_t count[NLevels] = {};
    /// items with a check that reached the rlimit
    uint64_t unknownRlimit = 0;
    /// per level: shape -> (count, first example)
    std::map<std::string, std::pair<uint64_t, uint64_t>> shapes[NLevels];
    /// per level: shape -> the printed first example
    std::map<std::string, std::string> texts[NLevels];
  };

  sea::solver::SolverResult check(const expr::ExprVector &fs) {
    m_solver.push();
    sea::Stopwatch watch;
    bool ok = true;
    for (const expr::Expr &f : fs)
      ok = ok && m_solver.add(f);
    sea::solver::SolverResult r =
        ok ? m_solver.check() : sea::solver::SolverResult::UNKNOWN;
    if (r == sea::solver::SolverResult::UNKNOWN && m_rlimitHit &&
        m_rlimitHit(watch.seconds()))
      ++m_rlimitUnknown;
    m_solver.pop();
    return r;
  }

  /// The two literals whose unsatisfiability decides the item: `a != b` and
  /// `a = b` (a pair), `!p` and `p` (a predicate).
  void literals(const Item &it, expr::Expr &x, expr::Expr &y) const {
    using namespace expr;
    if (it.b) {
      x = mk<NEG>(mk<EQ>(it.a, it.b));
      y = mk<EQ>(it.a, it.b);
    } else {
      x = mk<NEG>(it.a);
      y = it.a;
    }
  }

  Level classify(const Item &it) {
    using sea::solver::SolverResult;
    expr::Expr x, y;
    literals(it, x, y);
    bool unknown = false;
    // L0: no facts; L1: layout; L2: layout and A-INB
    std::vector<expr::ExprVector> levels(3);
    if (it.layout)
      levels[1].push_back(it.layout);
    levels[2] = levels[1];
    if (it.inb)
      levels[2].push_back(it.inb);
    for (unsigned l = 0; l < 3; ++l) {
      if (l == 2 && !it.inb)
        return unknown ? Unknown : Unstated;
      if (l > 0 && levels[l].empty())
        continue;
      if (l == 2) {
        SolverResult pr = check(levels[2]);
        if (pr == SolverResult::UNSAT)
          return Vacuous;
      }
      for (const expr::Expr &lit : {x, y}) {
        expr::ExprVector q = levels[l];
        q.push_back(lit);
        SolverResult r = check(q);
        if (r == SolverResult::UNSAT)
          return Level(l);
        if (r != SolverResult::SAT)
          unknown = true;
      }
    }
    return unknown ? Unknown : Undecided;
  }

  std::string shapeOf(const Item &it) const {
    std::string a = shape(it.a, m_depth);
    if (!it.b)
      return a;
    std::string b = shape(it.b, m_depth);
    return a < b ? a + " | " + b : b + " | " + a;
  }

  std::string shape(const expr::Expr &e, unsigned d) const {
    using namespace expr;
    if (const ObjectBase *o = m_bases.lookup(e))
      return o->size.isZero() ? "N" : "O";
    if (op::bv::is_bvnum(e))
      return "K";
    if (e->arity() == 0 || isOpX<FAPP>(e)) {
      std::ostringstream os;
      os << *e;
      std::string t = os.str();
      if (llvm::StringRef(t).starts_with("ack."))
        return "R";
      return "X";
    }
    if (d == 0)
      return "_";
    std::string res = "(" + e->op().name();
    for (size_t i = 0; i < e->arity(); ++i)
      res += " " + shape(e->arg(i), d - 1);
    return res + ")";
  }

  sea::solver::Solver &m_solver;
  const expr::BaseRegistry &m_bases;
  unsigned m_depth;
  size_t m_textMax = 600;
  std::map<std::string, Src> m_src;
  std::function<bool(double)> m_rlimitHit;
  uint64_t m_rlimitUnknown = 0;
};

} // namespace sea

#endif
