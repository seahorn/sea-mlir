// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Fuzz.cpp - greybox fuzzing over the interpreter --------------------===//
//
// See Fuzz.h and docs/reference/fuzzing.md.
//
//===----------------------------------------------------------------------===//

#include "sea/Interp/Fuzz.h"

#include "sea/Support/Diag.h"
#include "sea/Support/Log.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <set>

using namespace mlir;
using llvm::APInt;
using llvm::StringRef;

namespace sea {

static log::Tag logFuzz("fuzz.run", 2,
                        "sea-run --fuzz: start and end (1); each trial that "
                        "reached new coverage (2)");

//===----------------------------------------------------------------------===//
// Corpus files
//===----------------------------------------------------------------------===//

/// `addrText` (`<addr>` or `<object>+<offset>`) as InputSource keys a cell.
static bool cellKey(StringRef addrText, std::string &key) {
  auto [object, offText] = addrText.rsplit('+');
  if (offText.empty())
    std::tie(object, offText) = std::make_pair(StringRef(), addrText);
  APInt addr;
  if (offText.getAsInteger(0, addr))
    return false;
  key = llvm::toString(addr, 10, /*Signed=*/false);
  if (!object.empty())
    key = (object + "+" + key).str();
  return true;
}

LogicalResult parseAssignment(StringRef text, Assignment &out,
                              std::string &error) {
  auto [key, value] = text.split('=');
  auto [siteText, addrText] = key.split('@');
  out = Assignment();
  // `<site>@` (no address) is a cell key without its cell: an error
  if (key.contains('@')) {
    if (siteText.getAsInteger(10, out.site) || !cellKey(addrText, out.cell) ||
        value.empty()) {
      error = "expected <site>@<addr>=<value> or "
              "<site>@<object>+<offset>=<value>";
      return failure();
    }
    out.value = value.str();
    return success();
  }
  if (siteText.getAsInteger(10, out.site) || value.empty()) {
    error = "expected <site>=<value>";
    return failure();
  }
  out.value = value.str();
  return success();
}

void addAssignments(InputSource &source, llvm::ArrayRef<Assignment> as) {
  for (const Assignment &a : as) {
    if (a.cell.empty())
      source.set(a.site, a.value);
    else
      source.setCell(a.site, a.cell, a.value);
  }
}

std::string formatAssignment(const Assignment &a) {
  if (a.cell.empty())
    return std::to_string(a.site) + "=" + a.value;
  return std::to_string(a.site) + "@" + a.cell + "=" + a.value;
}

/// The k=v fields of a record line's rest.
static std::map<std::string, std::string> recordFields(StringRef rest) {
  std::map<std::string, std::string> f;
  llvm::SmallVector<StringRef> toks;
  rest.split(toks, ' ', -1, /*KeepEmpty=*/false);
  for (StringRef t : toks) {
    auto [k, v] = t.split('=');
    if (!v.empty())
      f.emplace(k.str(), v.str());
  }
  return f;
}

LogicalResult parseCorpusText(StringRef text, CorpusEntryText &out,
                              std::string &error) {
  llvm::SmallVector<StringRef> lines;
  text.split(lines, '\n');
  unsigned lineNo = 0;
  for (StringRef raw : lines) {
    ++lineNo;
    StringRef line = raw.trim();
    if (line.empty() || line.starts_with("#"))
      continue;
    auto where = [&] {
      return out.name + ":" + std::to_string(lineNo) + ": '" + line.str() + "'";
    };
    auto [head, rest] = line.split(": ");
    bool isRecord = head != line && !head.contains('=') && !head.contains(' ');
    if (!isRecord) {
      Assignment a;
      std::string why;
      if (failed(parseAssignment(line, a, why))) {
        error = "bad corpus line " + where() + ", " + why;
        return failure();
      }
      out.assignments.push_back(std::move(a));
      continue;
    }
    if (head != "nondet" && head != "memread") {
      ++out.skippedLines; // another line of a saved run
      continue;
    }
    std::map<std::string, std::string> f = recordFields(rest);
    Assignment a;
    bool ok = f.count("site") && f.count("value") &&
              !StringRef(f["site"]).getAsInteger(10, a.site);
    if (ok && head == "memread")
      ok = f.count("addr") && cellKey(f["addr"], a.cell);
    if (!ok) {
      error = "bad corpus line " + where() + ", expected site=<n> " +
              (head == "memread" ? "addr=<a> " : "") + "value=<v>";
      return failure();
    }
    a.value = f["value"];
    out.assignments.push_back(std::move(a));
  }
  return success();
}

static LogicalResult readCorpusFile(StringRef path,
                                    std::vector<CorpusEntryText> &out,
                                    std::string &error) {
  auto buf = llvm::MemoryBuffer::getFile(path, /*IsText=*/true);
  if (!buf) {
    error = "cannot read corpus file '" + path.str() +
            "': " + buf.getError().message();
    return failure();
  }
  CorpusEntryText e;
  e.name = path.str();
  if (failed(parseCorpusText((*buf)->getBuffer(), e, error)))
    return failure();
  out.push_back(std::move(e));
  return success();
}

LogicalResult readCorpus(StringRef path, std::vector<CorpusEntryText> &out,
                         std::string &error) {
  if (!llvm::sys::fs::is_directory(path))
    return readCorpusFile(path, out, error);
  std::vector<std::string> files;
  std::error_code ec;
  for (llvm::sys::fs::directory_iterator it(path, ec), end; it != end && !ec;
       it.increment(ec))
    if (llvm::sys::fs::is_regular_file(it->path()))
      files.push_back(it->path());
  if (ec) {
    error =
        "cannot list corpus directory '" + path.str() + "': " + ec.message();
    return failure();
  }
  llvm::sort(files);
  for (const std::string &f : files)
    if (failed(readCorpusFile(f, out, error)))
      return failure();
  return success();
}

//===----------------------------------------------------------------------===//
// The fuzzer
//===----------------------------------------------------------------------===//

namespace {

/// A site (cell empty) or a memory cell: the key of an assignment.
using Key = std::pair<uint64_t, std::string>;
/// The values of one trial's inputs, by key (ordered: deterministic).
using Entry = std::map<Key, APInt>;

/// The nondet source of a trial: values preset by the entry, else the
/// seed's text (parsed at the read width), else fresh ones. A key gets one
/// value per trial.
class FuzzSource : public NondetSource {
public:
  /// `fresh` returns none when a key without a value stops the run (serve
  /// mode, Missing::Error).
  explicit FuzzSource(std::function<std::optional<APInt>(unsigned)> fresh)
      : fresh(std::move(fresh)) {}

