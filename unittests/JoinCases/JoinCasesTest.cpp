// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- JoinCasesTest.cpp - tests of the cases joins and counters ----------===//
//
// Plain-main driver (no gtest): builds joins through sea::JoinCases (the
// experimental sea-vc --vc-join-cases), checks that each returns the lite
// chain VCGen builds without it and that a join the cases constructor refuses
// gives its reason, then prints the counters (`cases.*` lines) of a fixed set
// of joins for test/VC/join-cases-counters.test to pin, and `failures: N`.
//
// The rearrangements (--vc-cases-reorder, --vc-cases-drop-default): for every
// pattern of values over 1..6 pairs and a default (every equality pattern of
// the n + 1 values, symbolic Boolean conditions), each combination of the two
// rearrangements is checked exhaustively, over every assignment of the
// conditions, to give the first-match value of the original term, and to leave
// no two pairs with one value (merge) and no pair equal to the default
// (drop-default). The evaluator below is independent of Expr's helpers: it
// reads AND / OR / NEG / ITE / TRUE / FALSE and the condition constants.
// Then the counters of fixed rearranged joins are printed (`reorder.` prefix).
//
//===----------------------------------------------------------------------===//

#include "sea/VC/JoinCases.h"
#include "sea/Support/Stats.h"

#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace expr;
using namespace expr::op;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      llvm::errs() << "FAIL: " << msg << " [" #cond "] (line " << __LINE__     \
                   << ")\n";                                                   \
    }                                                                          \
  } while (0)

/// VCGen's ite join without the option (VCGen.cpp, computePathCondForBb)
Expr iteJoin(const ExprVector &edges, const ExprVector &vals) {
  Expr val = vals.back();
  for (size_t j = vals.size() - 1; j > 0; --j)
    val = boolop::lite(edges[j - 1], vals[j - 1], val);
  return val;
}

std::string str(Expr e) {
  std::ostringstream os;
  os << *e;
  return os.str();
}

/// The value of a Boolean term under \p asg (condition constant -> value);
/// the term is built from the conditions by the rearrangements only.
bool evalBool(Expr e, const std::map<Expr, bool> &asg) {
  if (isOpX<TRUE>(e))
    return true;
  if (isOpX<FALSE>(e))
    return false;
  if (isOpX<NEG>(e))
    return !evalBool(e->left(), asg);
  if (isOpX<AND>(e)) {
    for (size_t i = 0; i < e->arity(); ++i)
      if (!evalBool(e->arg(i), asg))
        return false;
    return true;
  }
  if (isOpX<OR>(e)) {
    for (size_t i = 0; i < e->arity(); ++i)
      if (evalBool(e->arg(i), asg))
        return true;
    return false;
  }
  auto it = asg.find(e);
  if (it == asg.end()) {
    ++failures;
    llvm::errs() << "FAIL: unexpected condition term " << str(e) << "\n";
    return false;
  }
  return it->second;
}

/// The value (a leaf) an ite chain picks under \p asg.
Expr evalIte(Expr e, const std::map<Expr, bool> &asg) {
  while (isOpX<ITE>(e))
    e = evalBool(e->arg(0), asg) ? e->arg(1) : e->arg(2);
  return e;
}

/// The first-match value of pairs (edges[j], vals[j]), default vals.back().
Expr firstMatch(const ExprVector &edges, const ExprVector &vals,
                const std::map<Expr, bool> &asg) {
  for (size_t j = 0; j + 1 < vals.size(); ++j)
    if (asg.at(edges[j]))
      return vals[j];
  return vals.back();
}

/// Every equality pattern of n values (restricted growth strings).
void patterns(size_t n, std::vector<unsigned> &cur, unsigned mx,
              std::vector<std::vector<unsigned>> &out) {
  if (cur.size() == n) {
    out.push_back(cur);
    return;
  }
  for (unsigned k = 0; k <= mx + 1; ++k) {
    cur.push_back(k);
    patterns(n, cur, std::max(mx, k), out);
    cur.pop_back();
  }
}

