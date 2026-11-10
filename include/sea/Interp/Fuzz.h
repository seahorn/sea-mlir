// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Fuzz.h - greybox fuzzing over the interpreter ------------*- C++ -*-===//
//
// sea-run --fuzz (docs/reference/fuzzing.md): runs one program many times
// in-process, each trial on nondet values made by mutating a corpus entry,
// and keeps an input that reaches new coverage (a block, an edge between
// blocks, an assume that held) as a corpus entry. The interpreter is the
// golden semantics, so a trial is a concrete run of the same model the VC
// encodes.
//
// A corpus entry is a list of assignments in sea-run's --input syntax: a
// nondet site's value (`<site>=<value>`) or a memory cell's
// (`<site>@<addr>=<value>`, `<site>@<object>+<offset>=<value>`). A corpus
// file holds one per line; it may also hold the `nondet:` and `memread:`
// lines of a sea-run or sea-vc output, so a saved run is a seed.
//
// A site has one value per trial, as with --input: a site read again (in a
// loop) reads the same value, so every trial replays through --input-file.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_INTERP_FUZZ_H
#define SEA_INTERP_FUZZ_H

#include "sea/Interp/Interpreter.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sea {

/// One assignment: the value of a nondet site (`cell` empty) or of a memory
/// cell of the memory created at `site` (`cell`: the address in unsigned
/// decimal, or `<object>+<offset>` with the offset in unsigned decimal, as
/// InputSource::setCell keys it).
struct Assignment {
  uint64_t site = 0;
  std::string cell;
  std::string value;
};

/// Parses `<site>=<value>`, `<site>@<addr>=<value>` or
/// `<site>@<object>+<offset>=<value>` (the --input syntax). On failure,
/// `error` is the expected syntax.
mlir::LogicalResult parseAssignment(llvm::StringRef text, Assignment &out,
                                    std::string &error);

/// Gives each assignment to `source` (set / setCell).
void addAssignments(InputSource &source, llvm::ArrayRef<Assignment> as);

/// A corpus file: its assignments, and the number of `key: ...` lines of a
/// saved run that are not inputs (program:, status:, ...), skipped.
struct CorpusEntryText {
  std::string name; // the file
  std::vector<Assignment> assignments;
  unsigned skippedLines = 0;
};

/// Parses a corpus file's text: one assignment per line in --input syntax,
/// or a `nondet: site=.. value=..` / `memread: site=.. addr=.. value=..`
/// record line; blank lines and `#` comments are skipped, as are other
/// `key: ...` lines (counted). Anything else is an error (with its line).
mlir::LogicalResult parseCorpusText(llvm::StringRef text, CorpusEntryText &out,
                                    std::string &error);

/// Reads corpus files: a file, or every regular file of a directory (not
/// recursive), in name order.
mlir::LogicalResult readCorpus(llvm::StringRef path,
                               std::vector<CorpusEntryText> &out,
                               std::string &error);

struct FuzzOptions {
  /// Stop after this many trials (0: no bound), or after `seconds` of wall
  /// clock (0: no bound); at least one is set.
  uint64_t trials = 0;
  double seconds = 0;
  uint64_t seed = 0;
  /// Use the dictionary (the program's comparison and arithmetic
  /// constants, +-1).
  bool dictionary = true;
  /// Count failures (assert failures, memory-safety findings) instead of
  /// stopping at the first.
  bool keepGoing = false;
  /// RunOptions::deferMemoryErrors in every trial: a memory-safety
  /// violation on a run that reaches the end or a failing assert is a
  /// failure (status memory-error); on a run an assume blocks, nothing.
  bool deferMemoryErrors = false;
  /// RunOptions::addr0IsNull in every trial.
  bool addr0IsNull = true;
  /// RunOptions::memhavocNullNoop in every trial.
  bool memhavocNullNoop = true;
  /// Write failing and new-coverage inputs here (empty: none). The
  /// directory must be new or empty.
  std::string outDir;
  /// Per-trial interpreter options (maxSteps; prints are off in trials).
  uint64_t maxSteps = 1'000'000;
  /// Reach coverage (sea-run --reach-targets, sea-vc --reach-corpus): the
  /// target ops (ReachTargets.h); FuzzReport::reachHits says which trial
  /// first reached the top of each. Not with deferMemoryErrors (past a
  /// substituted violation a run is not an execution of the program).
  std::vector<mlir::Operation *> reachTargets;
};