  /// The origin of a read value (NondetRead / CellRead): from the preset or
  /// the text, and fresh.
  std::string givenOrigin = "fuzz", freshOrigin = "fuzz";

  /// Starts a trial on `preset` (and `text`, a seed's values).
  void reset(Entry preset, const std::map<Key, std::string> *seedText) {
    values = std::move(preset);
    used.clear();
    text = seedText;
  }

  FailureOr<APInt> get(NondetOp op, std::string &origin,
                       std::string &error) override {
    return lookup({op.getSite(), ""}, op.getType().getIntOrFloatBitWidth(),
                  origin, error);
  }
  FailureOr<APInt> get(StructNondetOp op, std::string &origin,
                       std::string &error) override {
    return lookup({op.getSite(), ""}, op.getBitsWidth(), origin, error);
  }
  FailureOr<APInt> getCell(Operation *siteOp, const APInt &addr,
                           StringRef object, unsigned cellWidth,
                           std::string &origin, std::string &error) override {
    std::string k = llvm::toString(addr, 10, /*Signed=*/false);
    if (!object.empty())
      k = (object + "+" + k).str();
    return lookup({memSiteOf(siteOp), std::move(k)}, cellWidth, origin, error);
  }
  std::optional<APInt> peekCell(Operation *siteOp, const APInt &addr,
                                StringRef object, unsigned cellWidth,
                                std::string &origin) override {
    std::string k = llvm::toString(addr, 10, /*Signed=*/false);
    if (!object.empty())
      k = (object + "+" + k).str();
    auto it = values.find({memSiteOf(siteOp), k});
    if (it == values.end() || it->second.getBitWidth() != cellWidth)
      return std::nullopt;
    origin = givenOrigin;
    return it->second;
  }

