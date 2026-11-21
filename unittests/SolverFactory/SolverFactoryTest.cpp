// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SolverFactoryTest.cpp - tests of sea/Expr/Smt/SolverFactory.h ------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if
// a check fails. Run by test/Tools/solver-factory.test. The lines do not
// depend on which optional backends the build has.
//
//  z3          a default solver decides a small query; rlimit 1 makes a
//              factoring query unknown (z3: resource limit), also when set
//              later (setLimits); timeoutMs 1 makes it unknown too; the
//              seam-check setup (relevancy 0, one push)
//              decides; memMB warns (limit-unsupported); an rlimit over
//              2^32-1 is clamped and warns (limit-clamped)
//  optional    Yices and Bitwuzla: when not built, null and the error
//              <prefix>solver-unavailable; when built, a solver, rlimit
//              warns (limit-unsupported); Yices' bad logic / param: null
//              and <prefix>yices-config / <prefix>yices-param
//  bitwuzla    cores: without SolverConfig::unsatCores a solver still
//              decides check / check_with_assumptions; with it,
//              unsat_core gives a subset of the assumptions holding the
//              one that conflicts. `bitwuzla-core-without-option` (an
//              argument): unsat_core on a solver built without it, which
//              must fail with bitwuzla.unsat-core (a fatal error: its own
//              process)
//
//===----------------------------------------------------------------------===//

#include "sea/Expr/Smt/SolverFactory.h"
#include "sea/Expr/Expr.hh"
#include "sea/Support/Diag.h"

#include "llvm/Support/raw_ostream.h"

#include <string>

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

using namespace expr;
using namespace expr::op;
using sea::solver::Backend;
using sea::solver::Limits;
using sea::solver::SolverConfig;
using sea::solver::SolverResult;

struct Terms {
  ExprFactory efac;
  Expr var(const std::string &n, unsigned w) {
    return bv::bvConst(mkTerm<std::string>(n, efac), w);
  }
  Expr num(unsigned long v, unsigned w) {
    return bv::bvnum(mpz_class(v), w, efac);
  }
  /// x + 1 = 3 (sat)
  Expr easy() { return mk<EQ>(mk<BADD>(var("e", 8), num(1, 8)), num(3, 8)); }
  /// x * y = a 64-bit semiprime, both factors > 1 (not decided in a few
  /// resource units)
  Expr hard() {
    Expr x = var("x", 64), y = var("y", 64);
    return mk<AND>(mk<EQ>(mk<BMUL>(x, y), num(1000000016000000063ul, 64)),
                   mk<AND>(mk<BUGT>(x, num(1, 64)), mk<BUGT>(y, num(1, 64))));
  }
};