/// The first trial that reached a reach target (FuzzOptions::reachTargets):
/// it was about to execute the target's op, every earlier assume having
/// held. The run's prefix up to there is exact, whatever it did after (a
/// failing assume or assert, a memory-safety stop, a limit).
struct ReachHit {
  /// 1-based; 0: no trial reached the target
  uint64_t trial = 0;
  /// a seed trial: its corpus file (CorpusEntryText::name), else empty
  std::string seed;
  /// the values the run read (a corpus file of them replays it)
  std::vector<Assignment> input;
  /// the run's uses of addr0-is-null and of memhavoc(null) = no-op
  uint64_t addr0IsNullUses = 0, memhavocNullNoopUses = 0;
};

/// The trials that failed at one op (a failing assert, a reached memory-safety
/// violation) or stopped with an error at one op: the first, and how many.
struct FuzzFailure {
  uint64_t trial = 0;
  RunStatus status = RunStatus::Error;
  mlir::Operation *at = nullptr;
  std::string message;
  std::vector<Assignment> input;
  std::string file;   // in FuzzOptions::outDir, if set
  uint64_t count = 0; // trials that stopped the same way
};

/// The number of RunStatus values.
constexpr unsigned kRunStatusCount =
    static_cast<unsigned>(RunStatus::Error) + 1;

struct FuzzReport {
  uint64_t trials = 0;
  uint64_t seedTrials = 0; // of which the corpus seeds' runs
  double seconds = 0;
  /// Runs per status (RunStatus order).
  uint64_t statusCount[kRunStatusCount] = {};
  unsigned blocksHit = 0, blocksTotal = 0;
  unsigned edgesHit = 0;
  unsigned assumesPassed = 0, assumesTotal = 0;
  uint64_t corpusSize = 0;   // entries in the corpus at the end
  uint64_t filesWritten = 0; // in outDir
  unsigned dictionarySize = 0;
  /// Failures, by the op that ended the run, in order found: assert
  /// failures (status assert-failed) and, with deferMemoryErrors,
  /// memory-safety findings (status memory-error, at the violation); the
  /// fuzzer stops at the first unless keepGoing.
  std::vector<FuzzFailure> failures;
  /// Runs that stopped (status error) at a memory-safety check, by op:
  /// without deferMemoryErrors, so not classified (a later assume might
  /// block the run). Never stop the fuzzer.
  std::vector<FuzzFailure> memoryChecks;
  /// Runs that stopped (status error) at an interpreter limit or a layout
  /// error, by op. Never findings; never stop the fuzzer.
  std::vector<FuzzFailure> limits;
  /// deferMemoryErrors: the runs that went past a violation, per final
  /// status (memory-error: findings; assume-blocked: infeasible;
  /// step-limit, error: not classified).
  uint64_t deferredStatusCount[kRunStatusCount] = {};
  /// Per blocking site (sea.assume, sea.unreachable): the runs it
  /// blocked; most first, then program order.
  std::vector<std::pair<mlir::Operation *, uint64_t>> blocked;
  /// The named assumption and the open decision the trials rested on:
  /// totals of RunResult::addr0IsNullUses / memhavocNullNoopUses, and the
  /// runs with any.
  uint64_t addr0IsNullUses = 0, addr0IsNullRuns = 0;
  uint64_t memhavocNullNoopUses = 0, memhavocNullNoopRuns = 0;
  /// Parallel to FuzzOptions::reachTargets.
  std::vector<ReachHit> reachHits;
};

/// Fuzzes `program`: first the seeds, in order, then mutated entries.
/// Writes failure-<trial>.txt (the first of each failure),
/// memory-check-<trial>.txt / limit-<trial>.txt (the first of each error)
/// and cov-<trial>.txt (new coverage) to FuzzOptions::outDir. Warns
/// fuzz.limit (once, then counted) when trials stop at a limit.
/// Fails (with `error`) on a seed value that does not fit its site, or an
/// output file that cannot be written.
mlir::FailureOr<FuzzReport> fuzz(ProgramOp program,
                                 llvm::ArrayRef<CorpusEntryText> seeds,
                                 const FuzzOptions &options,
                                 std::string &error);

/// The --input-syntax text of an assignment: `<site>=<value>` or
/// `<site>@<cell>=<value>`.
std::string formatAssignment(const Assignment &a);

/// Writes a corpus file: `header` (its lines are written as `# ` comments),
/// then one assignment per line. Fails (with `error`) when the file cannot
/// be written.
mlir::LogicalResult writeCorpusFile(llvm::StringRef path,
                                    llvm::ArrayRef<std::string> header,
                                    llvm::ArrayRef<Assignment> input,
                                    std::string &error);