  /// The values the trial read.
  Entry used;

private:
  FailureOr<APInt> lookup(Key key, unsigned width, std::string &origin,
                          std::string &error) {
    origin = givenOrigin;
    if (auto it = values.find(key); it != values.end()) {
      // a value of another width (the same key read at another width; a
      // site has one width, so not expected): a fresh value then. A replay
      // that differs is caught by sea-run's check (fuzz.replay-mismatch).
      if (it->second.getBitWidth() == width) {
        used[key] = it->second;
        return it->second;
      }
    }
    APInt v;
    auto it =
        text ? text->find(key) : std::map<Key, std::string>::const_iterator();
    if (text && it != text->end()) {
      FailureOr<APInt> p = parseBitVector(it->second, width);
      if (failed(p)) {
        error = "corpus value '" + it->second + "' for " +
                (key.second.empty() ? "site " + std::to_string(key.first)
                                    : "cell " + key.second + " of site " +
                                          std::to_string(key.first)) +
                " is not an i" + std::to_string(width) + " value";
        badSeed = true;
        return failure();
      }
      v = *p;
    } else {
      std::optional<APInt> f = fresh(width);
      if (!f) {
        error =
            "no input for " + (key.second.empty()
                                   ? "nondet site " + std::to_string(key.first)
                                   : "cell " + key.second + " of memory site " +
                                         std::to_string(key.first));
        return failure();
      }
      v = *f;
      origin = freshOrigin;
    }
    values[key] = v;
    used[key] = v;
    return v;
  }

  std::function<std::optional<APInt>(unsigned)> fresh;
  Entry values;
  const std::map<Key, std::string> *text = nullptr;

public:
  /// Set when a seed value did not fit its site (the run stopped with an
  /// error that is the corpus's, not the program's).
  bool badSeed = false;
};

class Fuzzer {
public:
  Fuzzer(ProgramOp program, const FuzzOptions &options)
      : program(program), options(options), rng(options.seed),
        source([this](unsigned w) { return std::optional(freshValue(w)); }) {
    unsigned order = 0;
    program.walk([&](Operation *op) {
      opOrder[op] = order++;
      if (isa<AssumeOp>(op))
        ++report.assumesTotal;
      if (auto c = dyn_cast<ConstOp>(op))
        addDictionary(c);
    });
    for (Block &b : program.getBody())
      (void)b, ++report.blocksTotal;
    report.dictionarySize = dict.size();
    runOptions.maxSteps = options.maxSteps;
    runOptions.deferMemoryErrors = options.deferMemoryErrors;
    runOptions.addr0IsNull = options.addr0IsNull;
    runOptions.memhavocNullNoop = options.memhavocNullNoop;
    runOptions.facts = programFactsOf(program);
    runOptions.prints = false;
    runOptions.onBlock = [this](Block *from, Block *to) {
      if (blocks.insert(to).second)
        newBlocks.push_back(to);
      if (edges.insert({from, to}).second)
        newEdges.push_back({from, to});
    };
    runOptions.onAssume = [this](AssumeOp a) {
      if (assumes.insert(a).second)
        newAssumes.push_back(a);
    };
  }

  FailureOr<FuzzReport> run(llvm::ArrayRef<CorpusEntryText> seeds,
                            std::string &error);

  /// One run on `src`, with the coverage hooks: the new* lists hold what
  /// it reached first. The runner of the trials and of serve mode.
  RunResult runOn(NondetSource &src) {
    newBlocks.clear();
    newEdges.clear();
    newAssumes.clear();
    return interpret(program, src, runOptions);
  }

  ServeCoverage coverage() const {
    ServeCoverage c;
    c.blocksHit = blocks.size();
    c.blocksTotal = report.blocksTotal;
    c.edgesHit = edges.size();
    c.assumesPassed = assumes.size();
    c.assumesTotal = report.assumesTotal;
    return c;
  }

