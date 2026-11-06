// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- sea-reduce.cpp - sea test-case reducer -------------------*- C++ -*-===//
//
// mlir-reduce for the sea dialect: the upstream driver (mlirReduceMain,
// reduction-tree, opt-reduction-pass) with the sea dialect and sea's passes
// registered. docs/Reducer.md.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Tools/mlir-reduce/MlirReduceMain.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "sea/Dialect/Sea/SeaDialect.h"
#include "sea/Reduce/Reduce.h"
#include "sea/Support/Diag.h"
#include "sea/Support/Version.h"
#include "sea/Transforms/Passes.h"

static const char *const agentGuide = R"(sea-reduce agent guide (plain, terse)

WHAT: mlir-reduce for sea programs: shrinks an input while an external
interestingness test keeps saying yes. The upstream driver
(mlirReduceMain) with the sea dialect, every sea pass, and sea reduction
patterns (a DialectReductionPatternInterface on the sea dialect).

WHY: a crash, a configuration disagreement or a blowup in a 14k-line
export becomes a small regression test mechanically.

TYPICAL:
  sea-reduce in.mlir --reduction-tree='traversal-mode=0 test=./t.sh' -o out.mlir
                         # 1) delete ranges of ops; 2) apply the sea patterns
                         #   to ranges of ops; a test per variant
  sea-reduce in.mlir --sea-reduction-tree='test=./t.sh pattern=memory-bypass'
                         # 2) alone: the same search, patterns only (no op
                         #   deletion: fast on large CFGs), folding off
  sea-reduce in.mlir --opt-reduction-pass='opt-pass=sea-reduce-cleanup test=./t.sh'
                         # dead ops, unreachable blocks, br chains (all or
                         #   nothing: kept only if still interesting)
  sea-reduce in.mlir --opt-reduction-pass='opt-pass=sea-reduce-apply{pattern=drop-assume} test=./t.sh'
                         # one pattern everywhere at once (cheap first try)
  sea-reduce in.mlir --opt-reduction-pass='opt-pass=sea-drop-fresh-memhavoc test=./t.sh'
                         # run a pass pipeline; kept only if still interesting
                         #   and smaller (any sea or upstream pass, e.g. sccp)
  sea-reduce in.mlir --reduction-tree='test=./t.sh test-arg=--foo'
                         # test-arg: extra arguments before the file

SEA PATTERNS (reduction-tree applies all; sea-opt / sea-reduce
--sea-reduce-apply="pattern=<name>[,..] limit=<n>" applies chosen ones):
  memory-bypass   store/memset/memmove/memhavoc/reset_modified -> its
                  incoming memory
  drop-assume     sea.assume erased
  select-branch   cond_br -> br to one successor (not the one ending in
                  sea.unreachable; else the true one); the other block
                  stays until sea-reduce-cleanup
  load-to-nondet  load -> sea.nondet "sea.reduce" [fresh site] (pointer:
                  sea.ptr.nondet); sites stay unique
  forward-operand value op -> its first operand of the same type
  zero-const      integer op without such an operand (icmp, casts) -> 0
  erase-dead      op with unused results (nondets, memory updates too)
Every candidate verifies; no pattern keeps meaning (the test decides).

DRIVER: utils/reduce-test.py (docs/Reducer.md) writes the test and runs
rounds (global patterns, cleanup, reduction-tree) to a fixpoint:
  utils/reduce-test.py reduce --mode verdict-differs --a=--lower \
      --b="--lower --seam" in.mlir -o small.mlir --log t.log
  utils/reduce-test.py reduce --mode slow --metric time \
      --a="<args A>" --b="<args B>" --a-over 12 --b-under 10 in.mlir -o s.mlir
  modes: crash (--signal n|any, --match regex), verdict-differs (both
  sat/unsat, different), slow (--metric time|mem|<seam-stats counter>,
  A over a-over, B under b-under; enforced by timeout + RLIMIT_AS),
  result (--result sat|unsat|unknown|none [--status n]). Parse, verify
  and sea-vc errors are never interesting. utils/reduce-test.py --agent.