//===----------------------------------------------------------------------===//
// Serve mode (sea-run --fuzz-serve): runs on inputs chosen outside
// sea-mlir, one per request, with the fuzzer's trial source and coverage
// kept across requests. No mutation, no corpus.
//===----------------------------------------------------------------------===//

/// One request: the values it sets, and what the sites it does not set get.
struct ServeRequest {
  std::vector<Assignment> inputs;
  /// Error: the run stops (status error) at a site without a value, as
  /// plain sea-run does.
  InputSource::Missing missing = InputSource::Missing::Error;
  /// The seed of this run's random values; none: the server's stream
  /// (seeded by FuzzOptions::seed), so later requests get other values.
  std::optional<uint64_t> seed;
};

/// The coverage of all runs so far, as in FuzzReport.
struct ServeCoverage {
  unsigned blocksHit = 0, blocksTotal = 0;
  unsigned edgesHit = 0;
  unsigned assumesPassed = 0, assumesTotal = 0;
};

/// One run and what it reached first. Ids: a block's position in the
/// program body (the entry is 0); an assume's position in blockingOpsOf.
struct ServeResult {
  RunResult run;
  /// The values the run read, one per site or cell (as in a trial), in
  /// --input syntax: `sea-run --input-file` of these lines replays the run.
  /// Ordered by site, then cell. RunResult::reads / cellReads have the
  /// origin of each read: "input" (the request set it), "zero", "random".
  std::vector<Assignment> input;
  std::vector<unsigned> newBlocks;
  /// (from, to); from -1: the entry of the run.
  std::vector<std::pair<int64_t, unsigned>> newEdges;
  std::vector<unsigned> newAssumes;
  /// assume-blocked: the id of the blocking op.
  std::optional<unsigned> blocked;
};

class FuzzServerImpl;

class FuzzServer {
public:
  /// Uses FuzzOptions::seed, maxSteps and deferMemoryErrors.
  FuzzServer(ProgramOp program, const FuzzOptions &options);
  ~FuzzServer();
  /// Runs `request`. Fails, without running (coverage unchanged), on a
  /// request that names no input site of the program, sets a memory
  /// site's value or a value site's cell, or gives a value that does not
  /// fit its site.
  mlir::FailureOr<ServeResult> run(const ServeRequest &request,
                                   std::string &error);
  ServeCoverage coverage() const;

private:
  std::unique_ptr<FuzzServerImpl> impl;
};

//===----------------------------------------------------------------------===//
// Introspection (FuzzIntrospect.cpp): what a strategy written outside
// sea-mlir needs to know about a program, computed statically.
//===----------------------------------------------------------------------===//

/// An input site: an op that reads values from the nondet source.
struct InputSite {
  uint64_t site = 0;
  /// sea.nondet / sea.struct.nondet: the callee; sea.mem.nondet: its name;
  /// sea.memhavoc: "-".
  std::string callee;
  /// A value: its width (a struct: the bits of its integer leaves); a
  /// memory: the cell width.
  unsigned width = 0;
  bool memory = false;
  mlir::Operation *op = nullptr;
};

/// The input sites of `program` in program order: sea.nondet,
/// sea.struct.nondet (values), sea.mem.nondet, sea.memhavoc (memory).
/// sea.ptr.nondet takes no input (a fresh object) and is not one.
std::vector<InputSite> inputSitesOf(ProgramOp program);

/// The ops that can block a run, in program order: sea.assume and
/// sea.unreachable. A blocking op's id is its index here (the id of
/// --fuzz-sites' fuzz.assume lines and of --fuzz-serve's responses).
std::vector<mlir::Operation *> blockingOpsOf(ProgramOp program);

/// The bounds of describeCondition: values more than this many ops below
/// the condition, and nodes past this many, render as `?`.
constexpr unsigned kConditionDepth = 6;
constexpr unsigned kConditionNodes = 40;

/// A condition written over input sites (docs/reference/fuzzing.md, "For
/// agents"): an s-expression with `s<N>` for the value of input site N,
/// constants in unsigned decimal, `(<op> args)` for pure ops (`(<pred> a
/// b)` for comparisons), `(load:<w> s<N>@<addr>)` for a load straight from
/// a sea.mem.nondet, and `?` where the backward walk stops. `sites`: the
/// input sites it names, ascending.
struct ConditionText {
  std::string text;
  std::vector<uint64_t> sites;
};

/// Walks back from `cond` through pure ops (and block arguments with one
/// incoming value) to input sites and constants. Static; no run needed.
ConditionText describeCondition(mlir::Value cond);

} // namespace sea

#endif // SEA_INTERP_FUZZ_H
