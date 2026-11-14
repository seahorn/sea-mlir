// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Reach.cpp - reachability of check sites and blocks -------*- C++ -*-===//

#include "sea/VC/Reach.h"

#include "sea/Support/Diag.h"
#include "sea/Support/Log.h"
#include "sea/Support/Timer.h"
#include "sea/VC/Replay.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

using namespace mlir;
using namespace expr;

namespace sea {

static log::Tag logReach("vc.reach", 2,
                         "each sea-vc --reach query: target, answer, solve "
                         "and replay time; each target a run decided");

Expr mkReachQuery(OpSemContext &ctx) {
  ExprVector conj(ctx.side().begin(), ctx.side().end());
  return op::boolop::land(conj, ctx.efac());
}

namespace {

enum class Verdict { Reachable, Unreachable, Unknown };

const char *verdictName(Verdict v) {
  return v == Verdict::Reachable     ? "reachable"
         : v == Verdict::Unreachable ? "unreachable"
                                     : "unknown";
}

/// The replay of a reach model: up to the target, every value is compared
/// with the model's (after it, the model leaves the blocks it does not need
/// unconstrained, so their values are not compared).
struct ReachReplay {
  RunResult run;
  bool reached = false;
  uint64_t checked = 0;
  uint64_t mismatches = 0;
  /// the targets the run reached (indices)
  std::vector<size_t> hit;
};

ReachReplay replayToTarget(ProgramOp program, solver::Model &model,
                           SeaOpSem &sem, OpSemContext &ctx,
                           const llvm::DenseMap<Operation *, size_t> &index,
                           Operation *target, const ReachOptions &ro) {
  ReachReplay r;
  const bool plantMismatch = ro.plantMismatch;
  if (plantMismatch)
    ++r.mismatches;
  ModelSource source(model, sem, ctx);
  RunOptions options;
  options.prints = false;
  options.warnEarlierVersion = false;
  options.addr0IsNull = ro.addr0IsNull;
  options.memhavocNullNoop = ro.memhavocNullNoop;
  options.onValue = [&](Value v, const llvm::APInt &interp) {
    if (r.reached)
      return;
    Expr t = sem.solverTerm(sem.lookup(v, ctx));
    Expr m = t ? model.eval(t, /*complete=*/false) : Expr();
    std::optional<llvm::APInt> mv = numeralToAPInt(m, interp.getBitWidth());
    // a term: the model leaves it unconstrained
    if (!mv && m && !isOpX<NONDET>(m))
      return;
    ++r.checked;
    if (!mv || *mv != interp)
      ++r.mismatches;
  };
  options.onOp = [&](Operation &op, auto) {
    auto it = index.find(&op);
    if (it == index.end())
      return;
    r.hit.push_back(it->second);
    if (&op == target)
      r.reached = true;
  };
  r.run = interpret(program, source, options);
  return r;
}

/// The A-INB check of a model (sea-vc's vc.seam-inb-spurious): the abstract
/// program run on the model's nondet values, and T found in it (AbstractSites).
/// `violated`: it leaves an object without having reached T. `after`: it
/// leaves an object only after T (the answer stands, the rest of the run
/// does not). `undetermined`: it does not reach T (it reads a nondet memory
/// cell, the model's cells being addresses, not object offsets; it stops
/// otherwise; or it ends on another path), or T is found only by a location
/// several ops share and the run stops. `unordered`: T is found only by
/// such a location and the run leaves an object: maybe before T. A run
/// decides a target (via=run, via=model-run) only if its check is not
/// violated or unordered; a model's own answer with an unordered check
/// stands, inb=undetermined.
enum class InbCheck { None, Ok, After, Violated, Undetermined, Unordered };

} // namespace

LogicalResult tagReachSites(mlir::ModuleOp module) {
  // tagged already: kept as given
  llvm::DenseMap<int64_t, Operation *> given;
  bool tagged = false;
  WalkResult w = module.walk([&](Operation *op) {
    auto a = op->getAttrOfType<IntegerAttr>(kReachSiteAttr);
    if (!a)
      return WalkResult::advance();
    tagged = true;
    if (!given.try_emplace(a.getInt(), op).second) {
      diag::error("vc.reach-site",
                  "two ops have reach site " + Twine(a.getInt()) +
                      " (sea.reach_site): a site names one check",
                  op->getLoc());
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  if (w.wasInterrupted())
    return failure();
  if (tagged)
    return success();
  int64_t k = 0;
  module.walk([&](Operation *op) {
    if (isa<AssumeOp, AssertOp>(op))
      op->setAttr(
          kReachSiteAttr,
          IntegerAttr::get(IntegerType::get(op->getContext(), 64), k++));
  });
  return success();
}

void tagBlockIds(mlir::ModuleOp module) {
  for (auto p : module.getOps<ProgramOp>()) {
    if (llvm::any_of(p.getBody(),
                     [](Block &b) { return bool(blockIdsOf(&b)); }))
      continue;
    Builder b(p.getContext());
    for (auto [n, blk] : llvm::enumerate(p.getBody()))
      if (!blk.empty() && blk.back().hasTrait<OpTrait::IsTerminator>())
        blk.getTerminator()->setAttr(kBlockIdsAttr,
                                     b.getStrArrayAttr({std::to_string(n)}));
  }
}

namespace {

/// Where the targets are in the abstract program: a check site the
/// lowering kept carries its tag (tagReachSites) in both programs; other
/// targets (folded assumes, merged asserts, blocks) are found by location:
/// an op of the target's kind at the target's location (a folded assume of
/// false, sea.unreachable, is a sea.assume there; a block, any op at its
/// location), ambiguous when more than one op of the abstract program
/// matches (inlined code repeats a location; a block can hold two ops at
/// one location): reaching one of them says nothing about which.
struct AbstractSites {
  explicit AbstractSites(ProgramOp abs) {
    abs.walk([&](Operation *op) {
      auto &e = byLoc[op->getLoc().getAsOpaquePointer()];
      e.push_back({op->getName().getStringRef()});
    });
  }
  struct Entry {
    StringRef name;
  };
  static bool matches(const ReachTarget &t, StringRef name) {
    return t.opName == "block" || name == t.opName ||
           (t.opName == "sea.unreachable" && name == "sea.assume");
  }
  bool ambiguous(const ReachTarget &t) const {
    auto it = byLoc.find(t.loc.getAsOpaquePointer());
    if (it == byLoc.end())
      return false;
    return llvm::count_if(it->second, [&](const Entry &e) {
             return matches(t, e.name);
           }) > 1;
  }
  llvm::DenseMap<const void *, llvm::SmallVector<Entry, 2>> byLoc;
};

/// The abstract program's run on a placed run's nondet values, and the ops
/// it reached the top of (tags; names by location), up to where it stopped.
struct AbstractRun {
  RunResult r;
  llvm::DenseSet<int64_t> tags;
  /// the tags of the asserts the run reached with their condition false
  /// (the failure targets, kAssertFailMarker)
  llvm::DenseSet<int64_t> failed;
  llvm::DenseMap<const void *, llvm::SmallVector<StringRef, 2>> seen;

  static std::optional<int64_t> tagOf(Operation *op) {
    if (auto a = op->getAttrOfType<IntegerAttr>(kReachSiteAttr))
      return a.getInt();
    return std::nullopt;
  }

  InbCheck check(const ReachTarget &t, const AbstractSites &sites,
                 std::string &message) const {
    bool at, ambiguous = false;
    if (std::optional<int64_t> tag = tagOf(t.op)) {
      // an assert's failure: reached with its condition false, not only
      // its top
      auto m = dyn_cast<PrintOp>(t.op);
      at = m && m.getTag() == kAssertFailMarker ? failed.contains(*tag)
                                                : tags.contains(*tag);
    } else {
      auto it = seen.find(t.loc.getAsOpaquePointer());
      at = it != seen.end() && llvm::any_of(it->second, [&](StringRef name) {
             return AbstractSites::matches(t, name);
           });
      ambiguous = sites.ambiguous(t);
    }
    const bool oob = isOutOfObjectAccess(r);
    const bool stopped = r.status == RunStatus::Error;
    if (stopped || oob)
      message = r.message;
    if (!at)
      return oob ? InbCheck::Violated : InbCheck::Undetermined;
    if (ambiguous && oob)
      return InbCheck::Unordered;
    if (ambiguous && stopped)
      return InbCheck::Undetermined;
    return oob ? InbCheck::After : InbCheck::Ok;
  }
};

AbstractRun runAbstract(ProgramOp abs, const RunResult &placed,
                        const ReachOptions &ro) {
  InputSource inputs(InputSource::Missing::Error);
  for (const NondetRead &rd : placed.reads) {
    llvm::SmallString<32> text;
    rd.value.toStringUnsigned(text);
    inputs.set(rd.site, std::string(text));
  }
  AbstractRun a;
  // the options of sea-vc's A-INB check of a counterexample
  RunOptions options;
  options.prints = false;
  options.warnEarlierVersion = false;
  options.addr0IsNull = ro.addr0IsNull;
  options.memhavocNullNoop = ro.memhavocNullNoop;
  options.onOp = [&](Operation &op, auto value) {
    if (std::optional<int64_t> tag = AbstractRun::tagOf(&op)) {
      a.tags.insert(*tag);
      if (auto as = dyn_cast<AssertOp>(op))
        if (std::optional<llvm::APInt> c = value(as.getCond());
            c && c->isZero())
          a.failed.insert(*tag);
    }
    auto &names = a.seen[op.getLoc().getAsOpaquePointer()];
    StringRef name = op.getName().getStringRef();
    if (!llvm::is_contained(names, name))
      names.push_back(name);
  };
  a.r = interpret(abs, inputs, options);
  return a;
}

/// ` site=<k>` (text and field) of a target with a reach site.
void addSite(out::Record &rec, raw_ostream &os, const ReachTarget &t) {
  if (t.site < 0)
    return;
  rec.num("site", uint64_t(t.site));
  os << " site=" << t.site;
  if (!t.of.empty()) {
    rec.str("of", t.of);
    os << " of=" << t.of;
  }
  if (!t.trueCheck)
    return;
  // a check of true: reached iff the next target of its block is
  rec.str("cond", "true");
  os << " cond=true";
  if (t.nextSite >= 0) {
    rec.num("next", uint64_t(t.nextSite));
    os << " next=" << t.nextSite;
  }
}

/// `# replays on: ...`, the program a witness replays on.
std::string replaysOn(const ReachOptions &options) {
  std::string program =
      options.programPath.empty() ? "<program>" : options.programPath;
  if (options.lowered)
    return "replays on: the lowered program (sea-opt --sea-vc-lower with "
           "the run's lowering options, of " +
           program + "), with sea-run --input-file=<this file>";
  return "replays on: the input program, with sea-run " + program +
         " --input-file=<this file>";
}

/// A corpus file of the run's inputs (sea-run --input-file).
bool writeWitness(StringRef path, const ReachTarget &t, StringRef at,
                  const ReachOptions &options, const RunResult &run) {
  std::error_code ec;
  llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::OF_Text);
  if (ec) {
    diag::error("vc.reach-witness",
                "cannot write '" + path + "': " + ec.message());
    return false;
  }
  os << "# sea-vc --reach: " << t.id << " op=" << t.opName << " at=" << at
     << "\n";
  os << "# " << replaysOn(options) << "\n";
  for (const NondetRead &rd : run.reads) {
    os << "nondet: site=" << rd.site << " callee=" << rd.callee
       << " width=" << rd.value.getBitWidth() << " value=";
    rd.value.print(os, /*isSigned=*/false);
    os << "\n";
  }
  for (const CellRead &cr : run.cellReads) {
    if (!cr.object.empty()) {
      diag::error("vc.reach-witness",
                  "a memory cell of an abstract memory in a witness (place "
                  "the program first)");
      return false;
    }
    os << "memread: site=" << memSiteOf(cr.siteOp) << " addr=";
    cr.addr.print(os, /*isSigned=*/false);
    os << " width=" << cr.value.getBitWidth() << " value=";
    cr.value.print(os, /*isSigned=*/false);
    os << "\n";
  }
  return true;
}

} // namespace

FailureOr<ReachRuns> runReachRuns(ProgramOp program,
                                  llvm::ArrayRef<ReachTarget> targets,
                                  const ReachRunOptions &runOptions,
                                  const ReachOptions &options,
                                  std::string &error) {
  ReachRuns out;
  out.decisions.resize(targets.size());
  FuzzOptions fo;
  fo.trials = runOptions.corpus.size() + runOptions.fuzzTrials;
  if (!fo.trials || targets.empty())
    return out;
  fo.seed = runOptions.seed;
  fo.maxSteps = runOptions.maxSteps;
  // coverage, not a bug hunt: a failing assert is a reached target
  fo.keepGoing = true;
  // the reach replays' interpreter options
  fo.addr0IsNull = options.addr0IsNull;
  fo.memhavocNullNoop = options.memhavocNullNoop;
  fo.reachTargets = reachTargetOps(targets);
  FailureOr<FuzzReport> rep = fuzz(program, runOptions.corpus, fo, error);
  if (failed(rep))
    return failure();
  out.trials = rep->trials;
  out.seedTrials = rep->seedTrials;
  if (!options.witnessDir.empty())
    if (std::error_code ec =
            llvm::sys::fs::create_directories(options.witnessDir)) {
      error = "cannot create '" + options.witnessDir + "': " + ec.message();
      return failure();
    }
  // the A-INB check (under --seam): once per run that reached a target
  std::optional<AbstractSites> sites;
  if (options.abstractProgram)
    sites.emplace(options.abstractProgram);
  std::map<uint64_t, AbstractRun> abstractRuns;
  std::set<uint64_t> deciding;
  for (auto [i, t] : llvm::enumerate(targets)) {
    const ReachHit &h = rep->reachHits[i];
    if (!h.trial)
      continue;
    ReachRunDecision &d = out.decisions[i];
    if (options.abstractProgram) {
      auto it = abstractRuns.find(h.trial);
      if (it == abstractRuns.end()) {
        // the run again, for its reads (the trial kept only its inputs)
        InputSource source(InputSource::Missing::Error);
        addAssignments(source, h.input);
        RunOptions ro;
        ro.prints = false;
        ro.warnEarlierVersion = false;
        ro.addr0IsNull = options.addr0IsNull;
        ro.memhavocNullNoop = options.memhavocNullNoop;
        ro.maxSteps = runOptions.maxSteps;
        RunResult placed = interpret(program, source, ro);
        it = abstractRuns
                 .emplace(h.trial,
                          runAbstract(options.abstractProgram, placed, options))
                 .first;
      }
      std::string message;
      InbCheck c = it->second.check(t, *sites, message);
      if (c == InbCheck::Violated || c == InbCheck::Unordered) {
        // as a model's run would be (replay=memory-error): not a run of
        // the program the VC is about; the solver answers it
        ++out.inbViolated;
        continue;
      }
      d.inbUndetermined = c == InbCheck::Undetermined;
    }
    d.reached = true;
    ++out.decided;
    d.by =
        h.seed.empty() ? "trial:" + std::to_string(h.trial) : "seed:" + h.seed;
    SEA_LOG(logReach, log::Object)
        .kv("target", t.id)
        .kv("result", "reachable")
        .kv("via", "run")
        .kv("by", d.by);
    // the assumptions a decision rests on: its run's, and its A-INB check's
    if (deciding.insert(h.trial).second) {
      out.addr0Uses += h.addr0IsNullUses;
      out.memhavocUses += h.memhavocNullNoopUses;
      if (auto it = abstractRuns.find(h.trial); it != abstractRuns.end()) {
        out.addr0Uses += it->second.r.addr0IsNullUses;
        out.memhavocUses += it->second.r.memhavocNullNoopUses;
      }
    }
    if (!h.seed.empty()) {
      d.witness = h.seed;
    } else if (!options.witnessDir.empty()) {
      llvm::SmallString<128> path(options.witnessDir);
      llvm::sys::path::append(path, "reach-" + Twine(i) + ".txt");
      std::string at = formatLoc(t.loc);
      std::vector<std::string> header{
          "sea-vc --reach: " + t.id + " op=" + t.opName +
              " at=" + (at.empty() ? "-" : at) +
              " via=run trial=" + std::to_string(h.trial),
          replaysOn(options)};
      if (failed(writeCorpusFile(path, header, h.input, error)))
        return failure();
      d.witness = path.str().str();
    }
  }
  out.seconds = rep->seconds;
  return out;
}

int runReach(ProgramOp program, llvm::ArrayRef<ReachTarget> targets,
             const VCGen &vcgen, Expr query, SeaOpSem &sem, OpSemContext &ctx,
             const ReachOptions &options, const ReachHooks &hooks,
             out::Emitter &out, const ReachRuns *runs) {
  const Stopwatch start;
  // a guard the query does not mention would be unconstrained (a sat for
  // nothing): only the entry's may be absent (it executes on every run)
  ExprSet consts;
  filter(query, bind::IsConst(), std::inserter(consts, consts.begin()));
  Block *entry = &program.getBody().front();

  llvm::DenseMap<Operation *, size_t> index;
  for (auto [i, t] : llvm::enumerate(targets))
    index[t.op] = i;
  if (!options.witnessDir.empty())
    if (std::error_code ec =
            llvm::sys::fs::create_directories(options.witnessDir)) {
      diag::error("vc.reach-witness", "cannot create '" + options.witnessDir +
                                          "': " + ec.message());
      return 1;
    }

  // made at the first query (none when runs decided every target)
  std::unique_ptr<solver::Solver> shared;
  std::optional<AbstractSites> sites;
  if (options.abstractProgram)
    sites.emplace(options.abstractProgram);
  // printed at the end, in program order: a run that starts again
  // (ReachHooks::withModel) prints nothing of this one
  std::vector<std::optional<out::Record>> records(targets.size());
  auto emitRecords = [&] {
    for (const std::optional<out::Record> &r : records)
      if (r)
        out.emit(*r);
  };
  // ReachOptions::modelRuns: the targets a model's run reached before their
  // turn (the target whose model it was; -1: none)
  std::vector<int64_t> modelBy(targets.size(), -1);
  std::vector<bool> modelInbUndetermined(targets.size(), false);
  std::vector<std::string> witnessOf(targets.size());
  std::vector<size_t> order(targets.size());
  for (size_t k = 0; k < order.size(); ++k)
    order[k] = options.order == ReachOrder::Deepest ? order.size() - 1 - k : k;
  std::vector<Verdict> verdicts(targets.size(), Verdict::Unknown);
  std::vector<bool> hitByRun(targets.size(), false);
  unsigned nReach = 0, nUnreach = 0, nUnknown = 0, queries = 0;
  // steps of the reach runs that rested on the interpreter's assumptions
  uint64_t addr0Uses = 0, memhavocUses = 0;
  bool bug = false, memError = false, error = false;
  // what decided the targets: concrete runs, the CFG, the solver
  unsigned byRuns = 0, byCfg = 0, bySolver = 0, byModelRuns = 0;
  if (runs) {
    addr0Uses += runs->addr0Uses;
    memhavocUses += runs->memhavocUses;
  }
  for (size_t i : order) {
    const ReachTarget &t = targets[i];
    const Stopwatch t0;
    std::string at = formatLoc(t.loc);
    if (at.empty())
      at = "-";
    if (modelBy[i] >= 0) {
      // the run of an earlier query's model reached it: no query
      const ReachTarget &src = targets[modelBy[i]];
      verdicts[i] = Verdict::Reachable;
      ++nReach;
      ++byModelRuns;
      out::Record rec("reach");
      rec.str("id", t.id).str("status", "reachable").str("op", t.opName);
      rec.str("via", "model-run").str("by", src.id);
      raw_string_ostream os(rec.text);
      os << "reach: " << t.id << " reachable op=" << t.opName
         << " via=model-run by=" << src.id;
      if (modelInbUndetermined[i]) {
        rec.str("inb", "undetermined");
        os << " inb=undetermined";
      }
      if (const std::string &w = witnessOf[modelBy[i]]; !w.empty()) {
        rec.str("witness", w);
        os << " witness=" << w;
      }
      rec.set("time", out::jtoken("0.000"));
      os << " time=0.000 at=" << at;
      addLocFields(rec, "at", t.loc);
      addSite(rec, os, t);
      records[i] = std::move(rec);
      continue;
    }
    if (runs && runs->decisions[i].reached) {
      // a concrete run reached it (runReachRuns): no query
      const ReachRunDecision &d = runs->decisions[i];
      verdicts[i] = Verdict::Reachable;
      ++nReach;
      ++byRuns;
      out::Record rec("reach");
      rec.str("id", t.id).str("status", "reachable").str("op", t.opName);
      rec.str("via", "run").str("by", d.by);
      raw_string_ostream os(rec.text);
      os << "reach: " << t.id << " reachable op=" << t.opName
         << " via=run by=" << d.by;
      if (d.inbUndetermined) {
        rec.str("inb", "undetermined");
        os << " inb=undetermined";
      }
      if (!d.witness.empty()) {
        rec.str("witness", d.witness);
        os << " witness=" << d.witness;
      }
      rec.set("time", out::jtoken("0.000"));
      os << " time=0.000 at=" << at;
      addLocFields(rec, "at", t.loc);
      addSite(rec, os, t);
      records[i] = std::move(rec);
      continue;
    }
    // the block right before the target (sea-split-at-checks)
    Block *b = t.op->getBlock()->getSinglePredecessor();
    if (!b) {
      diag::error("vc.reach-form",
                  "a reach target without a block right before it (run "
                  "sea-split-at-checks after listing the targets)",
                  t.op->getLoc());
      return 1;
    }
    Expr g = vcgen.guard(b);
    out::Record rec("reach");
    rec.str("id", t.id);
    std::string extra; // text after the verdict, before at=
    std::string reason, replayStatus, witness, inbField;
    uint64_t mismatches = 0;
    Verdict v = Verdict::Unknown;
    if (!options.query) {
      // the runs only (ReachOptions::query): no solver
      reason = "no-query";
    } else if (!g) {
      // no path from the entry reaches the block
      v = Verdict::Unreachable;
      reason = "cfg";
    } else if (b != entry && !consts.count(g)) {
      diag::warn("vc.reach-guard-free",
                 "the guard of a reach target's block is not in the query "
                 "(it would be unconstrained): answered unknown",
                 t.op->getLoc());
      reason = "guard-free";
    } else {
      std::unique_ptr<solver::Solver> own;
      if (options.incremental && !shared) {
        shared = hooks.makeSolver();
        if (!shared)
          return 1;
      }
      if (!shared) {
        own = hooks.makeSolver();
        if (!own)
          return 1;
      }
      solver::Solver &s = shared ? *shared : *own;
      ExprVector lits{g};
      ++queries;
      const Stopwatch solveStart;
      // a solver of its own: the guard is asserted and the check is a
      // plain one (z3 answers check-sat-assuming with its incremental
      // core, which can be much slower than its one-shot solver)
      if (own && !own->add(g))
        return 1;
      solver::SolverResult res =
          own ? own->check()
              : s.check_with_assumptions(
                    llvm::make_range(lits.cbegin(), lits.cend()));
      SEA_LOG(logReach, log::Object)
          .kv("target", t.id)
          .kv("result", res == solver::SolverResult::SAT     ? "sat"
                        : res == solver::SolverResult::UNSAT ? "unsat"
                                                             : "unknown")
          .kv("solve-time", solveStart.seconds());
      if (res == solver::SolverResult::UNSAT) {
        v = Verdict::Unreachable;
      } else if (res == solver::SolverResult::SAT) {
        v = Verdict::Reachable;
        const Stopwatch replayStart;
        int rc = hooks.withModel(s, [&](solver::Model &m) {
          ReachReplay rp =
              replayToTarget(program, m, sem, ctx, index, t.op, options);
          mismatches = rp.mismatches;
          addr0Uses += rp.run.addr0IsNullUses;
          memhavocUses += rp.run.memhavocNullNoopUses;
          std::string inbMessage;
          std::optional<AbstractRun> abs;
          if (options.abstractProgram)
            abs = runAbstract(options.abstractProgram, rp.run, options);
          InbCheck inb =
              abs ? abs->check(t, *sites, inbMessage) : InbCheck::None;
          if (abs) {
            addr0Uses += abs->r.addr0IsNullUses;
            memhavocUses += abs->r.memhavocNullNoopUses;
          }
          // every target the run reaches before its turn is reachable
          // (the run is an execution; under --seam, the ones its input
          // program's run reaches inside every object)
          if (options.modelRuns && rp.reached && !rp.mismatches)
            for (size_t h : rp.hit) {
              if (h == i || verdicts[h] != Verdict::Unknown || records[h] ||
                  modelBy[h] >= 0 || (runs && runs->decisions[h].reached))
                continue;
              std::string m;
              InbCheck hc =
                  abs ? abs->check(targets[h], *sites, m) : InbCheck::None;
              if (hc == InbCheck::Violated || hc == InbCheck::Unordered)
                continue;
              modelBy[h] = int64_t(i);
              modelInbUndetermined[h] = hc == InbCheck::Undetermined;
            }
          if (inb == InbCheck::Undetermined || inb == InbCheck::Unordered)
            inbField = "undetermined";
          // the runs that show what the program does: to their target, in
          // agreement with the model, inside every object
          if (rp.reached && !rp.mismatches &&
              (inb == InbCheck::None || inb == InbCheck::Ok))
            for (size_t h : rp.hit)
              hitByRun[h] = true;
          if (inb == InbCheck::Violated) {
            // as for a counterexample (vc.seam-inb-spurious): the model
            // rests on A-INB, which its run breaks; a mismatch is explained
            replayStatus = "memory-error";
            memError = true;
            diag::warn("vc.reach-replay-memory-error",
                       "a reach model's run leaves an object before its "
                       "target (the input program, every access checked: " +
                           inbMessage +
                           "): the answer rests on A-INB, which the run "
                           "breaks; the program has a memory-safety error",
                       t.op->getLoc());
          } else if (rp.mismatches) {
            replayStatus = "mismatch";
            bug = true;
            diag::error("vc.reach-replay-mismatch",
                        Twine(rp.mismatches) +
                            " value(s) before the reach target differ "
                            "between the model and the interpreter: a VC or "
                            "interpreter bug",
                        t.op->getLoc());
          } else if (rp.reached) {
            replayStatus = "reached";
          } else if (rp.run.status == RunStatus::Error &&
                     isMemorySafetyKind(rp.run.errorKind)) {
            replayStatus = "memory-error";
            memError = true;
            diag::warn("vc.reach-replay-memory-error",
                       "the replay of a reach model stops at a "
                       "memory-safety error before its target: the answer "
                       "rests on A-INB, which the run breaks",
                       t.op->getLoc());
          } else if (rp.run.status == RunStatus::Error) {
            replayStatus = "error";
            error = true;
            diag::error("vc.replay-impossible",
                        "a reach model cannot be replayed: " + rp.run.message,
                        t.op->getLoc());
          } else {
            replayStatus = "not-reached";
            bug = true;
            diag::error("vc.reach-replay-not-reached",
                        "the replay of a reach model ends (" +
                            stringifyRunStatus(rp.run.status) +
                            ") before its target: a VC or interpreter bug",
                        t.op->getLoc());
          }
          if (!options.witnessDir.empty()) {
            llvm::SmallString<128> path(options.witnessDir);
            llvm::sys::path::append(path, "reach-" + Twine(i) + ".txt");
            if (writeWitness(path, t, at, options, rp.run))
              witness = witnessOf[i] = path.str().str();
            else
              error = true;
          }
        });
        SEA_LOG(logReach, log::Object)
            .kv("target", t.id)
            .kv("replay", replayStatus)
            .kv("replay-time", replayStart.seconds());
        if (rc) {
          // a rerun (sea-vc's retries) prints its own records; an error
          // prints the targets answered so far
          if (rc > 0)
            emitRecords();
          return rc;
        }
      } else {
        reason = hooks.reasonUnknown ? hooks.reasonUnknown(s, t0.seconds())
                                     : s.reason_unknown();
      }
    }
    verdicts[i] = v;
    if (v != Verdict::Unknown)
      ++(reason == "cfg" ? byCfg : bySolver);
    ++(v == Verdict::Reachable     ? nReach
       : v == Verdict::Unreachable ? nUnreach
                                   : nUnknown);
    const double sec = t0.seconds();
    rec.str("status", verdictName(v)).str("op", t.opName);
    raw_string_ostream os(rec.text);
    os << "reach: " << t.id << " " << verdictName(v) << " op=" << t.opName;
    if (!reason.empty()) {
      // one token in the text
      std::string r = reason;
      std::replace(r.begin(), r.end(), ' ', '_');
      rec.str("reason", r);
      os << " reason=" << r;
    }
    if (!replayStatus.empty()) {
      rec.str("replay", replayStatus);
      os << " replay=" << replayStatus;
    }
    if (!inbField.empty()) {
      rec.str("inb", inbField);
      os << " inb=" << inbField;
    }
    if (mismatches) {
      rec.num("mismatches", mismatches);
      os << " mismatches=" << mismatches;
    }
    if (!witness.empty()) {
      rec.str("witness", witness);
      os << " witness=" << witness;
    }
    std::string secText = secondsText(sec);
    rec.set("time", out::jtoken(secText));
    os << " time=" << secText << " at=" << at;
    addLocFields(rec, "at", t.loc);
    addSite(rec, os, t);
    records[i] = std::move(rec);
  }
  emitRecords();
  // a run reached a target the VC calls unreachable: one of them is wrong
  for (auto [i, t] : llvm::enumerate(targets))
    if (hitByRun[i] && verdicts[i] == Verdict::Unreachable) {
      bug = true;
      diag::error("vc.reach-inconsistent",
                  "a replayed run reaches " + t.id +
                      ", which the VC answers unreachable: a VC or "
                      "interpreter bug",
                  t.op->getLoc());
    }
  out.lines() << "reach.summary: targets=" << targets.size()
              << " reachable=" << nReach << " unreachable=" << nUnreach
              << " unknown=" << nUnknown << " queries=" << queries
              << " time=" << secondsText(start.seconds());
  if (options.modelRuns)
    out.lines() << " model-runs=" << byModelRuns;
  out.lines() << "\n";
  if (runs)
    out.lines() << "reach.runs: decided=" << byRuns
                << " model-run=" << byModelRuns << " solver=" << bySolver
                << " cfg=" << byCfg << " unknown=" << nUnknown
                << " trials=" << runs->trials << " seeds=" << runs->seedTrials
                << " inb-violated=" << runs->inbViolated
                << " time=" << secondsText(runs->seconds) << "\n";
  if (addr0Uses)
    out.lines() << "assume.addr0-is-null: uses=" << addr0Uses << "\n";
  if (memhavocUses)
    out.lines() << "assume.memhavoc-null-noop: uses=" << memhavocUses << "\n";
  out.flush();
  if (error)
    return 1;
  if (bug)
    return 2;
  if (memError)
    return 4;
  return nUnknown ? 3 : 0;
}

} // namespace sea
