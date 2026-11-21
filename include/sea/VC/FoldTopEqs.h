// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- FoldTopEqs.h - substitute the VC's top-level facts -------*- C++ -*-===//
//
// sea-vc --vc-fold-top-eqs (docs/design/FoldTopEqs.md): the top-level
// conjuncts of the query that fix a term to a constant (`T = c`, a Boolean
// literal) are facts of every model. Every other occurrence of T becomes c,
// the defining conjunct stays (models and replay unchanged), and the query
// is simplified again; rounds until nothing changes.
//
// The facts:
//  - term: `T = c` (either side), c a bit-vector numeral, T not a numeral;
//  - split: `concat(a, b) = c` also gives `a = hi(c)` and `b = lo(c)`
//    (recursively; n-ary concats by part);
//  - bool: a Boolean constant `b` as a conjunct gives b := true, `not b`
//    gives b := false (block guards, Boolean nondets).
// Skipped, counted: `bvand(x, m) = c` with a numeral mask m (it fixes some
// bits of x only; the term itself is not substituted either), and a concat
// part whose width the type checker cannot infer.
//
// Identity: Expr is hash-consed, so an occurrence of T is the node T.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_VC_FOLDTOPEQS_H
#define SEA_VC_FOLDTOPEQS_H

#include "sea/Expr/Expr.hh"
#include "sea/Expr/Smt/Model.hh"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace sea {

struct FoldTopEqsStats {
  /// distinct keys substituted, by kind (`facts` is their sum)
  uint64_t facts = 0, termFacts = 0, splitFacts = 0, boolFacts = 0;
  /// (parent node, operand) pairs replaced by a fact's value, summed over
  /// the rounds
  uint64_t uses = 0;
  /// rounds that changed the query
  uint64_t rounds = 0;
  /// concats of ites on one condition (and numerals) made ites of concats
  uint64_t hoisted = 0;
  /// facts not used: masked (`bvand(x, m) = c`), a concat part of unknown
  /// width
  uint64_t skippedMasked = 0, skippedUntyped = 0;
  /// the round bound stopped the iteration (not a fixpoint)
  bool roundCap = false;
};

class TopEqFolder {
public:
  using Simplify = std::function<expr::Expr(expr::Expr)>;
  /// \p simplify: the normalization run after each substitution
  explicit TopEqFolder(Simplify simplify, unsigned maxRounds = 64)
      : simplify(std::move(simplify)), maxRounds(maxRounds) {}

  /// \p query with every fact substituted; the facts themselves kept
  expr::Expr fold(expr::Expr query);
  /// the image of a node of fold's input in its output (for origins)
  expr::Expr image(const expr::Expr &n);

  const FoldTopEqsStats &stats() const { return st; }

private:
  enum class Kind { Term, Split, Bool };
  /// key -> (key, value): the key is held, so its node is not reused
  using Map =
      std::unordered_map<expr::ENode *, std::pair<expr::Expr, expr::Expr>>;
  using Cache = Map;

  /// the keys a conjunct fixes and their values (no registration)
  void factsOf(const expr::Expr &c,
               std::vector<std::tuple<expr::Expr, expr::Expr, Kind>> &out);
  void splitConcat(const expr::Expr &t, const expr::Expr &c,
                   std::vector<std::tuple<expr::Expr, expr::Expr, Kind>> &out);
  /// \p e with every key of \p m replaced, memoized in \p cache; counts uses
  expr::Expr subst(const expr::Expr &e, const Map &m, Cache &cache, bool count);
  /// \p e with the keys in \p own kept (their operands substituted)
  expr::Expr keep(const expr::Expr &e, const std::vector<expr::ENode *> &own,
                  const Map &m, Cache &cache, bool root);

  Simplify simplify;
  unsigned maxRounds;
  FoldTopEqsStats st;
  /// every key and its value (all rounds)
  Map facts;
  /// the map of each round that changed the query, for image()
  std::vector<Map> roundMaps;
  std::vector<Cache> imageCaches;
};

/// One elimination of the model trail: \p var := \p value (a numeral, true
/// or false). The query after the elimination does not mention var.
struct TrailEntry {
  expr::Expr var, value;
};

/// The second step (after the Ackermann lowering): a top-level conjunct
/// that defines an uninterpreted constant x (bit-vector or Boolean) as a
/// constant value (`x = c`, `x`, `not x`, `concat(x, y, ..) = c` with
/// constants and numerals as parts) is removed, every other occurrence of x
/// becomes c, and x := c goes to the trail; rounds until no conjunct is
/// such a definition. A model of the result extended by the trail is a
/// model of the input (docs/design/FoldTopEqs.md, "Model trail").
class TopConstEliminator {
public:
  expr::Expr eliminate(expr::Expr query);
  /// the eliminations, in order
  const std::vector<TrailEntry> &trail() const { return entries; }
  /// \p e with every eliminated constant replaced by its value
  expr::Expr image(const expr::Expr &e);
  /// rounds that eliminated a constant
  uint64_t rounds() const { return nRounds; }

private:
  using Map =
      std::unordered_map<expr::ENode *, std::pair<expr::Expr, expr::Expr>>;
  Map defs;
  Map cache;
  std::vector<TrailEntry> entries;
  uint64_t nRounds = 0;
};

/// A model of the query after TopConstEliminator, extended by its trail: a
/// model of the query before it. Every evaluation substitutes the trail
/// first.
class TrailModel : public solver::Model {
public:
  TrailModel(solver::Model &inner, TopConstEliminator &elim)
      : inner(inner), elim(elim) {}
  expr::Expr eval(expr::Expr e, bool complete) override {
    return inner.eval(elim.image(e), complete);
  }
  bool defaultCell(expr::Expr arr, expr::Expr idx) override {
    return inner.defaultCell(arr, idx);
  }
  void print(llvm::raw_ostream &o) const override { inner.print(o); }

private:
  solver::Model &inner;
  TopConstEliminator &elim;
};

} // namespace sea

#endif // SEA_VC_FOLDTOPEQS_H
