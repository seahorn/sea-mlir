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
#include "sea/Support/Version.h"
#include "sea/Support/WriteFile.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
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
  /// --check-inb-keep: a temporary directory stays (and is printed)
  bool keep = false;
  /// --vcgen-only: write the parts and the manifest, run no part
  bool prepareOnly = false;
  /// the input file (for the manifest: its path and hash) and the job's
  /// own command line (argv[1..])
  std::string source;
  std::vector<std::string> jobArgs;
  /// sea-lower-box-all's expand-max (sea-vc --box-all-expand)
  unsigned boxAllExpand = kBoxAllExpandDefault;
  /// the lowering, as --lower would run it (its options); the job runs its
  /// stages itself (docs/design/CheckInb.md, "Instrument, optimize,
  /// lower, unique")
  VCLowerConfig lower;
  /// the pointer mode resolved on the input (--ptr-mode for each part,
  /// which sees a lowered program)
  std::string ptrMode;
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

/// The options of every part, from the job's expanded command line (no
/// --config, no response file): one `--name[=value]` token per option (a
/// two-token option joined), positionals (the input) dropped, the job's
/// own options and the lowering's dropped (a part is lowered already),
/// file paths made absolute. An option that writes one file every part
/// would share is refused (false, \p bad says which).
inline bool partArguments(llvm::ArrayRef<const char *> args,
                          std::vector<std::string> &out, std::string &bad) {
  // the job's own options, and the lowering's: a part is lowered already
  static const llvm::StringSet<> dropped = {"check-inb",
                                            "check-inb-dir",
                                            "check-inb-keep",
                                            "check-inb-static",
                                            "check-inb-max-time",
                                            "program",
                                            "format",
                                            "vcgen-only",
                                            "config",
                                            "lower",
                                            "lower-opt",
                                            "lower-disable",
                                            "range-fold",
                                            "expand-memmove",
                                            "box-all-expand",
                                            "place-in",
                                            "place-out",
                                            "widen-memory",
                                            "widen-memory-report",
                                            "widen-memory-per-class",
                                            "ptr-mode"};
  // options that write one file (or stdout) every part would share
  static const llvm::StringSet<> shared = {"smt2",           "print-lowered",
                                           "seam-trail",     "ack-dump-pairs",
                                           "ack-relax-smt2", "split-smt2"};
  // options whose value is a path a part reads or appends to
  static const llvm::StringSet<> paths = {"log-file"};
  auto &registered = llvm::cl::getRegisteredOptions();
  bool positional = false;
  for (size_t i = 0; i < args.size(); ++i) {
    llvm::StringRef a(args[i]);
    if (positional || !a.starts_with("-") || a == "-") {
      // the input file (or standard input)
      continue;
    }
    if (a == "--") {
      positional = true;
      continue;
    }
    auto [name, value] = a.ltrim('-').split('=');
    bool hasValue = a.contains('=');
    std::string v = value.str();
    auto it = registered.find(name);
    if (!hasValue && it != registered.end() &&
        it->second->getValueExpectedFlag() == llvm::cl::ValueRequired &&
        i + 1 < args.size()) {
      v = args[++i];
      hasValue = true;
    }
    if (dropped.contains(name))
      continue;
    if (shared.contains(name)) {
      bad = "--" + name.str() +
            " writes one output that every part would share; run it on a "
            "part (--check-inb-dir, the manifest's argv) instead";
      return false;
    }
    if (hasValue && paths.contains(name)) {
      llvm::SmallString<256> abs(v);
      (void)llvm::sys::fs::make_absolute(abs);
      v = abs.str().str();
    }
    out.push_back(hasValue ? "--" + name.str() + "=" + v : "--" + name.str());
  }
  return true;
}

/// One part as the manifest describes it: everything an outside runner
/// needs to run it (docs/design/CheckInb.md, "The manifest").
struct PartSpec {
  unsigned index = 0;
  int64_t checkId = -1;
  std::string at;
  /// file names in the job directory
  std::string mlir, stdoutFile, stderrFile;
  /// the exact command line (argv[0] the sea-vc binary; absolute paths)
  std::vector<std::string> argv;
};

/// The manifest's format version: 1. Fields are only ever added.
constexpr unsigned kManifestVersion = 1;

