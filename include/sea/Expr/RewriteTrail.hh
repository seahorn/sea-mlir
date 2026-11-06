// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// The rule trail of the rewriter (docs/design/SeaM.md, "Rule
// trail and per-firing check").
//
// A *firing* is one rule call (Config::doRewrite) that changed its term: lhs
// is the term the rule was given (its kids already rewritten), rhs the term
// it returned. Each firing gets the names of the rules that decided it (the
// counters the rules bumped during that call, in order: the rule itself and
// the reasons of the decisions it rests on, e.g. row.store-skip with
// cmp.diff-base or arm.skip), the premises it used (Premise bits of
// seam::Decision: bit 1 = A-INB), and the VC operation that triggered it
// (the context: load / store / addr with the MLIR location and the access
// size in address units, or side / query).
//
// Caching. A firing is recorded once, when the rule runs. A later visit of
// the same term that takes the rewriter's cache is not a firing: it is
// counted (RewriteStats::cacheHits, in the run summary), not recorded. The
// term a cache hit returns is the rhs of the last firing of the chain that
// produced it, so a reader finds the rules behind any term from the trail.
//
// Cost. Off (RewriteStats::trail null) the rewriter and the counters test a
// pointer; nothing is allocated. On: one record per firing, the terms
// written once per node.
//
// Format (JSON lines; utils/seam-trail.py reads it):
//   trail file:
//     {"t":"run","run":R,"inb":B}                       a VC run starts
//     {"t":"fire","run":R,"id":N,"rules":[...],"premises":[...],
//      "op":"load","loc":"f.mlir:3:5","units":U,"lhs":I,"rhs":J
//      [,"lhs_text":"...","rhs_text":"..."]}           a firing
//     {"t":"summary","run":R,"firings":N,"rule_calls":C,"cache_hits":H}
//   terms file (<trail>.terms):
//     {"run":R,"id":I,"op":"select","args":[I1,I2]}     an inner node
//     {"run":R,"id":I,"op":"...","text":"..."}           a leaf (printed)
//   Node ids are those of the run's ExprFactory (unique within a run). Every
//   node reachable from a recorded lhs / rhs is in the terms file, once.
#include "sea/Expr/Expr.hh"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace expr {

class RewriteTrail {
public:
  /// The VC operation a firing happened under.
  struct Context {
    std::string kind; ///< load, store, addr, side, query, or "?"
    std::string loc;  ///< file:line:col of the op (empty for side / query)
    uint64_t units;   ///< address units of the access (0: not an access)
  };
  /// A recorded firing (kept in memory only with setKeep, for the check).
  struct Firing {
    uint64_t id;
    Expr lhs, rhs;
    llvm::SmallVector<unsigned, 4> rules; ///< name ids (ruleName)
    unsigned premises;
    unsigned ctx; ///< context id (context)
  };

  /// Premise bits as names (mirrors seam::Premise).
  static void premiseNames(unsigned p, llvm::SmallVectorImpl<const char *> &o) {
    if (p & 1u)
      o.push_back("A-INB");
    if (p & 2u)
      o.push_back("LAYOUT");
    if (p & 4u)
      o.push_back("ALIGN");
    if (p & 8u)
      o.push_back("PTR-INDEX");
  }

  // ---- configuration -------------------------------------------------------
  void setOutput(llvm::raw_ostream *firings, llvm::raw_ostream *terms) {
    m_out = firings;
    m_terms = terms;
  }
  void setPrintText(bool b) { m_printText = b; }
  void setKeep(bool b) { m_keep = b; }
  /// Firings with one of these rule names (an entry ending in `*` is a
  /// prefix; `all` matches every firing) are printed to \p os.
  void setTrace(std::vector<std::string> rules, llvm::raw_ostream *os) {
    m_trace = std::move(rules);
    m_traceOs = os;
  }

  // ---- runs ----------------------------------------------------------------
  /// A new VC run (a new ExprFactory): ids of nodes restart, kept firings of
  /// the previous run are dropped.
  void beginRun(bool inb) {
    ++m_run;
    m_kept.clear();
    m_printed.clear();
    m_contexts.clear();
    m_ctxKey = nullptr;
    m_ctxKind.clear();
    m_ctxUnits = 0;
    m_ctx = 0;
    m_contexts.push_back({"?", "", 0});
    m_runFirings = 0;
    if (m_out) {
      llvm::json::OStream j(*m_out);
      j.object([&] {
        j.attribute("t", "run");
        j.attribute("run", m_run);
        j.attribute("inb", inb);
      });
      *m_out << "\n";
    }
  }
  /// The run ends (before its ExprFactory): the summary, kept terms dropped.
  void endRun(uint64_t ruleCalls, uint64_t cacheHits) {
    if (m_out) {
      llvm::json::OStream j(*m_out);
      j.object([&] {
        j.attribute("t", "summary");
        j.attribute("run", m_run);
        j.attribute("firings", m_runFirings);
        j.attribute("rule_calls", ruleCalls);
        j.attribute("cache_hits", cacheHits);
      });
      *m_out << "\n";
      m_out->flush();
    }
    if (m_terms)
      m_terms->flush();
    m_kept.clear();
    m_printed.clear();
  }