  /// What the last run reached first.
  std::vector<Block *> newBlocks;
  std::vector<std::pair<Block *, Block *>> newEdges;
  std::vector<AssumeOp> newAssumes;

private:
  /// The program's integer constants used by comparisons and arithmetic,
  /// and +-1 of each, at their width.
  void addDictionary(ConstOp c) {
    auto attr = dyn_cast<IntegerAttr>(c.getValue());
    if (!attr)
      return;
    bool used = llvm::any_of(c->getUsers(), [](Operation *u) {
      return isa<ICmpOp, AddOp, SubOp, MulOp, UDivOp, SDivOp, URemOp, SRemOp,
                 UMinOp, UMaxOp, SMinOp, SMaxOp, UAddOOp, SAddOOp, USubOOp,
                 SSubOOp, UMulOOp, SMulOOp, SDivOOp>(u);
    });
    if (!used)
      return;
    APInt v = attr.getValue();
    if (v.getBitWidth() > 64)
      return; // wide constants: not in the dictionary
    for (APInt x : {v - 1, v, v + 1}) {
      auto key = std::make_pair(x.getBitWidth(), x.getZExtValue());
      if (dictSeen.insert(key).second)
        dict.push_back(x);
    }
  }

  APInt randomValue(unsigned w) {
    llvm::SmallVector<uint64_t> words((w + 63) / 64);
    for (uint64_t &x : words)
      x = rng();
    return APInt(w, words);
  }

  /// A dictionary value sized to `w` (sign or zero extended, truncated).
  APInt dictValue(unsigned w) {
    const APInt &c = dict[rng() % dict.size()];
    if (c.getBitWidth() >= w)
      return c.trunc(w);
    return (rng() & 1) ? c.sext(w) : c.zext(w);
  }

  bool useDict() { return options.dictionary && !dict.empty(); }

  /// A value for a site the entry does not set: random or dictionary.
  APInt freshValue(unsigned w) {
    if (useDict() && (rng() & 1))
      return dictValue(w);
    return randomValue(w);
  }

  APInt mutate(const APInt &v) {
    unsigned w = v.getBitWidth();
    switch (rng() % 5) {
    case 0:
      return randomValue(w);
    case 1: {
      APInt r = v;
      r.flipBit(rng() % w);
      return r;
    }
    case 2:
      return (rng() & 1) ? v + 1 : v - 1;
    case 3:
      switch (rng() % 5) {
      case 0:
        return APInt::getZero(w);
      case 1:
        return APInt(w, 1);
      case 2:
        return APInt::getAllOnes(w);
      case 3:
        return APInt::getSignedMinValue(w);
      default:
        return APInt::getSignedMaxValue(w);
      }
    default:
      return useDict() ? dictValue(w) : randomValue(w);
    }
  }

  /// The next trial's preset: a corpus entry (or the empty one) with 1, 2,
  /// 4 or 8 mutations.
  Entry nextEntry() {
    uint64_t pick = rng() % (corpus.size() + 1);
    if (pick == corpus.size())
      return {};
    Entry e = corpus[pick];
    if (e.empty())
      return e;
    unsigned n = 1u << (rng() % 4);
    for (unsigned i = 0; i < n; ++i) {
      auto it = e.begin();
      std::advance(it, rng() % e.size());
      it->second = mutate(it->second);
    }
    return e;
  }

public:
  static std::vector<Assignment> assignmentsOf(const Entry &e) {
    std::vector<Assignment> as;
    for (const auto &[k, v] : e)
      as.push_back({k.first, k.second, llvm::toString(v, 10, false)});
    return as;
  }

private:
  /// Writes `e` as a corpus file; returns its path.
  FailureOr<std::string> write(StringRef kind, const Entry &e,
                               const RunResult &r, std::string &error) {
    llvm::SmallString<128> path(options.outDir);
    llvm::sys::path::append(
        path, (kind + "-" + llvm::Twine(report.trials) + ".txt").str());
    std::error_code ec;
    llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::OF_Text);
    if (ec) {
      error = "cannot write '" + path.str().str() + "': " + ec.message();
      return failure();
    }
    os << "# sea-run --fuzz: trial=" << report.trials
       << " status=" << stringifyRunStatus(r.status) << "\n";
    if (r.status == RunStatus::MemoryError)
      os << "# replay with: sea-run --defer-memory-errors --input-file=<this "
            "file>\n";
    // what the run rested on (the replay needs the same options)
    if (r.addr0IsNullUses || r.memhavocNullNoopUses)
      os << "# assume: addr0-is-null=" << r.addr0IsNullUses
         << " memhavoc-null-noop=" << r.memhavocNullNoopUses << "\n";
    for (const Assignment &a : assignmentsOf(e))
      os << formatAssignment(a) << "\n";
    ++report.filesWritten;
    return path.str().str();
  }

