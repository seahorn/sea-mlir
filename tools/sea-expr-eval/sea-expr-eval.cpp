// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- sea-expr-eval.cpp - evaluate bit-vector terms through Expr -*- C++
//-*-===//
//
// Test utility for the Expr port. Reads one closed bit-vector term per line,
// builds it as an Expr, and evaluates all of them with one solver call: each
// term t_i gets a fresh constant x_i with the assertion x_i = t_i, and the
// value of x_i is read from the model. This exercises Expr construction and
// the solver marshalling (Expr -> z3 / Yices / Bitwuzla -> model -> Expr).
//
// Input lines (values are unsigned decimal):
//   bin <smt-op> <w> <a> <b>     bvadd, bvsub, ..., bvashr
//   cmp <smt-op> <w> <a> <b>     =, distinct, bvult, ...; result is 0/1
//   zext|sext <w> <w2> <a>       extend a from w to w2 bits
//   trunc <w> <w2> <a>           low w2 bits of a
//   ite <w> <c> <a> <b>          c is 0/1
//   concat <w> <w2> <a> <b>      a above b (w + w2 bits)
//   extract <w> <low> <ew> <a>   ew bits of a from bit low
//   mulo u|s <w> <a> <b>         the VC's native mul overflow; result 0/1
// Output: one unsigned decimal value per input line, in order.
//
//===----------------------------------------------------------------------===//

#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprAPInt.hh"
#include "sea/Expr/ExprOverflow.hh"
#include "sea/Expr/Smt/Yices2SolverImpl.hh"
#ifdef WITH_BITWUZLA
#include "sea/Expr/Smt/BitwuzlaSolverImpl.hh"
#endif
#include "sea/Expr/Smt/Z3SolverImpl.hh"

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>

using namespace expr;
using namespace expr::op;
using namespace llvm;

static cl::opt<std::string> inputFilename(cl::Positional, cl::desc("<input>"),
                                          cl::init("-"));

enum class Backend { Z3, Yices, Bitwuzla };
static cl::opt<Backend>
    backend("solver", cl::desc("Solver to evaluate with"),
            cl::init(Backend::Z3),
            cl::values(clEnumValN(Backend::Z3, "z3", "z3"),
                       clEnumValN(Backend::Yices, "yices", "Yices 2"),
                       clEnumValN(Backend::Bitwuzla, "bitwuzla",
                                  "Bitwuzla (if built with BITWUZLA_ROOT)")));

static cl::opt<bool> agent("agent",
                           cl::desc("Print a terse guide for agents and exit"),
                           cl::init(false));

static const char *const agentGuide =
    R"(sea-expr-eval agent guide (plain, terse)

WHAT: test utility for the Expr port. Builds closed bit-vector terms as
Expr, solves them with z3, Yices or Bitwuzla through sea's marshalling, and
prints the model values. utils/check-semantics.py compares its output with z3's
own SMT-LIB evaluation and with sea-run.

TYPICAL:
  printf 'bin bvadd 8 200 100\n' | sea-expr-eval --solver=yices   # 44
  (--solver=bitwuzla works only if built with -DBITWUZLA_ROOT=<install>)

INPUT (one term per line, unsigned decimal values):
  bin <bvadd|bvsub|...|bvashr> <w> <a> <b>
  cmp <=|distinct|bvult|...> <w> <a> <b>     (prints 0 or 1)
  zext|sext <w> <w2> <a>    trunc <w> <w2> <a>    ite <w> <c> <a> <b>

OUTPUT: one unsigned decimal value per input line, in order.
STATUS CODES: 0 ok; 1 bad input or solver did not return sat.
)";

static int fail(const Twine &msg) {
  errs() << "sea-expr-eval: " << msg << "\n";
  return 1;
}

namespace {
struct TermBuilder {
  ExprFactory &efac;
  Expr trueE, falseE;

  explicit TermBuilder(ExprFactory &efac)
      : efac(efac), trueE(mk<TRUE>(efac)), falseE(mk<FALSE>(efac)) {}

  Expr num(StringRef text, unsigned w) {
    return bv::bvnum(mpz_class(text.str()), w, efac);
  }

