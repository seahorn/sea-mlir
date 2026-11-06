// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/*==-- Type Checker and Type Inference For Expressions --==*/
#include "sea/Expr/TypeChecker.hh"
#include "sea/Expr/ExprAPInt.hh"
#include "sea/Expr/ExprVisitor.hh"
#include "sea/Support/SeaDebug.h"
#include "sea/Support/SeaLog.hh"
#include "llvm/Support/raw_ostream.h"

#include <unordered_map>
#include <vector>

using namespace expr;

namespace expr {
/// The inference engine visits terms in pre-order; an operator whose
/// rule is top-down (typeCheckTopDown: FDECL) and a leaf are inferred when
/// met, any other node after its kids, left to right. Every inferred node is
/// cached. Once a rule fails, the rest of the query is skipped: the failed
/// node and every node above it on the way back to the root are cached as
/// ERROR_TY, each remembering the first failure (its culprit). A query that
/// meets a cached failure replays it. Rules call typeOf on their kids (a
/// cache hit) or on other terms (a nested query: same cache, same failure
/// state; the state is reset when the outermost query ends).
class TypeChecker::Impl {
  TypeChecker &m_tc;

  struct Entry {
    Expr term;    // keeps the key alive
    Expr sort;    // ERROR_TY on failure
    Expr culprit; // on failure: the first failure it depends on
  };
  std::unordered_map<ENode *, Entry> m_cache;
  /// why a culprit failed (first reason), and whether it lacks a rule
  struct Why {
    Expr term;
    std::string why;
    bool unsupported = false;
  };
  std::unordered_map<ENode *, Why> m_why;

  /// no failure met in the current (outermost) query
  bool m_ok = true;
  /// the first failure of the current query
  Expr m_culprit;
  /// the first failure of the last finished outermost query
  Expr m_lastCulprit;
  /// nesting depth of queries (rules query other terms)
  unsigned m_depth = 0;

  struct Frame {
    Expr e;
    bool expanded;
  };
  /// the explicit stack (shared by nested queries, each using its own top)
  std::vector<Frame> m_stack;

  void noteFailure(Expr culprit) {
    if (m_ok)
      m_culprit = culprit;
    m_ok = false;
  }

  void infer(const Expr &e) {
    Expr sort;
    bool unsupported = false;
    if (m_ok) {
      sort = e->op().inferType(e, m_tc);
      if (!sort) {
        unsupported = true;
        sort = sort::errorTy(e->efac());
      }
    } else {
      sort = sort::errorTy(e->efac());
    }

    if (!isOp<ERROR_TY>(sort)) {
      m_cache.emplace(e.get(), Entry{e, sort, Expr()});
      return;
    }
    if (m_ok) {
      // e is the innermost failure: record why (unless the rule did)
      auto [it, inserted] = m_why.try_emplace(e.get());
      if (inserted) {
        it->second.term = e;
        it->second.unsupported = unsupported;
        it->second.why =
            unsupported ? "no sort rule for the operator" : "ill-sorted term";
      }
    }
    noteFailure(e);
    m_cache.emplace(e.get(), Entry{e, sort, m_culprit});
  }

  void visit(const Expr &root) {
    size_t base = m_stack.size();
    m_stack.push_back({root, false});
    while (m_stack.size() > base) {
      Frame &fr = m_stack.back();
      if (fr.expanded) {
        Expr e = std::move(fr.e);
        m_stack.pop_back();
        infer(e);
        continue;
      }
      if (!m_ok) {
        m_stack.pop_back();
        continue;
      }
      ENode *n = fr.e.get();
      auto it = m_cache.find(n);
      if (it != m_cache.end()) {
        if (isOp<ERROR_TY>(it->second.sort))
          noteFailure(it->second.culprit);
        m_stack.pop_back();
        continue;
      }
      if (n->arity() == 0 || n->op().typeCheckTopDown()) {
        Expr e = std::move(fr.e);
        m_stack.pop_back();
        infer(e);
        continue;
      }
      fr.expanded = true;
      // kids in reverse, so that the leftmost is visited first
      for (size_t i = n->arity(); i-- > 0;)
        m_stack.push_back({Expr(n->arg(i)), false});
    }
  }

public:
  Impl(TypeChecker &tc) : m_tc(tc) {}

  Expr typeOf(const Expr &e) {
    // fast path: a cached result
    auto it = m_cache.find(e.get());
    if (it == m_cache.end()) {
      ++m_depth;
      visit(e);
      --m_depth;
      it = m_cache.find(e.get());
    } else if (m_ok && isOp<ERROR_TY>(it->second.sort)) {
      noteFailure(it->second.culprit);
    }
    // not inferred: the query was skipped after an earlier failure
    Expr sort =
        it != m_cache.end() ? it->second.sort : sort::errorTy(e->efac());
    if (m_depth == 0) {
      m_lastCulprit = m_ok ? Expr() : m_culprit;
      m_ok = true;
      m_culprit = Expr();
    }
    return sort;
  }

  Result check(const Expr &e) {
    Result r;
    Expr sort = typeOf(e);
    if (!isOp<ERROR_TY>(sort)) {
      r.sort = sort;
      return r;
    }
    r.culprit = m_lastCulprit ? m_lastCulprit : e;
    auto w = m_why.find(r.culprit.get());
    if (w != m_why.end()) {
      r.why = w->second.why;
      r.unsupported = w->second.unsupported;
    } else {
      r.why = "ill-sorted term";
    }
    return r;
  }

  Expr fail(const Expr &e, const std::string &why) {
    auto [it, inserted] = m_why.try_emplace(e.get());
    if (inserted) {
      it->second.term = e;
      it->second.why = why;
    }
    return sort::errorTy(e->efac());
  }

  void reset() {
    assert(m_depth == 0 && "TypeChecker::reset during a query");
    m_cache.clear();
    m_why.clear();
    m_lastCulprit = Expr();
    m_culprit = Expr();
    m_ok = true;
  }

  size_t cacheSize() const { return m_cache.size(); }
};

TypeChecker::TypeChecker() : m_impl(new TypeChecker::Impl(*this)) {}
TypeChecker::~TypeChecker() { delete m_impl; }
Expr TypeChecker::typeOf(Expr e) { return m_impl->typeOf(e); }
TypeChecker::Result TypeChecker::check(Expr e) { return m_impl->check(e); }
Expr TypeChecker::fail(Expr e, const std::string &why) {
  return m_impl->fail(e, why);
}
void TypeChecker::reset() { m_impl->reset(); }
size_t TypeChecker::cacheSize() const { return m_impl->cacheSize(); }

} // namespace expr