/// The exhaustive check of the rearrangements; returns the joins checked.
unsigned checkReorder(ExprFactory &efac) {
  unsigned checked = 0;
  TypeChecker tc;
  ExprVector conds, leaves;
  for (unsigned i = 0; i < 6; ++i)
    conds.push_back(
        bind::boolConst(mkTerm<std::string>("c" + std::to_string(i), efac)));
  for (unsigned i = 0; i < 7; ++i)
    leaves.push_back(
        bv::bvConst(mkTerm<std::string>("x" + std::to_string(i), efac), 8));
  for (size_t m = 1; m <= 6; ++m) {
    std::vector<std::vector<unsigned>> pats;
    std::vector<unsigned> cur{0};
    patterns(m + 1, cur, 0, pats);
    ExprVector edges(conds.begin(), conds.begin() + m);
    edges.push_back(mk<TRUE>(efac)); // the default edge, unused
    for (const auto &pat : pats) {
      ExprVector vals;
      for (unsigned k : pat)
        vals.push_back(leaves[k]);
      for (unsigned mode = 1; mode < 4; ++mode) {
        sea::JoinCases::Reorder r{(mode & 1) != 0, (mode & 2) != 0};
        sea::JoinCases jc(r);
        std::string why;
        Expr got = jc.join(edges, vals, sea::JoinKind::Value, why);
        ++checked;
        if (!got) {
          CHECK(false, "join refused: " << why);
          continue;
        }
        // the rearranged term, for its shape
        ExprVector args;
        for (size_t j = 0; j < m; ++j) {
          args.push_back(edges[j]);
          args.push_back(vals[j]);
        }
        args.push_back(vals.back());
        Expr c = op::cases::make(args, tc, why);
        Expr re = jc.reorder(c, r, sea::JoinKind::Value);
        if (isOpX<CASES>(re)) {
          const size_t np = re->arity() / 2;
          for (size_t a = 0; a < np; ++a) {
            if (r.dropDefault)
              CHECK(re->arg(2 * a + 1) != vals.back(),
                    "a pair equal to the default is left");
            if (r.merge)
              for (size_t b = a + 1; b < np; ++b)
                CHECK(re->arg(2 * a + 1) != re->arg(2 * b + 1),
                      "two pairs with one value are left");
          }
        }
        for (unsigned bits = 0; bits < (1u << m); ++bits) {
          std::map<Expr, bool> asg;
          for (size_t j = 0; j < m; ++j)
            asg[conds[j]] = (bits >> j) & 1;
          Expr want = firstMatch(edges, vals, asg);
          CHECK(evalIte(got, asg) == want,
                "rearranged join differs: m=" << m << " mode=" << mode
                                              << " bits=" << bits);
          if (isOpX<CASES>(re))
            CHECK(evalIte(op::cases::expand(re), asg) == want,
                  "rearranged term differs");
          else
            CHECK(re == want, "no pair left: the default");
        }
      }
    }
  }
  return checked;
}

} // namespace

/// The `cases.*` lines of \p jc on stdout.
static void printCounters(const sea::JoinCases &jc) {
  sea::stats::Report r;
  jc.report(r);
  r.print(llvm::outs(), false);
}