  // ---- context -------------------------------------------------------------
  /// The operation the next firings belong to. \p key identifies it (an
  /// mlir::Operation *, or null); \p loc is called only when it changes.
  template <typename LocFn>
  void setContext(llvm::StringRef kind, const void *key, uint64_t units,
                  LocFn loc) {
    if (key == m_ctxKey && units == m_ctxUnits && kind == m_ctxKind)
      return;
    m_ctxKey = key;
    m_ctxUnits = units;
    m_ctxKind = kind.str();
    m_contexts.push_back({m_ctxKind, loc(), units});
    m_ctx = m_contexts.size() - 1;
  }
  const Context &context(unsigned id) const { return m_contexts[id]; }

  // ---- per rule call (the rewriter and the counters) ----------------------
  void begin() {
    m_active = true;
    m_pending.clear();
    m_prem = 0;
  }
  void note(llvm::StringRef rule) {
    if (!m_active || rule.starts_with("premise."))
      return;
    unsigned id = intern(rule);
    for (unsigned p : m_pending)
      if (p == id)
        return;
    m_pending.push_back(id);
  }
  void notePremise(unsigned p) {
    if (m_active)
      m_prem |= p;
  }
  /// The rule call ended with \p rhs for \p lhs; a firing if they differ.
  void end(const Expr &lhs, const Expr &rhs) {
    m_active = false;
    if (lhs == rhs)
      return;
    if (m_pending.empty())
      m_pending.push_back(intern("?")); // a rule that changed a term uncounted
    Firing f{m_next++,
             lhs,
             rhs,
             llvm::SmallVector<unsigned, 4>(m_pending.begin(), m_pending.end()),
             m_prem,
             m_ctx};
    ++m_runFirings;
    if (m_out)
      write(f);
    if (m_traceOs && traced(f))
      trace(f);
    if (m_keep)
      m_kept.push_back(std::move(f));
  }

  // ---- reading -------------------------------------------------------------
  const std::vector<Firing> &kept() const { return m_kept; }
  llvm::StringRef ruleName(unsigned id) const { return m_names[id]; }
  std::string rulesOf(const Firing &f, llvm::StringRef sep = ",") const {
    std::string s;
    for (unsigned r : f.rules) {
      if (!s.empty())
        s += sep;
      s += m_names[r];
    }
    return s;
  }
  uint64_t firingsThisRun() const { return m_runFirings; }

  /// \p e printed as `op(arg, ...)` (leaves by Expr's printer), cut after
  /// about \p max characters (`...`). Bounded: a VC term is a DAG whose tree
  /// can be exponentially larger, so the printer stops with the budget.
  static std::string text(const Expr &e, size_t max = 2000) {
    std::string s;
    textRec(e.get(), max, s);
    if (s.size() > max) {
      s.resize(max);
      s += "...";
    }
    return s;
  }

private:
  static void textRec(ENode *n, size_t max, std::string &s) {
    if (s.size() > max)
      return;
    const bool leaf = n->arity() == 0 || isOpX<BIND>(n) ||
                      (isOpX<FAPP>(n) && n->arity() == 1);
    if (leaf) {
      std::ostringstream ss;
      ss << *n;
      s += ss.str();
      return;
    }
    s += n->op().name();
    s += "(";
    for (size_t i = 0; i < n->arity() && s.size() <= max; ++i) {
      if (i)
        s += ", ";
      textRec(n->arg(i), max, s);
    }
    s += ")";
  }

  unsigned intern(llvm::StringRef name) {
    auto it = m_nameIds.find(name);
    if (it != m_nameIds.end())
      return it->second;
    unsigned id = m_names.size();
    m_names.push_back(name.str());
    m_nameIds[name] = id;
    return id;
  }

