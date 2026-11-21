// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- CheckInbJob.h - sea-vc --check-inb: the A-INB discharge job -*- C++
//-*-===//
//
// docs/design/CheckInb.md. The selected abstract program is instrumented
// (sea-check-inb: a bounds check before every access, the program's asserts
// dropped, tier (a) static discharge) and split (sea-split-asserts: one
// program per check left, the earlier checks assumed). Each part is then an
// ordinary safety job: sea-vc runs itself on it, with the command line's
// options (--lower, --seam, --ackermannize=off, the solver, --timeout, ...),
// in a process of its own, and reads its jsonl output. One
// `check-inb.part:` line per part, then the verdict: unsat iff every part
// is unsat.
//
// A part runs in its own process: the safety flow of sea-vc is one long
// main() with global state (the output emitter, the warning summary, the
// assumption ledger), and a part needs all of it, fresh. A process per
// part also bounds each part's memory (--mem-limit) and leaves room for
// running parts in parallel later.
//
//===----------------------------------------------------------------------===//
#ifndef SEA_VC_TOOL_CHECKINBJOB_H
#define SEA_VC_TOOL_CHECKINBJOB_H

#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Interp/Interpreter.h"
#include "sea/Support/Diag.h"
#include "sea/Support/Output.h"
#include "sea/Support/WriteFile.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sea::checkinb {

struct JobOptions {
  /// this sea-vc
  std::string self;
  /// the options every part runs with (the command line's, without the
  /// input, --check-inb*, --program and --format)
  std::vector<std::string> partArgs;
  /// where the parts and their outputs go (empty: a temporary directory,
  /// removed at the end)
  std::string dir;
  bool staticDischarge = true;
  bool memhavocNullNoop = true;
  /// stop starting parts after this many seconds (0: no limit); the parts
  /// not run are `skipped` and the verdict is unknown
  unsigned maxSeconds = 0;
  /// sea-lower-box-all's expand-max (sea-vc --box-all-expand)
  unsigned boxAllExpand = kBoxAllExpandDefault;
};

/// What one part's run said.
struct PartRun {
  unsigned index = 0;
  int64_t checkId = -1;
  std::string at;
  std::string result = "error";
  int status = 1;
  double seconds = 0, solveSeconds = -1;
  std::string failAt;
  /// the named assumptions its answer rests on: the `assume.<name>` lines
  /// that are in effect (a field not 0, or the state word `vc`)
  std::vector<std::string> assumes;
  /// the kinds of the warnings and errors on its stderr (`... [kind]`)
  std::vector<std::string> diagKinds;
  /// the first error line of its stderr (`Error: ...` or an MLIR
  /// `error:`), and its kind (`[kind]`, else empty)
  std::string firstError, errorKind;
};

inline std::string locText(mlir::Location loc) {
  if (auto fl = loc->findInstanceOf<mlir::FileLineColLoc>())
    return (fl.getFilename().strref() + ":" + llvm::Twine(fl.getLine()) + ":" +
            llvm::Twine(fl.getColumn()))
        .str();
  std::string s;
  llvm::raw_string_ostream os(s);
  loc.print(os);
  return s;
}

/// Reads a part's jsonl output: the result, the solver time, the failing
/// assert's location.
inline void readPartOutput(llvm::StringRef path, PartRun &r) {
  auto buf = llvm::MemoryBuffer::getFile(path);
  if (!buf)
    return;
  llvm::SmallVector<llvm::StringRef> lines;
  (*buf)->getBuffer().split(lines, '\n', -1, false);
  for (llvm::StringRef line : lines) {
    llvm::Expected<llvm::json::Value> v = llvm::json::parse(line);
    if (!v) {
      llvm::consumeError(v.takeError());
      continue;
    }
    const llvm::json::Object *o = v->getAsObject();
    if (!o)
      continue;
    std::optional<llvm::StringRef> kind = o->getString("kind");
    if (!kind)
      continue;
    if (*kind == "result") {
      if (auto s = o->getString("value"))
        r.result = s->str();
    } else if (*kind == "stat") {
      auto key = o->getString("key");
      if (key && *key == "solve-time") {
        if (auto d = o->getNumber("value"))
          r.solveSeconds = *d;
        else if (auto s = o->getString("value")) {
          double x;
          if (!s->getAsDouble(x))
            r.solveSeconds = x;
        }
      }
      // a named assumption in effect: a nonzero count or `vc`
      if (key && key->starts_with("assume.")) {
        bool active = false;
        for (const auto &[k, val] : *o) {
          if (k == "kind" || k == "key" || k == "text")
            continue;
          if (auto n = val.getAsNumber())
            active |= *n != 0;
          else if (auto s = val.getAsString())
            active |= !s->empty() && *s != "0";
        }
        std::string name =
            key->drop_front(llvm::StringRef("assume.").size()).str();
        if (active && !llvm::is_contained(r.assumes, name))
          r.assumes.push_back(name);
      }
    } else if (*kind == "replay") {
      if (auto s = o->getString("at"))
        r.failAt = s->str();
    }
  }
}