  /// Returns the term and its result width, or nullptr on bad input.
  Expr build(StringRef line, unsigned &width) {
    SmallVector<StringRef> f;
    line.split(f, ' ', -1, /*KeepEmpty=*/false);
    auto u = [&](size_t i) {
      unsigned v = 0;
      return i < f.size() && !f[i].getAsInteger(10, v) ? v : 0u;
    };
    if (f.size() == 5 && f[0] == "bin") {
      unsigned w = width = u(2);
      Expr a = num(f[3], w), b = num(f[4], w);
      static const StringMap<std::function<Expr(Expr, Expr)>> ops = {
          {"bvadd", [](Expr a, Expr b) { return mk<BADD>(a, b); }},
          {"bvsub", [](Expr a, Expr b) { return mk<BSUB>(a, b); }},
          {"bvmul", [](Expr a, Expr b) { return mk<BMUL>(a, b); }},
          {"bvudiv", [](Expr a, Expr b) { return mk<BUDIV>(a, b); }},
          {"bvsdiv", [](Expr a, Expr b) { return mk<BSDIV>(a, b); }},
          {"bvurem", [](Expr a, Expr b) { return mk<BUREM>(a, b); }},
          {"bvsrem", [](Expr a, Expr b) { return mk<BSREM>(a, b); }},
          {"bvand", [](Expr a, Expr b) { return mk<BAND>(a, b); }},
          {"bvor", [](Expr a, Expr b) { return mk<BOR>(a, b); }},
          {"bvxor", [](Expr a, Expr b) { return mk<BXOR>(a, b); }},
          {"bvshl", [](Expr a, Expr b) { return mk<BSHL>(a, b); }},
          {"bvlshr", [](Expr a, Expr b) { return mk<BLSHR>(a, b); }},
          {"bvashr", [](Expr a, Expr b) { return mk<BASHR>(a, b); }},
      };
      auto it = ops.find(f[1]);
      return it == ops.end() ? Expr() : it->second(a, b);
    }
    if (f.size() == 5 && f[0] == "mulo" && (f[1] == "u" || f[1] == "s")) {
      unsigned w = u(2);
      width = 1;
      return mk<ITE>(bv::mulOverflows(f[1] == "s", num(f[3], w), num(f[4], w)),
                     num("1", 1), num("0", 1));
    }
    if (f.size() == 5 && f[0] == "cmp") {
      unsigned w = u(2);
      width = 1;
      Expr a = num(f[3], w), b = num(f[4], w);
      static const StringMap<std::function<Expr(Expr, Expr)>> ops = {
          {"=", [](Expr a, Expr b) { return mk<EQ>(a, b); }},
          {"distinct", [](Expr a, Expr b) { return mk<NEQ>(a, b); }},
          {"bvult", [](Expr a, Expr b) { return mk<BULT>(a, b); }},
          {"bvule", [](Expr a, Expr b) { return mk<BULE>(a, b); }},
          {"bvugt", [](Expr a, Expr b) { return mk<BUGT>(a, b); }},
          {"bvuge", [](Expr a, Expr b) { return mk<BUGE>(a, b); }},
          {"bvslt", [](Expr a, Expr b) { return mk<BSLT>(a, b); }},
          {"bvsle", [](Expr a, Expr b) { return mk<BSLE>(a, b); }},
          {"bvsgt", [](Expr a, Expr b) { return mk<BSGT>(a, b); }},
          {"bvsge", [](Expr a, Expr b) { return mk<BSGE>(a, b); }},
      };
      auto it = ops.find(f[1]);
      if (it == ops.end())
        return Expr();
      // A Bool result, returned as a bv1 so every line has a bit-vector value.
      return mk<ITE>(it->second(a, b), num("1", 1), num("0", 1));
    }
    if (f.size() == 4 && (f[0] == "zext" || f[0] == "sext")) {
      unsigned w = u(1);
      width = u(2);
      Expr a = num(f[3], w);
      return f[0] == "zext" ? bv::zext(a, width) : bv::sext(a, width);
    }
    if (f.size() == 4 && f[0] == "trunc") {
      unsigned w = u(1);
      width = u(2);
      return bv::extract(width - 1, 0, num(f[3], w));
    }
    if (f.size() == 5 && f[0] == "concat") {
      unsigned w = u(1), w2 = u(2);
      width = w + w2;
      return bv::concat(num(f[3], w), num(f[4], w2));
    }
    if (f.size() == 5 && f[0] == "extract") {
      unsigned w = u(1), low = u(2);
      width = u(3);
      return bv::extract(low + width - 1, low, num(f[4], w));
    }
    if (f.size() == 5 && f[0] == "ite") {
      unsigned w = width = u(1);
      Expr c = mk<EQ>(num(f[2], 1), num("1", 1));
      return mk<ITE>(c, num(f[3], w), num(f[4], w));
    }
    return Expr();
  }
};
} // namespace

int main(int argc, char **argv) {
  InitLLVM y(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Evaluate bv terms through Expr\n");
  if (agent) {
    outs() << agentGuide;
    return 0;
  }

  auto buf = MemoryBuffer::getFileOrSTDIN(inputFilename);
  if (!buf)
    return fail("cannot read " + inputFilename);

  ExprFactory efac;
  TermBuilder tb(efac);
  std::unique_ptr<sea::solver::Solver> solver;
  if (backend == Backend::Z3) {
    solver = std::make_unique<sea::solver::z3_solver_impl>(efac);
  } else if (backend == Backend::Bitwuzla) {
#ifdef WITH_BITWUZLA
    solver = std::make_unique<sea::solver::bitwuzla_solver_impl>(efac);
#else
    return fail("built without Bitwuzla (configure with -DBITWUZLA_ROOT)");
#endif
  } else {
#ifdef WITH_YICES2
    solver = std::make_unique<sea::solver::yices_solver_impl>(efac);
#else
    return fail("built without Yices (configure with -DSEA_ENABLE_YICES=ON "
                "-DYICES2_ROOT=<install>)");
#endif
  }

  ExprVector vars;
  for (line_iterator it(**buf, /*SkipBlanks=*/true); !it.is_at_eof(); ++it) {
    unsigned width = 0;
    Expr t = tb.build(*it, width);
    if (!t || width == 0)
      return fail("bad input line " + Twine(it.line_number()) + ": " + *it);
    Expr x = bv::bvConst(
        mkTerm<std::string>("x" + std::to_string(vars.size()), efac), width);
    vars.push_back(x);
    if (!solver->add(mk<EQ>(x, t)))
      return fail("the solver cannot encode line " + Twine(it.line_number()));
  }

  if (solver->check() != sea::solver::SolverResult::SAT)
    return fail("solver did not return sat");
  auto model = solver->get_model();
  for (Expr x : vars) {
    Expr v = model->eval(x, /*complete=*/true);
    if (!bv::is_bvnum(v))
      return fail("model value is not a numeral: " +
                  boost::lexical_cast<std::string>(*v));
    mpz_class n = bv::toMpz(v);
    // Normalize to [0, 2^w): a backend may return a signed numeral.
    mpz_class mod = mpz_class(1UL) << bv::widthBvNum(v);
    n = ((n % mod) + mod) % mod;
    outs() << n.to_string() << "\n";
  }
  return 0;
}
