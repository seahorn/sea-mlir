// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Lazy Ackermann constraints (sea-vc --ack-relax; docs/design/Ackermann.md,
// "Lazy constraints"): a diagnosis mode, not the default.
//
// The candidates R are the constraints the eager lowering keeps
// (Ackermannizer::constraints(), after every drop). A selector picks the
// constraints given to the first solve (all: none of R). Each round solves
// the query with the kept set K (sea-vc --ack-relax-mode: on a fresh solver,
// or on one solver that is given the constraints as they join K):
//  - unsat: the answer (omitting constraints only adds models);
//  - sat: every omitted constraint `lhs = rhs -> r_k = r_l` is evaluated in
//    the model; none broken: the model satisfies the whole lowered query,
//    the answer; else every broken one joins K and the next round runs;
//  - unknown: the answer.
// After the last allowed round without an answer the caller solves the
// whole query (Stop::Rounds: a fallback, reported).
// The caller's time budget (sea-vc --timeout) bounds the whole loop: the
// loop asks `expired` before each round and after an unknown one; once it
// is true the loop stops (Stop::Timeout, no further solve, no fallback).
//
// runAckRelax is the loop over an abstract solve (unit-tested with a fake);
// ackRelaxBroken evaluates the omitted constraints in a model.
#include "sea/Expr/Ackermann.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprOpBv.hh"
#include "sea/Expr/Smt/Model.hh"
#include "sea/Expr/Smt/Solver.hh"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace expr {

/// Which candidates the first round keeps. Only `all` (start with every
/// candidate omitted) is implemented; others (by cost, class, location,
/// top-N, a seeded fraction) are a parse error until they are.
struct AckRelaxSelector {
  enum class Kind { All };
  Kind kind = Kind::All;

  /// \p text as a selector; nullopt and \p err set if it is not one.
  static std::optional<AckRelaxSelector> parse(llvm::StringRef text,
                                               std::string &err) {
    if (text == "all")
      return AckRelaxSelector{Kind::All};
    err =
        "--ack-relax: unknown selector '" + text.str() + "' (implemented: all)";
    return std::nullopt;
  }
  std::string str() const { return "all"; }
  /// The kept set of the first round over \p n candidates.
  std::vector<bool> initial(size_t n) const {
    switch (kind) {
    case Kind::All:
      return std::vector<bool>(n, false);
    }
    return std::vector<bool>(n, false);
  }
};

/// The query of a round: the lowered body, the kept candidates' conjuncts
/// and, after --vc-defs, the definitions of the named sums (the names are
/// made once, on the whole query: the definitions of every name stay, also
/// of names only omitted conjuncts use; a definition only fixes its name).
/// With every candidate kept, build() is the whole query (the same node).
struct AckRelaxQuery {
  Expr body;
  std::vector<Expr> facts; ///< per candidate (Ackermannizer::constraints())
  std::vector<Expr> tail;  ///< the definitions conjoined after the query

  /// From the lowering (recordConstraints) and the query \p lowered that
  /// lower() returned.
  static AckRelaxQuery of(const Ackermannizer &ack, const Expr &lowered) {
    AckRelaxQuery q;
    q.body = ack.body() ? ack.body() : lowered;
    for (const auto &c : ack.constraints())
      q.facts.push_back(c.fact);
    return q;
  }

  /// After a rewrite of the whole query (SumNamer::name) that took
  /// \p lowered to \p named, with \p image its map of nodes: \p named must
  /// be image(lowered), or the conjunction of it and the definitions. False
  /// if a part has no image or the query is not of that form.
  bool rename(llvm::function_ref<Expr(const Expr &)> image, const Expr &lowered,
              const Expr &named) {
    Expr img = image(lowered);
    if (!img || !(body = image(body)))
      return false;
    for (Expr &f : facts)
      if (!(f = image(f)))
        return false;
    tail.clear();
    if (named == img)
      return true;
    if (!isOpX<AND>(named) || named->arity() < 2 || named->arg(0) != img.get())
      return false;
    for (size_t i = 1; i < named->arity(); ++i)
      tail.push_back(Expr(named->arg(i)));
    return true;
  }

  /// The query with the candidates \p keep (one flag per fact).
  Expr build(const std::vector<bool> &keep) const {
    std::vector<Expr> conj{body};
    for (size_t i = 0; i < facts.size(); ++i)
      if (keep[i])
        conj.push_back(facts[i]);
    Expr inner =
        conj.size() == 1 ? body : mknary<AND>(conj.begin(), conj.end());
    if (tail.empty())
      return inner;
    std::vector<Expr> all{inner};
    all.insert(all.end(), tail.begin(), tail.end());
    return mknary<AND>(all.begin(), all.end());
  }
};

/// One solve of the loop.
struct AckRelaxRound {
  size_t kept = 0; ///< |K| given to this solve
  sea::solver::SolverResult result = sea::solver::SolverResult::UNKNOWN;
  size_t broken = 0;      ///< sat: omitted constraints the model breaks
  size_t unevaluated = 0; ///< of them: a term without a numeral value
  std::optional<uint64_t> conflicts, rlimit; ///< the backend's, if it has them
  double seconds = 0;
  /// the backend's engine of this check, if it says (z3, incremental
  /// rounds: tactic, smt, sat); empty otherwise
  std::string engine;
};