int main(int argc, char **argv) {
  ExprFactory efac;
  auto b = [&](const std::string &n) {
    return bind::boolConst(mkTerm<std::string>(n, efac));
  };
  auto bv = [&](const std::string &n, unsigned w) {
    return bv::bvConst(mkTerm<std::string>(n, efac), w);
  };
  Expr e0 = b("e0"), e1 = b("e1"), e2 = b("e2"), e3 = b("e3");
  Expr x = bv("x", 8), y = bv("y", 8), w = bv("w", 16);
  {
    sea::JoinCases jc;
    std::string why;
    auto join = [&](const ExprVector &edges, const ExprVector &vals,
                    sea::JoinKind k) {
      Expr r = jc.join(edges, vals, k, why);
      CHECK(r == iteJoin(edges, vals), "join is the ite join");
    };
    // the joins whose counters the lit test pins
    join({e0, e1}, {x, y}, sea::JoinKind::Value);         // distinct
    join({e0, e1}, {x, x}, sea::JoinKind::Value);         // all equal
    join({e0, e1, e2}, {x, y, x}, sea::JoinKind::Value);  // non-adjacent
    join({e0, e1, e2}, {x, x, y}, sea::JoinKind::Memory); // adjacent, not tail
    join({e0, e1, e2, e3}, {y, x, x, x},
         sea::JoinKind::Companion); // a run equal to the default
    join({mk<TRUE>(efac), e1}, {x, y}, sea::JoinKind::Value); // literal edge
    join({e0}, {x}, sea::JoinKind::Value); // one edge: no cases, not counted
    CHECK(jc.join({e0}, {x}, sea::JoinKind::Value, why) == x, "one edge");

    // a refused join: null and the reason; not counted
    why.clear();
    CHECK(!jc.join({e0, e1}, {w, x}, sea::JoinKind::Value, why),
          "mixed sorts refused");
    CHECK(why.find("value 0 has a sort other") != std::string::npos,
          "refusal reason: " << why);
    why.clear();
    CHECK(!jc.join({x, e1}, {y, x}, sea::JoinKind::Value, why),
          "bit-vector edge refused");
    CHECK(why.find("condition 0 is not Boolean") != std::string::npos,
          "refusal reason: " << why);
    printCounters(jc);
  }
  if (argc > 1 && std::string(argv[1]) == "reorder") {
    llvm::outs() << "reorder.exhaustive: " << checkReorder(efac) << "\n";
    std::string why;
    {
      // merge only
      sea::JoinCases jc({true, false});
      Expr r =
          jc.join({e0, e1, e2, e3}, {x, y, x, y}, sea::JoinKind::Value, why);
      // pair 2 moves ahead of pair 1: e0 | (e2 & !e1); then the pair of y
      // before the default y folds
      CHECK(r == mk<ITE>(mk<OR>(e0, mk<AND>(e2, mk<NEG>(e1))), x, y),
            "merged join: " << str(r));
      r = jc.join({e0, e1, e2}, {x, x, y}, sea::JoinKind::Memory, why);
      CHECK(r == mk<ITE>(mk<OR>(e0, e1), x, y), "adjacent merge: " << str(r));
      // a run equal to the default only: unchanged
      r = jc.join({e0, e1, e2, e3}, {y, x, x, x}, sea::JoinKind::Companion,
                  why);
      CHECK(r == mk<ITE>(e0, y, x), "default run: " << str(r));
      r = jc.join({e0, e1}, {x, y}, sea::JoinKind::Value, why);
      CHECK(r == mk<ITE>(e0, x, y), "distinct: " << str(r));
      llvm::outs() << "reorder: merge\n";
      printCounters(jc);
    }
    {
      // merge and drop-default
      sea::JoinCases jc({true, true});
      Expr r = jc.join({e0, e1, e2}, {x, y, x}, sea::JoinKind::Value, why);
      CHECK(r == mk<ITE>(mk<AND>(e1, mk<NEG>(e0)), y, x),
            "dropped pair: " << str(r));
      Expr z = bv("z", 8);
      r = jc.join({e0, e1, e2, e3, e3}, {x, y, y, x, z}, sea::JoinKind::Memory,
                  why);
      CHECK(r == mk<ITE>(mk<OR>(e0, mk<AND>(e3, mk<NEG>(mk<OR>(e1, e2)))), x,
                         mk<ITE>(mk<OR>(e1, e2), y, z)),
            "two merges: " << str(r));
      r = jc.join({e0, e1}, {x, x}, sea::JoinKind::Companion, why);
      CHECK(r == x, "all equal: " << str(r));
      llvm::outs() << "reorder: merge drop-default\n";
      printCounters(jc);
    }
  }
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