void testZ3(Terms &T) {
  SolverConfig cfg;
  {
    auto s = sea::solver::makeSolver(T.efac, Backend::Z3);
    CHECK(s && s->add(T.easy()) && s->check() == SolverResult::SAT,
          "z3 default");
  }
  Limits one;
  one.rlimit = 1;
  {
    auto s = sea::solver::makeSolver(T.efac, Backend::Z3, one);
    CHECK(s && s->add(T.hard()) && s->check() == SolverResult::UNKNOWN,
          "z3 rlimit 1");
    CHECK(s && s->reason_unknown().find("resource limit") != std::string::npos,
          "z3 rlimit reason: " << (s ? s->reason_unknown() : ""));
  }
  // timeoutMs reaches z3: 1 ms does not decide the hard query (a limit that
  // never reached z3 would let it run on), at construction and later
  Limits tiny;
  tiny.timeoutMs = 1;
  {
    auto s = sea::solver::makeSolver(T.efac, Backend::Z3, tiny);
    CHECK(s && s->add(T.hard()) && s->check() == SolverResult::UNKNOWN,
          "z3 timeout 1 ms");
  }
  {
    auto s = sea::solver::makeSolver(T.efac, Backend::Z3);
    CHECK(s && s->add(T.hard()), "z3 add");
    sea::solver::setLimits(*s, tiny);
    CHECK(s->check() == SolverResult::UNKNOWN, "z3 setLimits timeout 1 ms");
  }
  {
    auto s = sea::solver::makeSolver(T.efac, Backend::Z3);
    CHECK(s && s->add(T.hard()), "z3 add");
    sea::solver::setLimits(*s, one);
    CHECK(s->check() == SolverResult::UNKNOWN, "z3 setLimits rlimit 1");
  }
  // an rlimit over z3's 32-bit parameter: clamped to its largest, said, not
  // truncated (2^32 + 1 would be 1 in 32 bits); at construction and later
  {
    Limits big;
    big.rlimit = (uint64_t(1) << 32) + 1;
    unsigned before = sea::diag::count("smt.limit-clamped");
    auto s = sea::solver::makeSolver(T.efac, Backend::Z3, big);
    CHECK(sea::diag::count("smt.limit-clamped") == before + 1,
          "z3 rlimit 2^32+1 warns");
    CHECK(s && s->add(T.easy()) && s->check() == SolverResult::SAT,
          "z3 rlimit 2^32+1 decides");
    sea::solver::setLimits(*s, big);
    CHECK(sea::diag::count("smt.limit-clamped") == before + 2,
          "z3 setLimits rlimit 2^32+1 warns");
    CHECK(s->check() == SolverResult::SAT,
          "z3 setLimits rlimit 2^32+1 decides");
    // the largest z3 takes: no warning
    Limits max;
    max.rlimit = 4294967295u;
    auto m = sea::solver::makeSolver(T.efac, Backend::Z3, max);
    CHECK(m && sea::diag::count("smt.limit-clamped") == before + 2,
          "z3 rlimit 2^32-1 does not warn");
  }
  {
    SolverConfig c;
    c.z3Relevancy0Push = true;
    auto s = sea::solver::makeSolver(T.efac, Backend::Z3, Limits(), c);
    // relevancy 0 is not observable here: the Solver interface reads back no
    // parameter value, and the answer does not depend on it; the check is
    // that the setup is accepted and still decides
    CHECK(s && s->add(T.easy()) && s->check() == SolverResult::SAT,
          "z3 relevancy 0 + push");
  }
  {
    Limits mem;
    mem.memMB = 1024;
    unsigned before = sea::diag::count("smt.limit-unsupported");
    auto s = sea::solver::makeSolver(T.efac, Backend::Z3, mem);
    CHECK(s, "z3 with memMB");
    CHECK(sea::diag::count("smt.limit-unsupported") == before + 1,
          "z3 memMB warns");
  }
  llvm::outs() << "z3: ok\n";
}

void testOptional(Terms &T, Backend b) {
  SolverConfig cfg;
  cfg.kindPrefix = "t.";
  const char *name = sea::solver::backendName(b);
  if (!sea::solver::available(b)) {
    unsigned before = sea::diag::count("t.solver-unavailable");
    auto s = sea::solver::makeSolver(T.efac, b, Limits(), cfg);
    CHECK(!s, name << " unavailable: null");
    CHECK(sea::diag::count("t.solver-unavailable") == before + 1,
          name << " unavailable: error");
    llvm::outs() << name << ": ok\n";
    return;
  }
  {
    auto s = sea::solver::makeSolver(T.efac, b, Limits(), cfg);
    CHECK(s && s->add(T.easy()) && s->check() == SolverResult::SAT,
          name << " default");
  }
  {
    Limits l;
    l.rlimit = 1;
    unsigned before = sea::diag::count("t.limit-unsupported");
    auto s = sea::solver::makeSolver(T.efac, b, l, cfg);
    CHECK(s && s->add(T.easy()) && s->check() == SolverResult::SAT,
          name << " rlimit ignored");
    CHECK(sea::diag::count("t.limit-unsupported") == before + 1,
          name << " rlimit warns");
  }
  if (b == Backend::Yices) {
    SolverConfig bad = cfg;
    bad.yicesLogic = "NO_SUCH_LOGIC";
    unsigned before = sea::diag::count("t.yices-config");
    CHECK(!sea::solver::makeSolver(T.efac, b, Limits(), bad),
          "yices bad logic: null");
    CHECK(sea::diag::count("t.yices-config") == before + 1,
          "yices bad logic: error");
    bad = cfg;
    bad.yicesParams = {"no-such-param=1"};
    before = sea::diag::count("t.yices-param");
    CHECK(!sea::solver::makeSolver(T.efac, b, Limits(), bad),
          "yices bad param: null");
    CHECK(sea::diag::count("t.yices-param") == before + 1,
          "yices bad param: error");
  }
  llvm::outs() << name << ": ok\n";
}