/// The diagnostic kinds of a part's stderr: the `[kind]` at the end of
/// each `Warning:` / `Error:` line, once each.
inline void readPartDiags(llvm::StringRef path, PartRun &r) {
  auto buf = llvm::MemoryBuffer::getFile(path);
  if (!buf)
    return;
  llvm::SmallVector<llvm::StringRef> lines;
  (*buf)->getBuffer().split(lines, '\n', -1, false);
  for (llvm::StringRef line : lines) {
    line = line.rtrim();
    if (r.firstError.empty() &&
        (line.starts_with("Error:") || line.contains("error:"))) {
      r.firstError = line.str();
      if (line.ends_with("]") && line.rfind('[') != llvm::StringRef::npos)
        r.errorKind = line.substr(line.rfind('[') + 1).drop_back().str();
    }
    if (!(line.starts_with("Warning:") || line.starts_with("Error:")) ||
        !line.ends_with("]"))
      continue;
    size_t open = line.rfind('[');
    if (open == llvm::StringRef::npos)
      continue;
    std::string kind = line.substr(open + 1, line.size() - open - 2).str();
    if (!llvm::is_contained(r.diagKinds, kind))
      r.diagKinds.push_back(kind);
  }
}

/// A part without its assert: a bug of the split.
inline mlir::LogicalResult postconditionFailedNoAssert(ProgramOp part) {
  diag::error("vc.check-inb-part",
              "sea-check-inb: part @" + part.getSymName() +
                  " has no assert (a bug of sea-split-asserts)",
              part.getLoc());
  return mlir::failure();
}

/// The worst of two exit statuses of parts: a bug or an error (5, 2, 6, 4,
/// 1, in that order) over unknown (3) over a decided part (0).
inline int worseStatus(int a, int b) {
  auto rank = [](int s) {
    switch (s) {
    case 0:
      return 0;
    case 3:
      return 1;
    case 1:
      return 2;
    case 4:
      return 3;
    case 6:
      return 4;
    case 2:
      return 5;
    case 5:
      return 6;
    default:
      return 2;
    }
  };
  return rank(a) >= rank(b) ? a : b;
}

