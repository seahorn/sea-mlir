// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Reach.cpp - reachability of check sites and blocks -------*- C++ -*-===//

#include "sea/VC/Reach.h"

#include "sea/Support/Diag.h"
#include "sea/Support/Log.h"
#include "sea/VC/Replay.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <chrono>

using namespace mlir;
using namespace expr;

namespace sea {

static log::Tag logReach("vc.reach", 2,
                         "each sea-vc --reach query: target, answer, solve "
                         "and replay time");

namespace {
/// `f.c:3:5@g.c:9:2`: the position, then the call sites; empty without a
/// file position.
std::string positionId(Location loc) {
  SourcePositions p = positionsOf(loc);
  std::string id;
  for (auto [i, f] : llvm::enumerate(p)) {
    id += i ? "@" : "";
    // a position in the .mlir file itself (an op the exporter left without
    // a source position): its file by basename, so that copies of one
    // program in other directories give the same ids
    StringRef file = f.getFilename().getValue();
    if (file.ends_with(".mlir"))
      id += (llvm::sys::path::filename(file) + ":" + Twine(f.getLine()) + ":" +
             Twine(f.getColumn()))
                .str();
    else
      id += fileLocText(f);
  }
  return id;
}
} // namespace

std::vector<ReachTarget> collectReachTargets(ProgramOp program,
                                             ReachKind kind) {
  std::vector<ReachTarget> targets;
  auto add = [&](Operation *op, StringRef name, Location loc, std::string id) {
    targets.push_back({std::move(id), name.str(), op, loc});
  };
  for (Block &b : program.getBody()) {
    if (kind == ReachKind::Blocks) {
      Operation *named = nullptr;
      for (Operation &op : b)
        if (!positionsOf(op.getLoc()).empty()) {
          named = &op;
          break;
        }
      Operation *top = &b.front();
      add(top, "block", named ? named->getLoc() : top->getLoc(),
          named ? positionId(named->getLoc()) : "");
      continue;
    }
    for (Operation &op : b)
      if (isa<AssumeOp, AssertOp, UnreachableOp>(op))
        add(&op, op.getName().getStringRef(), op.getLoc(),
            positionId(op.getLoc()));
  }
  // unlocated: by order; a repeated id: #<n> from the second on
  unsigned unlocated = 0;
  llvm::StringMap<unsigned> seen;
  for (ReachTarget &t : targets) {
    if (t.id.empty())
      t.id = "unlocated#" + std::to_string(unlocated++);
    unsigned n = ++seen[t.id];
    if (n > 1)
      t.id += "#" + std::to_string(n);
  }
  return targets;
}

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
/// program run on the model's nondet values. `violated`: it leaves an
/// object before it reaches an op of the target's kind at the target's
/// location (a folded assume of false, sea.unreachable, is a sea.assume
/// there; a block, any op at its location), or without reaching one.
/// `undetermined`: the run reads a nondet memory cell (the model's cells
/// are addresses, not object offsets) or stops otherwise without deciding.
/// `after`: it leaves an object only after the target (the answer stands,
/// the rest of the run does not).
enum class InbCheck { None, Ok, After, Violated, Undetermined };

InbCheck checkAbstract(ProgramOp abs, const ReachTarget &t,
                       const RunResult &placed, const ReachOptions &ro,
                       std::string &message, RunResult &r) {
  InputSource inputs(InputSource::Missing::Error);
  for (const NondetRead &rd : placed.reads) {
    llvm::SmallString<32> text;
    rd.value.toStringUnsigned(text);
    inputs.set(rd.site, std::string(text));
  }
  bool reached = false;
  // the options of sea-vc's A-INB check of a counterexample
  RunOptions options;
  options.prints = false;
  options.warnEarlierVersion = false;
  options.addr0IsNull = ro.addr0IsNull;
  options.memhavocNullNoop = ro.memhavocNullNoop;
  options.onOp = [&](Operation &op, auto) {
    if (reached || op.getLoc() != t.loc)
      return;
    StringRef name = op.getName().getStringRef();
    reached = t.opName == "block" || name == t.opName ||
              (t.opName == "sea.unreachable" && name == "sea.assume");
  };
  r = interpret(abs, inputs, options);
  if (isOutOfObjectAccess(r)) {
    message = r.message;
    return reached ? InbCheck::After : InbCheck::Violated;
  }
  if (r.status == RunStatus::Error && !reached) {
    message = r.message;
    return InbCheck::Undetermined;
  }
  return InbCheck::Ok;
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
  if (options.lowered)
    os << "# replays on: the lowered program (sea-opt --sea-vc-lower with "
          "the run's lowering options, of "
       << (options.programPath.empty() ? "<program>" : options.programPath)
       << "), with sea-run --input-file=<this file>\n";
  else
    os << "# replays on: the input program, with sea-run "
       << (options.programPath.empty() ? "<program>" : options.programPath)
       << " --input-file=<this file>\n";
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

int runReach(ProgramOp program, llvm::ArrayRef<ReachTarget> targets,
             const VCGen &vcgen, Expr query, SeaOpSem &sem, OpSemContext &ctx,
             const ReachOptions &options, const ReachHooks &hooks,
             out::Emitter &out) {
  using Clock = std::chrono::steady_clock;
  auto since = [](Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
  };
  const auto start = Clock::now();
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

  std::unique_ptr<solver::Solver> shared;
  if (options.incremental) {
    shared = hooks.makeSolver();
    if (!shared)
      return 1;
  }
  // printed at the end: a run that starts again (ReachHooks::withModel)
  // prints nothing of this one
  std::vector<out::Record> records;
  std::vector<Verdict> verdicts(targets.size(), Verdict::Unknown);
  std::vector<bool> hitByRun(targets.size(), false);
  unsigned nReach = 0, nUnreach = 0, nUnknown = 0, queries = 0;
  // steps of the reach runs that rested on the interpreter's assumptions
  uint64_t addr0Uses = 0, memhavocUses = 0;
  bool bug = false, memError = false, error = false;
  for (auto [i, t] : llvm::enumerate(targets)) {
    const auto t0 = Clock::now();
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
    std::string at = formatLoc(t.loc);
    if (at.empty())
      at = "-";
    out::Record rec("reach");
    rec.str("id", t.id);
    std::string extra; // text after the verdict, before at=
    std::string reason, replayStatus, witness, inbField;
    uint64_t mismatches = 0;
    Verdict v = Verdict::Unknown;
    if (!g) {
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
      if (!shared) {
        own = hooks.makeSolver();
        if (!own)
          return 1;
      }
      solver::Solver &s = shared ? *shared : *own;
      ExprVector lits{g};
      ++queries;
      const auto solveStart = Clock::now();
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
          .kv("solve-time", since(solveStart));
      if (res == solver::SolverResult::UNSAT) {
        v = Verdict::Unreachable;
      } else if (res == solver::SolverResult::SAT) {
        v = Verdict::Reachable;
        const auto replayStart = Clock::now();
        int rc = hooks.withModel(s, [&](solver::Model &m) {
          ReachReplay rp =
              replayToTarget(program, m, sem, ctx, index, t.op, options);
          mismatches = rp.mismatches;
          addr0Uses += rp.run.addr0IsNullUses;
          memhavocUses += rp.run.memhavocNullNoopUses;
          std::string inbMessage;
          RunResult absRun;
          InbCheck inb = options.abstractProgram
                             ? checkAbstract(options.abstractProgram, t, rp.run,
                                             options, inbMessage, absRun)
                             : InbCheck::None;
          addr0Uses += absRun.addr0IsNullUses;
          memhavocUses += absRun.memhavocNullNoopUses;
          if (inb == InbCheck::Undetermined)
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
              witness = path.str().str();
            else
              error = true;
          }
        });
        SEA_LOG(logReach, log::Object)
            .kv("target", t.id)
            .kv("replay", replayStatus)
            .kv("replay-time", since(replayStart));
        if (rc) {
          // a rerun (sea-vc's retries) prints its own records; an error
          // prints the targets answered so far
          if (rc > 0)
            for (const out::Record &r : records)
              out.emit(r);
          return rc;
        }
      } else {
        reason = hooks.reasonUnknown ? hooks.reasonUnknown(s, since(t0))
                                     : s.reason_unknown();
      }
    }
    verdicts[i] = v;
    ++(v == Verdict::Reachable     ? nReach
       : v == Verdict::Unreachable ? nUnreach
                                   : nUnknown);
    const double sec = since(t0);
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
    std::string secText = llvm::formatv("{0:f3}", sec).str();
    rec.set("time", out::jtoken(secText));
    os << " time=" << secText << " at=" << at;
    addLocFields(rec, "at", t.loc);
    records.push_back(std::move(rec));
  }
  for (const out::Record &r : records)
    out.emit(r);
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
              << " time=" << llvm::formatv("{0:f3}", since(start)).str()
              << "\n";
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