  /// One trial on `preset` (and a seed's text). Returns failure on an
  /// error of the fuzzer (`error` set); `stop` when the run must end.
  LogicalResult trial(Entry preset, const std::map<Key, std::string> *seedText,
                      bool &stop, std::string &error);

  ProgramOp program;
  const FuzzOptions &options;
  std::mt19937_64 rng;
  FuzzSource source;
  RunOptions runOptions;
  std::vector<APInt> dict;
  std::set<std::pair<unsigned, uint64_t>> dictSeen;
  llvm::DenseMap<Operation *, unsigned> opOrder;
  std::vector<Entry> corpus;
  llvm::DenseSet<Block *> blocks;
  llvm::DenseSet<std::pair<Block *, Block *>> edges;
  llvm::DenseSet<Operation *> assumes;
  llvm::DenseMap<Operation *, uint64_t> blockedBy;
  FuzzReport report;
};

} // namespace

LogicalResult Fuzzer::trial(Entry preset,
                            const std::map<Key, std::string> *seedText,
                            bool &stop, std::string &error) {
  source.reset(std::move(preset), seedText);
  RunResult r = runOn(source);
  ++report.trials;
  if (source.badSeed) {
    error = r.message;
    return failure();
  }
  ++report.statusCount[static_cast<unsigned>(r.status)];
  report.addr0IsNullUses += r.addr0IsNullUses;
  report.addr0IsNullRuns += r.addr0IsNullUses != 0;
  report.memhavocNullNoopUses += r.memhavocNullNoopUses;
  report.memhavocNullNoopRuns += r.memhavocNullNoopUses != 0;
  if (r.deferredCount)
    ++report.deferredStatusCount[static_cast<unsigned>(r.status)];
  if (r.status == RunStatus::AssumeBlocked && r.at)
    ++blockedBy[r.at];
  bool failing =
      r.status == RunStatus::AssertFailed || r.status == RunStatus::MemoryError;
  bool erring = r.status == RunStatus::Error;
  // a memory-safety check that stopped the run (no deferMemoryErrors), or
  // a limit (a layout error included)
  bool memoryCheck = erring && isMemorySafetyError(r);
  if (erring && !memoryCheck)
    diag::warn("fuzz.limit",
               "a trial stopped at an interpreter limit (not a finding; "
               "fuzz.limit lines count them per op): " +
                   r.message,
               r.at ? std::optional<Location>(r.at->getLoc()) : std::nullopt);
  if (!newBlocks.empty() || !newEdges.empty() || !newAssumes.empty()) {
    corpus.push_back(source.used);
    SEA_LOG(logFuzz, 2)
        .msg("new-coverage")
        .kv("trial", report.trials)
        .kv("status", stringifyRunStatus(r.status))
        .kv("blocks", newBlocks.size())
        .kv("edges", newEdges.size())
        .kv("assumes", newAssumes.size())
        .kv("corpus", corpus.size());
    if (!options.outDir.empty() && !failing && !erring)
      if (failed(write("cov", source.used, r, error)))
        return failure();
  }
  if (!failing && !erring)
    return success();
  std::vector<FuzzFailure> &list = failing       ? report.failures
                                   : memoryCheck ? report.memoryChecks
                                                 : report.limits;
  auto it =
      llvm::find_if(list, [&](const FuzzFailure &f) { return f.at == r.at; });
  if (it != list.end()) {
    ++it->count;
  } else {
    FuzzFailure f;
    f.trial = report.trials;
    f.status = r.status;
    f.at = r.at;
    f.message = r.message;
    f.input = assignmentsOf(source.used);
    f.count = 1;
    if (!options.outDir.empty()) {
      FailureOr<std::string> p = write(failing       ? "failure"
                                       : memoryCheck ? "memory-check"
                                                     : "limit",
                                       source.used, r, error);
      if (failed(p))
        return failure();
      f.file = *p;
    }
    list.push_back(std::move(f));
  }
  if (failing && !options.keepGoing)
    stop = true;
  return success();
}