inline int runJob(mlir::ModuleOp module, ProgramOp program, const JobOptions &o,
                  out::Emitter &out) {
  using Clock = std::chrono::steady_clock;
  auto secondsSince = [](Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
  };
  mlir::MLIRContext *ctx = module.getContext();
  // the selected program only
  for (ProgramOp p : llvm::make_early_inc_range(module.getOps<ProgramOp>()))
    if (p != program)
      p.erase();
  std::string name = program.getSymName().str();
  {
    // boxes first, as the lowering does (tracking markers, then boxes):
    // the loads of a box's body become ordinary loads at the box's place,
    // which sea-check-inb checks; a box not in assert polarity is refused
    // here as in --lower. In each part's own --lower both passes are then
    // no-ops (no marker, no box left).
    mlir::PassManager pm(ctx);
    mlir::OpPassManager &prog = pm.nest<ProgramOp>();
    prog.addPass(createApplyTrackingPass());
    prog.addPass(createLowerBoxAllPass(o.boxAllExpand));
    prog.addPass(createCheckInbPass(o.staticDischarge, o.memhavocNullNoop));
    if (mlir::failed(pm.run(module)))
      return 1;
  }
  // the pointer width (the narrowest one, if several)
  unsigned ptrWidth = 64;
  {
    unsigned w = 0;
    program.walk([&](mlir::Operation *op) {
      for (mlir::Type t : op->getResultTypes())
        if (auto pt = mlir::dyn_cast<PtrType>(t))
          w = w ? std::min(w, pt.getWidth()) : pt.getWidth();
    });
    if (w)
      ptrWidth = w;
  }
  // the largest object, as placement sizes it (sea-check-inb records it)
  llvm::APInt largestObject(128, 0);
  if (auto l =
          program->getAttrOfType<mlir::IntegerAttr>(kCheckInbLargestObjectAttr))
    largestObject = l.getValue().zextOrTrunc(128);
  uint64_t checks = 0, staticN = 0, left = 0, zero = 0;
  if (auto d =
          program->getAttrOfType<mlir::DictionaryAttr>(kCheckInbStatsAttr)) {
    auto get = [&](llvm::StringRef k) -> uint64_t {
      auto a = d.getAs<mlir::IntegerAttr>(k);
      return a ? a.getInt() : 0;
    };
    checks = get("checks");
    staticN = get("static");
    left = get("left");
    zero = get("zero-length");
  }
  {
    out::Record rec("check-inb");
    rec.num("checks", checks)
        .num("static", staticN)
        .num("left", left)
        .num("zero_length", zero);
    rec.text = ("check-inb: checks=" + llvm::Twine(checks) + " static=" +
                llvm::Twine(staticN) + " left=" + llvm::Twine(left) +
                " zero-length=" + llvm::Twine(zero))
                   .str();
    out.emit(rec);
  }
  // the parts are the programs the split makes: an input that carries the
  // attribute already (a part of an earlier split) is not one
  program->removeAttr(kSplitAssertAttr);
  {
    mlir::PassManager pm(ctx);
    pm.addPass(createSplitAssertsPass(name));
    if (mlir::failed(pm.run(module)))
      return 1;
  }
  std::vector<ProgramOp> parts;
  for (ProgramOp p : module.getOps<ProgramOp>())
    if (p->hasAttr(kSplitAssertAttr))
      parts.push_back(p);

  // the work directory
  std::string dir = o.dir;
  bool tempDir = dir.empty();
  if (tempDir) {
    llvm::SmallString<128> d;
    if (std::error_code ec =
            llvm::sys::fs::createUniqueDirectory("sea-vc-check-inb", d)) {
      diag::error("vc.check-inb-dir",
                  "cannot make a temporary directory: " + ec.message());
      return 1;
    }
    dir = d.str().str();
  } else if (std::error_code ec = llvm::sys::fs::create_directories(dir)) {
    diag::error("vc.check-inb-dir", "cannot make " + dir + ": " + ec.message());
    return 1;
  }

  // a temporary directory goes on every return from here
  auto removeTemp = llvm::scope_exit([&] {
    if (tempDir)
      (void)llvm::sys::fs::remove_directories(dir);
  });
  std::vector<PartRun> runs;
  int worst = 0;
  unsigned skipped = 0;
  auto t0 = Clock::now();
  for (auto [i, part] : llvm::enumerate(parts)) {
    PartRun r;
    r.index = i;
    AssertOp a;
    part.walk([&](AssertOp x) { a = x; });
    if (!a) {
      (void)postconditionFailedNoAssert(part);
      return 1;
    }
    r.at = locText(a.getLoc());
    if (auto id = a->getAttrOfType<mlir::IntegerAttr>(kInbCheckAttr))
      r.checkId = id.getInt();
    if (o.maxSeconds && secondsSince(t0) > o.maxSeconds) {
      ++skipped;
      continue;
    }
    std::string base = (dir + "/part-" + llvm::Twine(i)).str();
    std::string mlirFile = base + ".mlir", jsonFile = base + ".jsonl",
                errFile = base + ".err";
    {
      // the part alone, with its source locations
      mlir::OpBuilder b(ctx);
      mlir::OwningOpRef<mlir::ModuleOp> m =
          mlir::ModuleOp::create(b.getUnknownLoc());
      m->getBody()->push_back(part->clone());
      if (mlir::failed(writeFileAtomic(
              mlirFile,
              [&](llvm::raw_ostream &os) {
                m->print(os, mlir::OpPrintingFlags().enableDebugInfo());
                return mlir::success();
              },
              "vc.check-inb-dir", "--check-inb part")))
        return 1;
    }
    std::vector<llvm::StringRef> args{o.self, mlirFile};
    for (const std::string &s : o.partArgs)
      args.push_back(s);
    args.push_back("--format=jsonl");
    // the last occurrence wins: a --config file or response file that
    // names --check-inb does not run the job again inside the part
    args.push_back("--check-inb=false");
    std::optional<llvm::StringRef> redirects[] = {
        std::nullopt, llvm::StringRef(jsonFile), llvm::StringRef(errFile)};
    auto tp = Clock::now();
    std::string err;
    r.status = llvm::sys::ExecuteAndWait(o.self, args, std::nullopt, redirects,
                                         0, 0, &err);
    r.seconds = secondsSince(tp);
    if (r.status < 0) {
      diag::error("vc.check-inb-part",
                  "part " + llvm::Twine(i) + " did not run: " + err);
      r.status = 1;
    }
    readPartOutput(jsonFile, r);
    readPartDiags(errFile, r);
    if (r.status != 0 && r.status != 3 && r.result != "unknown")
      r.result = r.status == 4 ? "inconclusive" : "error";
    worst = worseStatus(worst, r.status);
    out::Record rec("check-inb.part");
    rec.num("part", i)
        .snum("id", r.checkId)
        .str("at", r.at)
        .str("result", r.result)
        .snum("status", r.status)
        .str("time", llvm::formatv("{0:f3}", r.seconds).str());
    std::string text =
        llvm::formatv("check-inb.part: {0} id={1} at={2} "
                      "result={3} status={4} time={5:f3}",
                      i, r.checkId, r.at, r.result, r.status, r.seconds)
            .str();
    if (r.solveSeconds >= 0) {
      rec.str("solve_time", llvm::formatv("{0:f3}", r.solveSeconds).str());
      text += llvm::formatv(" solve-time={0:f3}", r.solveSeconds).str();
    }
    if (!r.failAt.empty()) {
      rec.str("fail_at", r.failAt);
      text += " fail-at=" + r.failAt;
    }
    // why a part failed, without --check-inb-dir: its error's kind here,
    // its first error line on check-inb.part-error
    const bool failed = r.status != 0 && r.status != 3;
    if (failed) {
      std::string kind = r.errorKind.empty() ? "-" : r.errorKind;
      rec.str("error", kind);
      text += " error=" + kind;
    }
    rec.text = text;
    out.emit(rec);
    if (failed) {
      out::Record er("check-inb.part-error");
      er.num("part", i).str("message", r.firstError);
      er.text =
          llvm::formatv("check-inb.part-error: {0} {1}", i,
                        r.firstError.empty() ? "(no error line)" : r.firstError)
              .str();
      out.emit(er);
    }
    out.flush();
    runs.push_back(r);
  }

  unsigned unsat = 0, sat = 0, unknown = 0, other = 0;
  std::vector<double> times;
  // no "slowest part" here: which part is slowest is a timing, which the
  // part lines give; a summary field of it would differ between two runs
  // that agree on everything else
  double maxT = 0;
  for (const PartRun &r : runs) {
    times.push_back(r.seconds);
    maxT = std::max(maxT, r.seconds);
    if (r.status == 0 && r.result == "unsat")
      ++unsat;
    else if (r.status == 0 && r.result == "sat")
      ++sat;
    else if (r.result == "unknown")
      ++unknown;
    else
      ++other;
  }
  std::sort(times.begin(), times.end());
  double median =
      times.empty() ? 0
      : times.size() % 2
          ? times[times.size() / 2]
          : (times[times.size() / 2 - 1] + times[times.size() / 2]) / 2;
  {
    out::Record rec("check-inb.parts");
    rec.num("total", parts.size())
        .num("unsat", unsat)
        .num("sat", sat)
        .num("unknown", unknown)
        .num("other", other)
        .num("skipped", skipped)
        .str("median_time", llvm::formatv("{0:f3}", median).str())
        .str("max_time", llvm::formatv("{0:f3}", maxT).str())
        .str("time", llvm::formatv("{0:f3}", secondsSince(t0)).str());
    rec.text =
        llvm::formatv("check-inb.parts: total={0} unsat={1} sat={2} "
                      "unknown={3} other={4} skipped={5} median-time={6:f3} "
                      "max-time={7:f3} time={8:f3}",
                      parts.size(), unsat, sat, unknown, other, skipped, median,
                      maxT, secondsSince(t0))
            .str();
    out.emit(rec);
  }
  // the verdict: sat if a part's failure replayed; unsat iff every part
  // is unsat; else unknown
  std::string verdict = sat                                 ? "sat"
                        : unsat == parts.size() && !skipped ? "unsat"
                                                            : "unknown";
  {
    // the parts' diagnostics, which their own output (stderr) holds: the
    // number of parts that reported each kind. A warning names an
    // approximation or an assumption the part's answer rests on.
    std::map<std::string, unsigned> kinds;
    for (const PartRun &r : runs)
      for (const std::string &k : r.diagKinds)
        ++kinds[k];
    out::Record rec("check-inb.part-diags");
    std::string text = "check-inb.part-diags:";
    if (kinds.empty())
      text += " none";
    for (const auto &[k, n] : kinds) {
      rec.num(k, n);
      text += " " + k + "=" + std::to_string(n);
    }
    rec.text = text;
    out.emit(rec);
  }
  {
    // what an unsat discharges (docs/design/CheckInb.md): A-INB, and the
    // premises of --seam it implies. A-NOWRAP reads an offset as signed,
    // so A-INB implies it only for objects of fewer than 2^(w-1) units:
    // the largest object of the program (after the box lowering, before
    // the split: every part's objects are among them) decides, sized as
    // placement sizes it.
    std::string what = "none";
    if (verdict == "unsat") {
      what = "a-inb a-sep a-null";
      std::string whyNot;
      llvm::APInt half = llvm::APInt::getOneBitSet(128, ptrWidth - 1);
      if (largestObject.uge(half))
        whyNot = "an object of " +
                 llvm::toString(largestObject, 10, /*Signed=*/false) +
                 " units, at least 2^" + std::to_string(ptrWidth - 1);
      if (whyNot.empty())
        what += " a-nowrap";
      else
        diag::warn("check-inb.nowrap-not-implied",
                   "A-INB implies A-NOWRAP only for objects of fewer than "
                   "2^(w-1) units (w = " +
                       llvm::Twine(ptrWidth) + "): " + whyNot +
                       "; a-nowrap is not discharged");
    }
    // what the result rests on: the parts' named assumptions in effect
    // (their assume.* lines), but the premises the job discharges, and the
    // assumptions their placement made explicit (warnings `<kind>-assume`,
    // as place.symbolic-size-assume: an allocation of symbolic size is
    // assumed to fit its room); sorted, `none` without any
    std::set<std::string> under;
    for (const PartRun &r : runs) {
      for (const std::string &a : r.assumes)
        // a part's use of A-NOWRAP is discharged only with it listed
        if (a != "a-sep" && a != "a-null" &&
            (a != "a-nowrap" ||
             llvm::StringRef(what).find("a-nowrap") == llvm::StringRef::npos))
          under.insert(a);
      for (const std::string &k : r.diagKinds)
        if (llvm::StringRef(k).ends_with("-assume"))
          under.insert(k);
    }
    std::string underText =
        under.empty() ? "none" : llvm::join(under.begin(), under.end(), ",");
    out::Record rec("check-inb.discharges");
    rec.str("premises", what).str("under", underText);
    rec.text = "check-inb.discharges: " + what + " under=" + underText;
    out.emit(rec);
  }
  {
    out::Record rec("result");
    rec.str("value", verdict);
    rec.text = "result: " + verdict;
    out.emit(rec);
  }
  out.flush();
  if (worst != 0 && worst != 3)
    return worst;
  return verdict == "unknown" ? 3 : 0;
}

} // namespace sea::checkinb

#endif // SEA_VC_TOOL_CHECKINBJOB_H
