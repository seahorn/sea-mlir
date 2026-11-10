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
  /// Count assert failures instead of stopping at the first.
  bool keepGoing = false;
  /// Write failing and new-coverage inputs here (empty: none). The
  /// directory must be new or empty.
  std::string outDir;
  /// Per-trial interpreter options (maxSteps; prints are off in trials).
  uint64_t maxSteps = 1'000'000;
};

/// A trial that failed an assert, or the first trial that stopped with an
/// error at an op (status error: out of bounds, through null, misaligned,
/// past an interpreter limit, unsupported).
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
  /// Assert failures, by the op that ended the run, in order found; the
  /// fuzzer stops at the first unless keepGoing.
  std::vector<FuzzFailure> failures;
  /// Errors, by the op that ended the run, in order found (never stop).
  std::vector<FuzzFailure> errors;
  /// Per blocking site (sea.assume, sea.unreachable): the runs it
  /// blocked; most first, then program order.
  std::vector<std::pair<mlir::Operation *, uint64_t>> blocked;
};

/// Fuzzes `program`: first the seeds, in order, then mutated entries.
/// Writes failure-<trial>.txt (the first of each assert failure),
/// error-<trial>.txt (the first of each error) and cov-<trial>.txt (new
/// coverage) to FuzzOptions::outDir.
/// Fails (with `error`) on a seed value that does not fit its site, or an
/// output file that cannot be written.
mlir::FailureOr<FuzzReport> fuzz(ProgramOp program,
                                 llvm::ArrayRef<CorpusEntryText> seeds,
                                 const FuzzOptions &options,
                                 std::string &error);

/// The --input-syntax text of an assignment: `<site>=<value>` or
/// `<site>@<cell>=<value>`.
std::string formatAssignment(const Assignment &a);

} // namespace sea

#endif // SEA_INTERP_FUZZ_H
