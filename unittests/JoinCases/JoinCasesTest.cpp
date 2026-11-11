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
//===----------------------------------------------------------------------===//

#include "sea/VC/JoinCases.h"

#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <string>

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

} // namespace

int main() {
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
    jc.print(llvm::outs());
  }
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
