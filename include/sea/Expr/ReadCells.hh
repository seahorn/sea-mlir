// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// The cells of array constants a query reads, and a solver::Model that answers
// solver::Model::defaultCell from them (docs/design/Print.md, sections 4-5).
//
// A cell A[v] of an array constant A is a default of a model when no
// constraint of the query reads it: every use of A in the query is the array
// operand of a select, reached through store chains and ite arms, and no
// select index evaluates to v in the model. Changing A[v] then changes no
// subterm of the query, so every value of the cell is consistent with the
// model; the value the model gives it (a solver's array default, an `else`)
// is not a fact of the counterexample.
//
// Conservative: a read through a store at the same index still counts as a
// read of A; an array used any other way (an equality of arrays, a lambda,
// an operator this file does not know), or one with an index the model
// gives no numeral, has no default cell. An array constant the query does
// not mention has only default cells.
#include "sea/Expr/Ackermann.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/Smt/Model.hh"

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace expr {

/// The read cells of the array constants of one query.
class ReadCells {
public:
  explicit ReadCells(Expr query) { analyze(query); }

  /// Does the query mention array constant \p a?
  bool mentions(ENode *a) const {
    return m_reads.count(a) || m_opaque.count(a);
  }
  /// Is every use of \p a a read (a select through stores and ites)?
  bool onlyRead(ENode *a) const { return !m_opaque.count(a); }
  /// The index terms of the reads of \p a.
  const std::vector<Expr> &indices(ENode *a) const {
    static const std::vector<Expr> none;
    auto it = m_reads.find(a);
    return it == m_reads.end() ? none : it->second;
  }

private:
  /// An array-valued term: an array constant, a store, an ite of arrays.
  bool arrayValued(ENode *n) {
    if (op::bind::isArrayConst(Expr(n)) || isOpX<STORE>(n))
      return true;
    return isOpX<ITE>(n) && n->arity() == 3 && arrayValued(n->arg(1));
  }

  /// The array constants under the array term \p n (through store chains
  /// and ite arms) to \p fn.
  template <typename Fn>
  void bases(ENode *n, Fn fn) {
    std::vector<ENode *> stack{n};
    std::unordered_set<ENode *> seen;
    while (!stack.empty()) {
      ENode *m = stack.back();
      stack.pop_back();
      if (!seen.insert(m).second)
        continue;
      if (op::bind::isArrayConst(Expr(m)))
        fn(m);
      else if (isOpX<STORE>(m))
        stack.push_back(m->arg(0));
      else if (isOpX<ITE>(m) && m->arity() == 3) {
        stack.push_back(m->arg(1));
        stack.push_back(m->arg(2));
      }
    }
  }

  void analyze(Expr query) {
    using namespace ackermann_detail;
    std::unordered_set<ENode *> seen;
    std::vector<ENode *> stack{query.get()};
    auto opaque = [&](ENode *b) { m_opaque.insert(b); };
    // the root itself as an array (not a query): every base is opaque
    if (arrayValued(query.get()))
      bases(query.get(), opaque);
    while (!stack.empty()) {
      ENode *n = stack.back();
      stack.pop_back();
      if (!seen.insert(n).second || isLeaf(n))
        continue;
      if (isOpX<SELECT>(n))
        bases(n->arg(0), [&](ENode *b) {
          m_reads[b].push_back(Expr(n->arg(1)));
          m_keep.push_back(Expr(b));
        });
      for (size_t k = 0; k < n->arity(); ++k) {
        ENode *c = n->arg(k);
        bool allowed = (isOpX<SELECT>(n) && k == 0) ||
                       (isOpX<STORE>(n) && k == 0) ||
                       (isOpX<ITE>(n) && k > 0 && arrayValued(n));
        if (!allowed && arrayValued(c))
          bases(c, [&](ENode *b) {
            opaque(b);
            m_keep.push_back(Expr(b));
          });
        stack.push_back(c);
      }
    }
  }

  std::unordered_map<ENode *, std::vector<Expr>> m_reads;
  std::unordered_set<ENode *> m_opaque;
  std::vector<Expr> m_keep;
};

/// A solver's model of \p query (the term given to the solver) with
/// defaultCell answered by ReadCells: evaluation is the inner model's.
class ReadCellsModel : public sea::solver::Model {
public:
  ReadCellsModel(sea::solver::Model &inner, Expr query)
      : m_inner(inner), m_query(query) {}

  Expr eval(Expr e, bool complete) override {
    return m_inner.eval(e, complete);
  }
  void print(llvm::raw_ostream &o) const override { m_inner.print(o); }

  bool defaultCell(Expr arr, Expr idx) override {
    using namespace ackermann_detail;
    if (!arr || !idx || !op::bind::isArrayConst(arr) || !op::bv::is_bvnum(idx))
      return false;
    if (!m_cells)
      m_cells = std::make_unique<ReadCells>(m_query); // on first use
    ENode *a = arr.get();
    if (!m_cells->mentions(a))
      return true;
    if (!m_cells->onlyRead(a))
      return false;
    auto it = m_values.find(a);
    if (it == m_values.end()) {
      // the values of the read indices; none if one has no numeral
      std::optional<std::set<mpz_class>> vals(std::in_place);
      for (const Expr &i : m_cells->indices(a)) {
        Expr v = m_inner.eval(i, /*complete=*/true);
        if (!v || !op::bv::is_bvnum(v)) {
          vals.reset();
          break;
        }
        vals->insert(reducedValue(v));
      }
      it = m_values.emplace(a, std::move(vals)).first;
    }
    return it->second && !it->second->count(reducedValue(idx));
  }

private:
  sea::solver::Model &m_inner;
  Expr m_query;
  std::unique_ptr<ReadCells> m_cells;
  std::unordered_map<ENode *, std::optional<std::set<mpz_class>>> m_values;
};

} // namespace expr