FailureOr<FuzzReport> Fuzzer::run(llvm::ArrayRef<CorpusEntryText> seeds,
                                  std::string &error) {
  if (!options.outDir.empty() && llvm::sys::fs::exists(options.outDir)) {
    std::error_code ec;
    llvm::sys::fs::directory_iterator it(options.outDir, ec);
    if (ec || it != llvm::sys::fs::directory_iterator()) {
      error = "--fuzz-out directory '" + options.outDir +
              "' is not empty (or not a directory); give a new or empty one, "
              "so the inputs of different runs do not mix";
      return failure();
    }
  }
  if (!options.outDir.empty())
    if (std::error_code ec =
            llvm::sys::fs::create_directories(options.outDir)) {
      error = "cannot create '" + options.outDir + "': " + ec.message();
      return failure();
    }
  SEA_LOG(logFuzz, 1)
      .msg("start")
      .kv("program", ("@" + program.getSymName()).str())
      .kv("seeds", seeds.size())
      .kv("trials", options.trials)
      .kv("seconds", options.seconds)
      .kv("seed", options.seed)
      .kv("dictionary", options.dictionary ? dict.size() : 0);
  auto start = std::chrono::steady_clock::now();
  auto elapsed = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
        .count();
  };
  auto budgetLeft = [&] {
    if (options.trials && report.trials >= options.trials)
      return false;
    if (options.seconds > 0 && elapsed() >= options.seconds)
      return false;
    return true;
  };
  bool stop = false;
  for (const CorpusEntryText &s : seeds) {
    if (stop || !budgetLeft())
      break;
    std::map<Key, std::string> text;
    for (const Assignment &a : s.assignments)
      text[{a.site, a.cell}] = a.value;
    if (failed(trial({}, &text, stop, error))) {
      error = s.name + ": " + error;
      return failure();
    }
    ++report.seedTrials;
  }
  while (!stop && budgetLeft())
    if (failed(trial(nextEntry(), nullptr, stop, error)))
      return failure();
  report.seconds = elapsed();
  report.blocksHit = blocks.size();
  report.edgesHit = edges.size();
  report.assumesPassed = assumes.size();
  report.corpusSize = corpus.size();
  for (auto [op, n] : blockedBy)
    report.blocked.push_back({op, n});
  llvm::sort(report.blocked, [&](const auto &a, const auto &b) {
    if (a.second != b.second)
      return a.second > b.second;
    return opOrder.lookup(a.first) < opOrder.lookup(b.first);
  });
  SEA_LOG(logFuzz, 1)
      .msg("end")
      .kv("trials", report.trials)
      .kv("time", report.seconds)
      .kv("blocks", report.blocksHit)
      .kv("blocks-total", report.blocksTotal)
      .kv("corpus", report.corpusSize)
      .kv("failures",
          report.statusCount[static_cast<unsigned>(RunStatus::AssertFailed)] +
              report.statusCount[static_cast<unsigned>(RunStatus::MemoryError)])
      .kv("errors",
          report.statusCount[static_cast<unsigned>(RunStatus::Error)]);
  return report;
}

FailureOr<FuzzReport> fuzz(ProgramOp program,
                           llvm::ArrayRef<CorpusEntryText> seeds,
                           const FuzzOptions &options, std::string &error) {
  Fuzzer f(program, options);
  return f.run(seeds, error);
}

//===----------------------------------------------------------------------===//
// Serve mode
//===----------------------------------------------------------------------===//