/// The SHA-256 of a file, hex; empty if it cannot be read.
inline std::string sha256Of(llvm::StringRef path) {
  auto buf = llvm::MemoryBuffer::getFile(path);
  if (!buf)
    return "";
  auto h = llvm::SHA256::hash(llvm::arrayRefFromStringRef((*buf)->getBuffer()));
  return llvm::toHex(h, /*LowerCase=*/true);
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
  // one pass manager run at a time, on one thread (the passes report
  // through the shared diagnostics)
  ctx->disableMultithreading();
  auto run = [&](llvm::function_ref<void(mlir::OpPassManager &)> add) {
    mlir::PassManager pm(ctx);
    add(pm.nest<ProgramOp>());
    return pm.run(module);
  };
  // instrument: tracking markers and boxes first, as the lowering does (the
  // loads of a box's body become ordinary loads at the box's place), then
  // the checks (static discharge included)
  if (mlir::failed(run([&](mlir::OpPassManager &prog) {
        prog.addPass(createApplyTrackingPass());
        prog.addPass(createLowerBoxAllPass(o.boxAllExpand));
        prog.addPass(createCheckInbPass(o.staticDischarge, o.memhavocNullNoop));
      })))
    return 1;
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
  // optimize, lower once (placement, fat pointers), optimize again
  auto countAsserts = [&]() {
    uint64_t n = 0;
    program.walk([&](AssertOp a) {
      if (!mlir::matchPattern(a.getCond(), mlir::m_One()))
        ++n;
    });
    return n;
  };
  if (mlir::failed(run([&](mlir::OpPassManager &prog) {
        addVCOptimizeChecks(prog, o.lower);
        prog.addPass(createScalarizeStructsPass());
        addVCLowerMemory(prog, o.lower);
      })))
    return 1;
  // the largest placed object (A-NOWRAP): the sizes of the segments right
  // after placement (the folds after it erase unused ones), every part's
  // objects among them (null aside: size 0)
  llvm::APInt largestObject(128, 0);
  program.walk([&](SegmentOp seg) {
    if (seg.getKind() == "null")
      return;
    llvm::APInt sz = seg.getSize().zextOrTrunc(128);
    if (sz.ugt(largestObject))
      largestObject = sz;
  });
  if (mlir::failed(run([&](mlir::OpPassManager &prog) {
        addVCOptimizeChecks(prog, o.lower);
      })))
    return 1;
  const uint64_t beforeSplit = countAsserts();
  const uint64_t folded = left > beforeSplit ? left - beforeSplit : 0;
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
  // each part's check, before its own optimize merges it
  struct PartCheck {
    int64_t id = -1;
    std::string at;
  };
  std::vector<PartCheck> partChecks;
  for (ProgramOp part : parts) {
    AssertOp a;
    part.walk([&](AssertOp x) { a = x; });
    if (!a) {
      (void)postconditionFailedNoAssert(part);
      return 1;
    }
    PartCheck pc;
    pc.at = locText(a.getLoc());
    if (auto id = a->getAttrOfType<mlir::IntegerAttr>(kInbCheckAttr))
      pc.id = id.getInt();
    partChecks.push_back(pc);
  }
  // the program itself, if no part was made of it (no assert left), is
  // done: only the parts go on
  for (ProgramOp p : llvm::make_early_inc_range(module.getOps<ProgramOp>()))
    if (!p->hasAttr(kSplitAssertAttr))
      p.erase();
  // optimize each part with the rest of --lower: the fold of facts (the
  // earlier checks are assumes now), merge (one assert), the tail
  if (mlir::failed(run([&](mlir::OpPassManager &prog) {
        addVCAssumeFold(prog, o.lower);
        prog.addPass(createMergeAssertsPass());
        addVCLowerTail(prog, o.lower);
      })))
    return 1;
  // a part whose assert folded to true needs no solver: unsat
  std::vector<bool> trivial;
  uint64_t trivialN = 0;
  for (ProgramOp part : parts) {
    bool t = true;
    part.walk([&](AssertOp a) {
      t = t && mlir::matchPattern(a.getCond(), mlir::m_One());
    });
    trivial.push_back(t);
    trivialN += t;
  }
  {
    out::Record rec("check-inb.split");
    rec.num("folded", folded)
        .num("parts", parts.size())
        .num("trivial", trivialN)
        .num("to_solve", parts.size() - trivialN);
    rec.text =
        llvm::formatv("check-inb.split: folded={0} parts={1} "
                      "trivial={2} to-solve={3}",
                      folded, parts.size(), trivialN, parts.size() - trivialN)
            .str();
    out.emit(rec);
  }

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
  {
    // absolute: the manifest's argv runs from any directory
    llvm::SmallString<256> abs(dir);
    (void)llvm::sys::fs::make_absolute(abs);
    dir = abs.str().str();
  }
  // a temporary directory goes on every return from here, unless kept
  // (--check-inb-keep, --vcgen-only)
  const bool keepDir = !tempDir || o.keep || o.prepareOnly;
  auto removeTemp = llvm::scope_exit([&] {
    if (!keepDir)
      (void)llvm::sys::fs::remove_directories(dir);
  });
  if (keepDir) {
    out::Record rec("check-inb-dir");
    rec.str("path", dir);
    rec.text = "check-inb-dir: " + dir;
    out.emit(rec);
  }

  // step 1: every part, the parts' options (parts.cfg) and the manifest,
  // before any part runs. The directory is self-contained: every path in
  // it is relative to it, and a part runs with it as the working
  // directory (docs/design/CheckInb.md, "The manifest").
  const std::string partsCfg = "parts.cfg";
  if (mlir::failed(writeFileAtomic(
          dir + "/" + partsCfg,
          [&](llvm::raw_ostream &os) {
            os << "# sea-vc --check-inb: the options of every part (the "
                  "job's command line,\n# expanded and filtered; the "
                  "parts are lowered already)\n";
            auto line = [&](llvm::StringRef a) {
              // config-file syntax: a token with blanks, quotes, `#` or a
              // backslash is quoted
              if (a.find_first_of(" \t\"'#\\") == llvm::StringRef::npos) {
                os << a << "\n";
                return;
              }
              os << '"';
              for (char ch : a) {
                if (ch == '"' || ch == '\\')
                  os << '\\';
                os << ch;
              }
              os << "\"\n";
            };
            for (const std::string &a : o.partArgs)
              line(a);
            // the part is lowered: the pointer mode of the input, not of
            // the lowered program (sea-split-wide-access makes
            // addr.from_int)
            line("--ptr-mode=" + o.ptrMode);
            return mlir::success();
          },
          "vc.check-inb-dir", "--check-inb parts.cfg")))
    return 1;
  std::vector<PartSpec> specs;
  for (auto [i, part] : llvm::enumerate(parts)) {
    if (trivial[i])
      continue;
    PartSpec sp;
    sp.index = i;
    sp.at = partChecks[i].at;
    sp.checkId = partChecks[i].id;
    std::string base = ("part-" + llvm::Twine(i)).str();
    sp.mlir = base + ".mlir";
    sp.stdoutFile = base + ".jsonl";
    sp.stderrFile = base + ".err";
    std::string mlirPath = dir + "/" + sp.mlir;
    {
      // the part alone, with its source locations
      mlir::OpBuilder b(ctx);
      mlir::OwningOpRef<mlir::ModuleOp> m =
          mlir::ModuleOp::create(b.getUnknownLoc());
      m->getBody()->push_back(part->clone());
      if (mlir::failed(writeFileAtomic(
              mlirPath,
              [&](llvm::raw_ostream &os) {
                m->print(os, mlir::OpPrintingFlags().enableDebugInfo());
                return mlir::success();
              },
              "vc.check-inb-dir", "--check-inb part")))
        return 1;
    }
    // argv[0] names the tool, a runner substitutes its binary; paths are
    // relative to the job directory, the working directory of a part
    sp.argv = {"sea-vc", sp.mlir, "--config=" + partsCfg};
    sp.argv.push_back("--format=jsonl");
    // the last occurrence wins: a --config file or response file that
    // names --check-inb does not run the job again inside the part
    sp.argv.push_back("--check-inb=false");
    specs.push_back(std::move(sp));
  }
  {
    llvm::json::Array partsJ;
    for (const PartSpec &sp : specs) {
      llvm::json::Array argv;
      for (const std::string &s : sp.argv)
        argv.push_back(s);
      partsJ.push_back(llvm::json::Object{{"index", int64_t(sp.index)},
                                          {"check_id", sp.checkId},
                                          {"at", sp.at},
                                          {"mlir", sp.mlir},
                                          {"argv", std::move(argv)},
                                          {"stdout", sp.stdoutFile},
                                          {"stderr", sp.stderrFile}});
    }
    llvm::json::Array jobArgs;
    for (const std::string &s : o.jobArgs)
      jobArgs.push_back(s);
    std::string hash = sha256Of(o.source);
    llvm::SmallString<256> sourcePath(o.source);
    if (o.source != "-")
      (void)llvm::sys::fs::make_absolute(sourcePath);
    llvm::json::Object manifest{
        {"format", "sea-vc-check-inb"},
        {"version", int64_t(kManifestVersion)},
        {"sea_vc_version", versionString()},
        // a hint: the binary that wrote the job (argv[0] is `sea-vc`)
        {"sea_vc", o.self},
        // how to run a part: its argv, in the manifest's directory
        {"cwd", "."},
        {"config", partsCfg},
        {"source",
         llvm::json::Object{{"path", sourcePath.str()},
                            {"sha256", hash.empty()
                                           ? llvm::json::Value(nullptr)
                                           : llvm::json::Value(hash)}}},
        {"program", name},
        {"args", std::move(jobArgs)},
        {"counters", llvm::json::Object{{"checks", int64_t(checks)},
                                        {"static", int64_t(staticN)},
                                        {"left", int64_t(left)},
                                        {"zero_length", int64_t(zero)},
                                        {"folded", int64_t(folded)},
                                        {"parts", int64_t(parts.size())},
                                        {"trivial", int64_t(trivialN)}}},
        {"parts", std::move(partsJ)}};
    if (mlir::failed(writeFileAtomic(
            dir + "/manifest.json",
            [&](llvm::raw_ostream &os) {
              os << llvm::formatv("{0:2}",
                                  llvm::json::Value(std::move(manifest)))
                 << "\n";
              return mlir::success();
            },
            "vc.check-inb-dir", "--check-inb manifest")))
      return 1;
  }
  if (o.prepareOnly) {
    out::Record pr("check-inb.manifest");
    pr.str("path", dir + "/manifest.json").num("parts", specs.size());
    pr.text = llvm::formatv("check-inb.manifest: {0}/manifest.json parts={1}",
                            dir, specs.size())
                  .str();
    out.emit(pr);
    out::Record rec("result");
    rec.str("value", "none");
    rec.text = "result: none";
    out.emit(rec);
    out.flush();
    return 0;
  }
  std::vector<PartRun> runs;
  int worst = 0;
  unsigned skipped = 0;
  auto t0 = Clock::now();
  // step 2: run the parts, from the data the manifest was written from
  for (const PartSpec &sp : specs) {
    const unsigned i = sp.index;
    PartRun r;
    r.index = i;
    r.at = sp.at;
    r.checkId = sp.checkId;
    if (o.maxSeconds && secondsSince(t0) > o.maxSeconds) {
      ++skipped;
      continue;
    }
    std::string jsonFile = dir + "/" + sp.stdoutFile,
                errFile = dir + "/" + sp.stderrFile;
    // exactly the manifest's argv, argv[0] this binary, in the job
    // directory
    std::vector<llvm::StringRef> args(sp.argv.begin(), sp.argv.end());
    args[0] = o.self;
    std::optional<llvm::StringRef> redirects[] = {
        std::nullopt, llvm::StringRef(jsonFile), llvm::StringRef(errFile)};
    auto tp = Clock::now();
    std::string err;
    llvm::SmallString<256> cwd;
    (void)llvm::sys::fs::current_path(cwd);
    if (std::error_code ec = llvm::sys::fs::set_current_path(dir)) {
      diag::error("vc.check-inb-dir",
                  "cannot enter " + dir + ": " + ec.message());
      return 1;
    }
    r.status = llvm::sys::ExecuteAndWait(o.self, args, std::nullopt, redirects,
                                         0, 0, &err);
    (void)llvm::sys::fs::set_current_path(cwd);
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
  double maxT = 0, solveSum = 0;
  for (const PartRun &r : runs) {
    times.push_back(r.seconds);
    maxT = std::max(maxT, r.seconds);
    if (r.solveSeconds > 0)
      solveSum += r.solveSeconds;
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
    rec.num("total", specs.size())
        .num("unsat", unsat)
        .num("sat", sat)
        .num("unknown", unknown)
        .num("other", other)
        .num("skipped", skipped)
        .str("median_time", llvm::formatv("{0:f3}", median).str())
        .str("max_time", llvm::formatv("{0:f3}", maxT).str())
        .str("solve_time", llvm::formatv("{0:f3}", solveSum).str())
        .str("time", llvm::formatv("{0:f3}", secondsSince(t0)).str());
    rec.text =
        llvm::formatv("check-inb.parts: total={0} unsat={1} sat={2} "
                      "unknown={3} other={4} skipped={5} median-time={6:f3} "
                      "max-time={7:f3} solve-time={8:f3} time={9:f3}",
                      specs.size(), unsat, sat, unknown, other, skipped, median,
                      maxT, solveSum, secondsSince(t0))
            .str();
    out.emit(rec);
  }
  // the verdict: sat if a part's failure replayed; unsat iff every part
  // is unsat (a trivial part is); else unknown
  std::string verdict = sat                                 ? "sat"
                        : unsat == specs.size() && !skipped ? "unsat"
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
    // the largest placed object decides.
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
    // the lowering is the job's: placement made null address 0, and its
    // explicit assumptions are the job's own warnings (`*-assume`)
    if (o.lower.abstract)
      under.insert("addr0-is-null");
    for (const std::string &k : diag::warningKinds())
      if (llvm::StringRef(k).ends_with("-assume"))
        under.insert(k);
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