struct AckRelaxOutcome {
  /// Unsat / Unknown: the last round's answer. Consistent: a sat whose
  /// model breaks no omitted constraint. Rounds: the bound was reached
  /// without one of those (the caller solves the whole query). Timeout: the
  /// time budget of the whole loop is gone (no answer).
  enum class Stop { Unsat, Consistent, Unknown, Rounds, Timeout };
  Stop stop = Stop::Rounds;
  std::vector<AckRelaxRound> rounds;
  std::vector<bool> keep; ///< K after the last round (the one it solved)
  size_t kept() const {
    size_t n = 0;
    for (bool b : keep)
      n += b;
    return n;
  }
  static const char *stopName(Stop s) {
    switch (s) {
    case Stop::Unsat:
      return "unsat";
    case Stop::Consistent:
      return "consistent";
    case Stop::Unknown:
      return "unknown";
    case Stop::Rounds:
      return "rounds";
    case Stop::Timeout:
      return "timeout";
    }
    return "?";
  }
};

/// The loop over \p n candidates, at most \p maxRounds solves. \p solve
/// solves with the kept set it is given (K only grows from one call to the
/// next: an incremental \p solve asserts what joined it), fills the round
/// (result, effort) and, on sat, the omitted candidates its model breaks
/// (\p broken: indices into the n; each one not kept). A sat that names no
/// candidate ends the loop (Consistent). Every broken candidate joins K (all
/// of them per round: the rounds are full solves, and the measured broken
/// sets are small). \p expired: whether the time budget of the whole loop
/// is gone (checked before each round, and after an unknown one: that
/// round is then a timeout, not an answer, and the stop is Timeout).
inline AckRelaxOutcome runAckRelax(
    size_t n, const AckRelaxSelector &sel, unsigned maxRounds,
    llvm::function_ref<AckRelaxRound(const std::vector<bool> &keep,
                                     std::vector<size_t> &broken)>
        solve,
    llvm::function_ref<bool()> expired = [] { return false; }) {
  using SR = sea::solver::SolverResult;
  AckRelaxOutcome out;
  out.keep = sel.initial(n);
  std::vector<bool> solved = out.keep;
  for (unsigned r = 0; r < maxRounds; ++r) {
    if (expired()) {
      // K stays what the last solve had, not what it would have grown to
      if (!out.rounds.empty())
        out.keep = solved;
      out.stop = AckRelaxOutcome::Stop::Timeout;
      return out;
    }
    std::vector<size_t> broken;
    AckRelaxRound rd = solve(out.keep, broken);
    solved = out.keep;
    rd.kept = out.kept();
    rd.broken = broken.size();
    out.rounds.push_back(rd);
    if (rd.result == SR::UNSAT) {
      out.stop = AckRelaxOutcome::Stop::Unsat;
      return out;
    }
    if (rd.result != SR::SAT) {
      out.stop = expired() ? AckRelaxOutcome::Stop::Timeout
                           : AckRelaxOutcome::Stop::Unknown;
      return out;
    }
    if (broken.empty()) {
      out.stop = AckRelaxOutcome::Stop::Consistent;
      return out;
    }
    // the last round's K stays what was solved
    if (r + 1 == maxRounds)
      break;
    for (size_t i : broken)
      out.keep[i] = true;
  }
  // (after the last round K is not grown: the bound's break)
  out.stop = expired() ? AckRelaxOutcome::Stop::Timeout
                       : AckRelaxOutcome::Stop::Rounds;
  return out;
}

/// The omitted constraints (not \p keep) of \p cs that model \p m breaks:
/// lhs and rhs have one value and r_k, r_l differ. A term without a numeral
/// value counts as broken (adding a constraint is always sound), in
/// \p unevaluated too. Each term is evaluated once.
inline std::vector<size_t> ackRelaxBroken(
    const Ackermannizer &ack, const std::vector<Ackermannizer::Constraint> &cs,
    const std::vector<bool> &keep, sea::solver::Model &m, size_t &unevaluated) {
  std::unordered_map<ENode *, Expr> memo;
  auto value = [&](const Expr &e) -> Expr {
    auto it = memo.find(e.get());
    if (it != memo.end())
      return it->second;
    Expr v = m.eval(e, /*complete=*/true);
    if (v && !op::bv::is_bvnum(v))
      v = Expr();
    memo.emplace(e.get(), v);
    return v;
  };
  auto same = [](const Expr &a, const Expr &b) {
    return ackermann_detail::reducedValue(a) ==
               ackermann_detail::reducedValue(b) &&
           op::bv::widthBvNum(a) == op::bv::widthBvNum(b);
  };
  std::vector<size_t> out;
  unevaluated = 0;
  for (size_t i = 0; i < cs.size(); ++i) {
    if (keep[i])
      continue;
    const Ackermannizer::Constraint &c = cs[i];
    const auto &rs = ack.arrays()[c.array].reads;
    Expr a = value(c.lhs), b = value(c.rhs);
    Expr va = value(rs[c.k].value), vb = value(rs[c.l].value);
    if (!a || !b || !va || !vb) {
      ++unevaluated;
      out.push_back(i);
      continue;
    }
    if (same(a, b) && !same(va, vb))
      out.push_back(i);
  }
  return out;
}

} // namespace expr