void testBitwuzlaCores(Terms &T) {
  if (!sea::solver::available(Backend::Bitwuzla)) {
    llvm::outs() << "bitwuzla cores: ok\n";
    return;
  }
  Expr x = T.var("cx", 8), y = T.var("cy", 8);
  Expr fact = mk<EQ>(x, T.num(1, 8));
  Expr conflict = mk<EQ>(x, T.num(2, 8)), free = mk<EQ>(y, T.num(0, 8));
  ExprVector unsatLits{conflict, free}, satLits{free};
  auto range = [](const ExprVector &v) {
    return llvm::make_range(v.cbegin(), v.cend());
  };
  {
    // no core option: the checks (also with assumptions) still work
    auto s = sea::solver::makeSolver(T.efac, Backend::Bitwuzla);
    CHECK(s && s->add(fact), "bitwuzla no-core add");
    if (!s)
      return;
    CHECK(s->check() == SolverResult::SAT, "bitwuzla no-core check");
    CHECK(s->check_with_assumptions(range(unsatLits)) == SolverResult::UNSAT,
          "bitwuzla no-core assumptions unsat");
    CHECK(s->check_with_assumptions(range(satLits)) == SolverResult::SAT,
          "bitwuzla no-core assumptions sat");
  }
  {
    SolverConfig c;
    c.unsatCores = true;
    auto s = sea::solver::makeSolver(T.efac, Backend::Bitwuzla, Limits(), c);
    CHECK(s && s->add(fact), "bitwuzla core add");
    if (!s)
      return;
    // a core only after unsat (else unsat_core is a fatal error, not a FAIL)
    SolverResult r = s->check_with_assumptions(range(unsatLits));
    CHECK(r == SolverResult::UNSAT, "bitwuzla core assumptions unsat");
    if (r != SolverResult::UNSAT)
      return;
    ExprVector core;
    s->unsat_core(core);
    bool subset = true, hasConflict = false;
    for (Expr e : core) {
      subset &= e == conflict || e == free;
      hasConflict |= e == conflict;
    }
    CHECK(subset && hasConflict, "bitwuzla core: subset with the conflict");
  }
  llvm::outs() << "bitwuzla cores: ok\n";
}

/// unsat_core on a Bitwuzla solver built without the core option: does not
/// return
int bitwuzlaCoreWithoutOption(Terms &T) {
  auto s = sea::solver::makeSolver(T.efac, Backend::Bitwuzla);
  if (!s)
    return 1;
  Expr x = T.var("cx", 8);
  ExprVector lits{mk<EQ>(x, T.num(2, 8))};
  s->add(mk<EQ>(x, T.num(1, 8)));
  if (s->check_with_assumptions(llvm::make_range(lits.cbegin(), lits.cend())) !=
      SolverResult::UNSAT)
    return 1;
  ExprVector core;
  s->unsat_core(core);
  llvm::outs() << "core size: " << core.size() << "\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  Terms T;
  if (argc > 1 && std::string(argv[1]) == "bitwuzla-core-without-option")
    return bitwuzlaCoreWithoutOption(T);
  testZ3(T);
  testOptional(T, Backend::Yices);
  testOptional(T, Backend::Bitwuzla);
  testBitwuzlaCores(T);
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