INTERESTINGNESS TEST (mlir/lib/Reducer/Tester.cpp): run as
  <test> [test-arg...] <candidate.mlir>
exit status != 0: interesting (keep); 0: not interesting. A candidate that
does not verify (sea.program verifier: unique sites, typed block args) is
uninteresting without running the test. The test must make parse errors,
verifier errors and any other unrelated failure exit 0.
The input itself must be interesting, else: `uninterested module will not
be reduced` (reduction-tree) / `the original input is not interested`.

FLAGS (sea-reduce's own): --sea-patterns=<name>,... the patterns
reduction-tree uses (default all; `none`: op deletion only);
--show-candidate-errors prints the verifier errors of rejected candidates.

OUTPUT: the reduced module (-o file, default stdout), printed without
locations. stderr: `sea-reduce.diagnostics-suppressed: <n>` and
`sea-reduce.verifier-rejected: <n>` (candidates the MLIR verifier
rejected before the test ran: one error each, verification is
sequential); on failure the last diagnostics, then `sea-reduce: failed`. Status: 0 ok; 1 the input is not interesting, a pass failed or
the input did not parse.
)";

int main(int argc, char **argv) {
  bool showCandidateErrors = false;
  std::vector<char *> args;
  for (int i = 0; i < argc; ++i) {
    llvm::StringRef a(argv[i]);
    if (a == "--agent" || a == "-agent") {
      llvm::outs() << agentGuide;
      return 0;
    }
    // Ours, not mlirReduceMain's (it hides unrelated options).
    if (a == "--show-candidate-errors" || a == "-show-candidate-errors") {
      showCandidateErrors = true;
      continue;
    }
    if (a.consume_front("--sea-patterns=")) {
      llvm::SmallVector<llvm::StringRef> names;
      a.split(names, ',', -1, false);
      for (llvm::StringRef n : names)
        sea::reductionTreePatterns().push_back(n.str());
      continue;
    }
    args.push_back(argv[i]);
  }
  sea::setVersionPrinter("sea-reduce");
  mlir::registerTransformsPasses();
  sea::registerPasses();
  sea::registerReducePasses();
  sea::registerSeaReductionTreePass();
  struct Summary {
    ~Summary() { sea::diag::printSummary(); }
  } summary;

  mlir::DialectRegistry registry;
  registry.insert<sea::SeaDialect>();
  sea::registerSeaReductionInterface(registry);
  mlir::MLIRContext context(registry);
  // Sequential verification stops at a candidate's first error: one error
  // per candidate the verifier rejects (counted below).
  context.disableMultithreading();

  // Every candidate that does not verify reports its errors (upstream
  // Tester verifies before testing): keep only the last few, shown on
  // failure, where they are the reason (parse error, input not
  // interesting).
  std::deque<std::string> last;
  size_t suppressed = 0, rejected = 0;
  std::optional<mlir::ScopedDiagnosticHandler> handler;
  if (!showCandidateErrors)
    handler.emplace(&context, [&](mlir::Diagnostic &d) {
      std::string text;
      llvm::raw_string_ostream os(text);
      os << d.getLocation() << ": " << d;
      last.push_back(text);
      if (last.size() > 4)
        last.pop_front();
      ++suppressed;
      if (d.getSeverity() == mlir::DiagnosticSeverity::Error)
        ++rejected;
      return mlir::success();
    });
  bool ok = mlir::succeeded(mlir::mlirReduceMain(static_cast<int>(args.size()),
                                                 args.data(), context));
  handler.reset();
  if (!ok) {
    for (const std::string &t : last)
      llvm::errs() << t << "\n";
    llvm::errs() << "sea-reduce: failed\n";
    return 1;
  }
  if (!showCandidateErrors)
    llvm::errs() << "sea-reduce.diagnostics-suppressed: " << suppressed
                 << "\nsea-reduce.verifier-rejected: " << rejected << "\n";
  return 0;
}