  bool traced(const Firing &f) const {
    for (const std::string &t : m_trace) {
      if (t == "all")
        return true;
      for (unsigned r : f.rules) {
        llvm::StringRef n = m_names[r];
        if (n == t || (!t.empty() && t.back() == '*' &&
                       n.starts_with(llvm::StringRef(t).drop_back())))
          return true;
      }
    }
    return false;
  }

  void trace(const Firing &f) {
    const Context &c = m_contexts[f.ctx];
    llvm::SmallVector<const char *, 2> prem;
    premiseNames(f.premises, prem);
    llvm::raw_ostream &os = *m_traceOs;
    os << "seam-trace: id=" << f.id << " rules=" << rulesOf(f)
       << " premises=" << (prem.empty() ? "none" : "");
    for (size_t i = 0; i < prem.size(); ++i)
      os << (i ? "," : "") << prem[i];
    os << " op=" << c.kind;
    if (!c.loc.empty())
      os << " loc=" << c.loc;
    os << " lhs=" << f.lhs->getId() << " rhs=" << f.rhs->getId() << "\n";
    os << "seam-trace-lhs: " << text(f.lhs, 400) << "\n";
    os << "seam-trace-rhs: " << text(f.rhs, 400) << "\n";
  }

  void write(const Firing &f) {
    writeTerms(f.lhs);
    writeTerms(f.rhs);
    const Context &c = m_contexts[f.ctx];
    llvm::SmallVector<const char *, 2> prem;
    premiseNames(f.premises, prem);
    llvm::json::OStream j(*m_out);
    j.object([&] {
      j.attribute("t", "fire");
      j.attribute("run", m_run);
      j.attribute("id", f.id);
      j.attributeArray("rules", [&] {
        for (unsigned r : f.rules)
          j.value(m_names[r]);
      });
      j.attributeArray("premises", [&] {
        for (const char *p : prem)
          j.value(p);
      });
      j.attribute("op", c.kind);
      if (!c.loc.empty())
        j.attribute("loc", c.loc);
      if (c.units)
        j.attribute("units", c.units);
      j.attribute("lhs", f.lhs->getId());
      j.attribute("rhs", f.rhs->getId());
      if (m_printText) {
        j.attribute("lhs_text", text(f.lhs));
        j.attribute("rhs_text", text(f.rhs));
      }
    });
    *m_out << "\n";
  }

  /// Every node under \p root not yet written, once (iterative: VC terms are
  /// deep).
  void writeTerms(const Expr &root) {
    if (!m_terms || m_printed.count(root->getId()))
      return;
    llvm::SmallVector<std::pair<ENode *, bool>, 64> stack;
    stack.push_back({root.get(), false});
    while (!stack.empty()) {
      ENode *n = stack.back().first;
      bool expanded = stack.back().second;
      stack.pop_back();
      if (m_printed.count(n->getId()))
        continue;
      const bool leaf = n->arity() == 0 || isOpX<BIND>(n) ||
                        (isOpX<FAPP>(n) && n->arity() == 1);
      if (!leaf && !expanded) {
        stack.push_back({n, true});
        for (size_t i = n->arity(); i-- > 0;)
          if (!m_printed.count(n->arg(i)->getId()))
            stack.push_back({n->arg(i), false});
        continue;
      }
      m_printed.insert(n->getId());
      llvm::json::OStream j(*m_terms);
      j.object([&] {
        j.attribute("run", m_run);
        j.attribute("id", n->getId());
        j.attribute("op", n->op().name());
        if (leaf) {
          j.attribute("text", text(Expr(n), 400));
        } else {
          j.attributeArray("args", [&] {
            for (size_t i = 0; i < n->arity(); ++i)
              j.value(n->arg(i)->getId());
          });
        }
      });
      *m_terms << "\n";
    }
  }

  llvm::raw_ostream *m_out = nullptr, *m_terms = nullptr, *m_traceOs = nullptr;
  bool m_printText = false, m_keep = false;
  std::vector<std::string> m_trace;

  unsigned m_run = 0;
  uint64_t m_next = 0, m_runFirings = 0;
  std::vector<Context> m_contexts{{"?", "", 0}};
  unsigned m_ctx = 0;
  const void *m_ctxKey = nullptr;
  std::string m_ctxKind;
  uint64_t m_ctxUnits = 0;

  bool m_active = false;
  llvm::SmallVector<unsigned, 8> m_pending;
  unsigned m_prem = 0;

  std::vector<std::string> m_names;
  llvm::StringMap<unsigned> m_nameIds;
  std::vector<Firing> m_kept;
  llvm::DenseSet<unsigned> m_printed;
};

} // namespace expr