class FuzzServerImpl {
public:
  FuzzServerImpl(ProgramOp program, const FuzzOptions &options)
      : options(options), fuzzer(program, this->options), rng(options.seed),
        source([this](unsigned w) { return freshValue(w); }) {
    source.givenOrigin = "input";
    unsigned i = 0;
    for (Block &b : program.getBody())
      blockId[&b] = i++;
    for (auto [id, op] : llvm::enumerate(blockingOpsOf(program)))
      blockerId[op] = id;
    for (const InputSite &s : inputSitesOf(program))
      sites[s.site] = s;
  }

  FailureOr<ServeResult> run(const ServeRequest &req, std::string &error);

  FuzzOptions options;
  Fuzzer fuzzer;

private:
  std::optional<APInt> freshValue(unsigned w) {
    switch (missing) {
    case InputSource::Missing::Error:
      return std::nullopt;
    case InputSource::Missing::Zero:
      return APInt::getZero(w);
    case InputSource::Missing::Random: {
      llvm::SmallVector<uint64_t> words((w + 63) / 64);
      for (uint64_t &x : words)
        x = (*random)();
      return APInt(w, words);
    }
    }
    llvm_unreachable("unknown missing-input policy");
  }

  std::mt19937_64 rng;
  FuzzSource source;
  InputSource::Missing missing = InputSource::Missing::Error;
  std::mt19937_64 *random = &rng; // this run's stream
  llvm::DenseMap<Block *, unsigned> blockId;
  llvm::DenseMap<Operation *, unsigned> blockerId;
  std::map<uint64_t, InputSite> sites;
};

FailureOr<ServeResult> FuzzServerImpl::run(const ServeRequest &req,
                                           std::string &error) {
  // check the whole request before running: a bad one changes no coverage
  Entry preset;
  for (const Assignment &a : req.inputs) {
    auto it = sites.find(a.site);
    if (it == sites.end()) {
      error = "'" + formatAssignment(a) + "': the program has no input site " +
              std::to_string(a.site);
      return failure();
    }
    const InputSite &s = it->second;
    if (s.memory == a.cell.empty()) {
      error = "'" + formatAssignment(a) + "': site " + std::to_string(a.site) +
              (s.memory ? " is a memory: give <site>@<addr>=<value>"
                        : " is a value: give <site>=<value>");
      return failure();
    }
    FailureOr<APInt> v = parseBitVector(a.value, s.width);
    if (failed(v)) {
      error = "'" + formatAssignment(a) + "': not an i" +
              std::to_string(s.width) + " value";
      return failure();
    }
    preset[{a.site, a.cell}] = *v;
  }
  missing = req.missing;
  source.freshOrigin =
      missing == InputSource::Missing::Zero ? "zero" : "random";
  std::mt19937_64 own(req.seed.value_or(0));
  random = req.seed ? &own : &rng;
  source.reset(std::move(preset), nullptr);
  ServeResult out;
  out.run = fuzzer.runOn(source);
  random = &rng;
  out.input = Fuzzer::assignmentsOf(source.used);
  for (Block *b : fuzzer.newBlocks)
    out.newBlocks.push_back(blockId.lookup(b));
  for (auto [from, to] : fuzzer.newEdges)
    out.newEdges.push_back(
        {from ? int64_t(blockId.lookup(from)) : -1, blockId.lookup(to)});
  for (AssumeOp a : fuzzer.newAssumes)
    out.newAssumes.push_back(blockerId.lookup(a));
  if (out.run.status == RunStatus::AssumeBlocked && out.run.at)
    out.blocked = blockerId.lookup(out.run.at);
  return out;
}

FuzzServer::FuzzServer(ProgramOp program, const FuzzOptions &options)
    : impl(std::make_unique<FuzzServerImpl>(program, options)) {}

FuzzServer::~FuzzServer() = default;

FailureOr<ServeResult> FuzzServer::run(const ServeRequest &request,
                                       std::string &error) {
  return impl->run(request, error);
}

ServeCoverage FuzzServer::coverage() const { return impl->fuzzer.coverage(); }

} // namespace sea
