// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Interpreter.cpp - concrete interpreter for sea programs --*- C++ -*-===//

#include "sea/Interp/Interpreter.h"
#include "sea/Dialect/Sea/Semantics.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <memory>

using namespace mlir;
using llvm::APInt;

namespace sea {

NondetSource::~NondetSource() = default;

std::optional<APInt> NondetSource::peekCell(Operation *, const APInt &,
                                            StringRef, unsigned,
                                            std::string &) {
  return std::nullopt;
}

std::optional<APInt> InputSource::peekCell(Operation *siteOp, const APInt &addr,
                                           StringRef object, unsigned cellWidth,
                                           std::string &origin) {
  std::string key = llvm::toString(addr, 10, /*Signed=*/false);
  if (!object.empty())
    key = (object + "+" + key).str();
  auto it = cells.find({memSiteOf(siteOp), key});
  if (it != cells.end()) {
    FailureOr<APInt> v = parseBitVector(it->second, cellWidth);
    if (failed(v))
      return std::nullopt;
    origin = "input";
    return *v;
  }
  // a random value would change the draws of the run: none
  if (missing == Missing::Zero) {
    origin = "zero";
    return APInt::getZero(cellWidth);
  }
  return std::nullopt;
}

FailureOr<APInt> parseBitVector(StringRef text, unsigned width) {
  bool negative = text.consume_front("-");
  unsigned radix = 10;
  if (text.consume_front("0x") || text.consume_front("0X"))
    radix = 16;
  if (text.empty())
    return failure();
  APInt mag;
  if (text.getAsInteger(radix, mag))
    return failure();
  // getAsInteger sizes `mag` to its digits; drop leading zero bits.
  unsigned active = std::max(1u, mag.getActiveBits());
  mag = mag.zextOrTrunc(active);
  if (!negative) {
    if (active > width)
      return failure();
    return mag.zext(width);
  }
  // -m fits in w bits iff m <= 2^(w-1).
  APInt m = mag.zext(std::max(active, width) + 1);
  APInt limit = APInt::getOneBitSet(m.getBitWidth(), width - 1);
  if (m.ugt(limit))
    return failure();
  return (-m).trunc(width);
}

FailureOr<APInt> InputSource::missingValue(unsigned width,
                                           const std::string &what,
                                           std::string &origin,
                                           std::string &error) {
  switch (missing) {
  case Missing::Error:
    error = "no input for " + what;
    return failure();
  case Missing::Zero:
    origin = "zero";
    return APInt::getZero(width);
  case Missing::Random: {
    SmallVector<uint64_t> words((width + 63) / 64);
    for (uint64_t &w : words)
      w = rng();
    origin = "random";
    return APInt(width, words);
  }
  }
  llvm_unreachable("unknown missing-input policy");
}

FailureOr<APInt> NondetSource::get(StructNondetOp op, std::string &origin,
                                   std::string &error) {
  error = "this nondet source has no values for sea.struct.nondet (site " +
          std::to_string(op.getSite()) + ")";
  return failure();
}

FailureOr<APInt> InputSource::bySite(uint64_t site, unsigned width,
                                     std::string &origin, std::string &error) {
  auto it = inputs.find(site);
  if (it != inputs.end()) {
    FailureOr<APInt> v = parseBitVector(it->second, width);
    if (failed(v)) {
      error = "input '" + it->second + "' for site " + std::to_string(site) +
              " is not an i" + std::to_string(width) + " value";
      return failure();
    }
    origin = "input";
    return v;
  }
  return missingValue(width, "nondet site " + std::to_string(site), origin,
                      error);
}

FailureOr<APInt> InputSource::get(NondetOp op, std::string &origin,
                                  std::string &error) {
  return bySite(op.getSite(), op.getType().getIntOrFloatBitWidth(), origin,
                error);
}

FailureOr<APInt> InputSource::get(StructNondetOp op, std::string &origin,
                                  std::string &error) {
  return bySite(op.getSite(), op.getBitsWidth(), origin, error);
}

uint64_t memSiteOf(Operation *op) {
  if (auto mn = dyn_cast<MemNondetOp>(op))
    return mn.getSite();
  return cast<MemHavocOp>(op).getSite();
}

FailureOr<APInt> InputSource::getCell(Operation *siteOp, const APInt &addr,
                                      StringRef object, unsigned cellWidth,
                                      std::string &origin, std::string &error) {
  uint64_t site = memSiteOf(siteOp);
  // Cell inputs are keyed by the address as unsigned decimal, or by
  // <object>+<offset> for abstract memories.
  std::string key = llvm::toString(addr, 10, /*Signed=*/false);
  if (!object.empty())
    key = (object + "+" + key).str();
  std::string what = "cell " + key + " of memory site " + std::to_string(site);
  auto it = cells.find({site, key});
  if (it != cells.end()) {
    FailureOr<APInt> v = parseBitVector(it->second, cellWidth);
    if (failed(v)) {
      error = "input '" + it->second + "' for " + what + " is not an i" +
              std::to_string(cellWidth) + " value";
      return failure();
    }
    origin = "input";
    return v;
  }
  return missingValue(cellWidth, what, origin, error);
}

// The prefix of the messages of the access checks of an abstract program
// (null, out of bounds, freed).
static constexpr const char *kMemorySafety = "memory access ";

bool isMemorySafetyKind(RunErrorKind kind) {
  switch (kind) {
  case RunErrorKind::NullAccess:
  case RunErrorKind::OutOfBounds:
  case RunErrorKind::UseAfterFree:
  case RunErrorKind::DoubleFree:
  case RunErrorKind::BadFree:
    return true;
  case RunErrorKind::None:
  case RunErrorKind::Layout:
  case RunErrorKind::Limit:
    return false;
  }
  llvm_unreachable("unknown run error kind");
}

StringRef stringifyRunErrorKind(RunErrorKind kind) {
  switch (kind) {
  case RunErrorKind::None:
    return "none";
  case RunErrorKind::NullAccess:
    return "null";
  case RunErrorKind::OutOfBounds:
    return "out-of-bounds";
  case RunErrorKind::UseAfterFree:
    return "use-after-free";
  case RunErrorKind::DoubleFree:
    return "double-free";
  case RunErrorKind::BadFree:
    return "bad-free";
  case RunErrorKind::Layout:
    return "layout";
  case RunErrorKind::Limit:
    return "limit";
  }
  llvm_unreachable("unknown run error kind");
}

bool isMemorySafetyError(const RunResult &result) {
  return (result.status == RunStatus::Error ||
          result.status == RunStatus::MemoryError) &&
         isMemorySafetyKind(result.errorKind);
}

bool isOutOfObjectAccess(const RunResult &result) {
  return result.status == RunStatus::Error &&
         (result.errorKind == RunErrorKind::NullAccess ||
          result.errorKind == RunErrorKind::OutOfBounds ||
          result.errorKind == RunErrorKind::UseAfterFree);
}

StringRef stringifyRunStatus(RunStatus status) {
  switch (status) {
  case RunStatus::Halted:
    return "halted";
  case RunStatus::AssertFailed:
    return "assert-failed";
  case RunStatus::AssumeBlocked:
    return "assume-blocked";
  case RunStatus::StepLimit:
    return "step-limit";
  case RunStatus::MemoryError:
    return "memory-error";
  case RunStatus::Error:
    return "error";
  }
  llvm_unreachable("unknown run status");
}

std::optional<FileLineColLoc> fileLocOf(Location loc) {
  if (auto f = dyn_cast<FileLineColLoc>(loc))
    return f;
  SourcePositions p = positionsOf(loc);
  if (p.empty())
    return std::nullopt;
  return p.front();
}

std::optional<std::string> locNameOf(Location loc) {
  auto of = [](Location l) -> std::optional<std::string> {
    if (auto n = dyn_cast<NameLoc>(l)) {
      if (n.getName() != kMergedAssertLocName)
        return n.getName().str();
      return locNameOf(n.getChildLoc());
    }
    return std::nullopt;
  };
  if (auto r = of(loc))
    return r;
  if (auto f = dyn_cast<FusedLoc>(loc))
    for (Location l : f.getLocations())
      if (auto r = of(l))
        return r;
  return std::nullopt;
}

/// Appends the file positions of `loc` to `out` (positionsOf).
static void collectPositions(Location loc, SourcePositions &out) {
  if (auto f = dyn_cast<FileLineColLoc>(loc)) {
    out.push_back(f);
  } else if (auto n = dyn_cast<NameLoc>(loc)) {
    collectPositions(n.getChildLoc(), out);
  } else if (auto c = dyn_cast<CallSiteLoc>(loc)) {
    collectPositions(c.getCallee(), out);
    collectPositions(c.getCaller(), out);
  } else if (auto fl = dyn_cast<FusedLoc>(loc)) {
    // the first element with a position (fileLocOf's)
    for (Location l : fl.getLocations()) {
      size_t before = out.size();
      collectPositions(l, out);
      if (out.size() != before)
        return;
    }
  } else if (auto o = dyn_cast<OpaqueLoc>(loc)) {
    collectPositions(o.getFallbackLocation(), out);
  }
}

SourcePositions positionsOf(Location loc) {
  SourcePositions out;
  collectPositions(loc, out);
  return out;
}

SmallVector<FileLineColLoc> inlinedAtOf(Location loc) {
  SourcePositions p = positionsOf(loc);
  if (p.empty())
    return {};
  return SmallVector<FileLineColLoc>(p.begin() + 1, p.end());
}

std::string fileLocText(FileLineColLoc loc) {
  std::string t;
  llvm::raw_string_ostream os(t);
  printFileLoc(os, loc);
  return t;
}

void printInlinedAt(llvm::raw_ostream &os, ArrayRef<FileLineColLoc> callSites) {
  for (auto [i, c] : llvm::enumerate(callSites)) {
    os << (i ? "," : " inlined-at=");
    printFileLoc(os, c);
  }
}

void printPositions(llvm::raw_ostream &os, ArrayRef<FileLineColLoc> positions) {
  printFileLoc(os, positions.front());
  printInlinedAt(os, positions.drop_front());
}

std::string jsonCallSites(ArrayRef<FileLineColLoc> callSites) {
  std::vector<std::string> xs;
  for (FileLineColLoc c : callSites)
    xs.push_back(fileLocText(c));
  return out::jstrs(xs);
}

std::string formatLoc(Location loc, StringRef nameKey) {
  SourcePositions p = positionsOf(loc);
  std::optional<std::string> name = locNameOf(loc);
  if (p.empty() && !name)
    return "";
  std::string t;
  llvm::raw_string_ostream os(t);
  if (!p.empty())
    printPositions(os, p);
  else
    os << "-";
  if (name)
    os << " " << nameKey << "=" << *name;
  return t;
}

void addLocFields(out::Record &rec, StringRef key, Location loc) {
  SourcePositions p = positionsOf(loc);
  if (!p.empty())
    rec.str(key, fileLocText(p.front()));
  if (p.size() > 1)
    rec.set((key + "_inlined").str(), jsonCallSites(ArrayRef(p).drop_front()));
  if (std::optional<std::string> name = locNameOf(loc))
    rec.str((key + "_name").str(), *name);
}

void addPathFields(out::Record &rec, ArrayRef<SourcePositions> branchPath) {
  if (branchPath.empty())
    return;
  std::vector<std::string> path;
  std::string inlined = "[";
  bool anyInlined = false;
  for (auto [i, p] : llvm::enumerate(branchPath)) {
    path.push_back(fileLocText(p.front()));
    inlined += (i ? "," : "") + jsonCallSites(ArrayRef(p).drop_front());
    anyInlined |= p.size() > 1;
  }
  rec.set("path", out::jstrs(path));
  if (anyInlined)
    rec.set("path_inlined", inlined + "]");
}

std::optional<Location> stopLocOf(const RunResult &result) {
  if (result.status == RunStatus::AssertFailed && result.assertLoc)
    return result.assertLoc;
  if (result.at)
    return result.at->getLoc();
  return std::nullopt;
}

std::optional<Location> reachedLocOf(const RunResult &result) {
  if (result.status != RunStatus::MemoryError || !result.reachedAt)
    return std::nullopt;
  if (result.reachedStatus == RunStatus::AssertFailed && result.assertLoc)
    return result.assertLoc;
  return result.reachedAt->getLoc();
}

void printFileLoc(llvm::raw_ostream &os, FileLineColLoc loc) {
  os << loc.getFilename().getValue() << ":" << loc.getLine() << ":"
     << loc.getColumn();
}

void printAssertContext(llvm::raw_ostream &os, StringRef prefix,
                        const RunResult &result) {
  if (std::string l = result.assertLoc ? formatLoc(*result.assertLoc) : "";
      !l.empty())
    os << prefix << "assert-op: " << l << "\n";
  if (result.branchPath.empty())
    return;
  os << prefix << "assert-path:";
  for (auto [i, p] : llvm::enumerate(result.branchPath)) {
    os << (i ? " <- " : " ");
    printPositions(os, p);
  }
  os << "\n";
}

namespace {
/// An abstract pointer: object 0 is null.
struct Ptr {
  unsigned obj = 0;
  APInt off;
  bool operator==(const Ptr &o) const { return obj == o.obj && off == o.off; }
};

/// One leaf of a register value: bits, or a pointer (abstract level).
struct Leaf {
  APInt bits;
  std::optional<Ptr> ptr;
};

/// An object: allocated (abstract level; name as in the segment map:
/// stack.<site>, global.<name>, ...) or placed (a sea.segment).
struct Object : ObjectInfo {
  bool heap = false;
};

/// Provenance of placed address bits (docs/design/Print.md): the bits are
/// bits [bit, bit + width) of an address derived from object `obj` (a whole
/// address: bit 0 and the address width).
struct Prov {
  unsigned obj;
  unsigned bit = 0;
};

/// A cell's content: bits, or (abstract level) piece `piece` of a pointer
/// that spans several cells; `prov`: placed address bits.
struct CellValue {
  APInt bits;
  std::optional<Ptr> ptr;
  unsigned piece = 0;
  std::optional<Prov> prov = std::nullopt;
};

/// The storage of one chain of memory versions, updated in place.
///
/// Cells written by the run (or read once from a source and cached) live in
/// `cells`, each stamped with the sequence number of the update that wrote
/// it. A memhavoc adds a layer instead of enumerating its range. A read takes
/// the newest of: the written cell, the havoc layers covering the address,
/// and the base memory. Concrete memories are keyed by the address; abstract
/// ones by (object << W) | offset.
struct Buffer {
  struct Cell {
    CellValue value;
    uint64_t seq;
    bool dflt = false; // read from a source that left it unconstrained
  };
  struct Layer {
    Operation *siteOp; // the sea.memhavoc
    APInt start, len;  // keys
    uint64_t seq;
  };

  Value current;             // the version that may be consumed next
  Operation *baseOp;         // the sea.mem.nondet (or sea.mem.const)
  std::optional<APInt> fill; // the value of every base cell (sea.mem.const)
  unsigned cellWidth;
  unsigned units = 1; // index units per cell (k for k-byte words)
  MemLayout layout = MemLayout::exact;
  bool abstract = false;
  uint64_t seq = 0;
  llvm::DenseMap<APInt, Cell> cells;
  SmallVector<Layer> layers;
  /// Objects written by `tracked` updates (modification tracking).
  llvm::DenseSet<unsigned> modified;
};

class Interpreter {
public:
  Interpreter(ProgramOp program, NondetSource &nondets,
              const RunOptions &options)
      : program(program), nondets(nondets), options(options) {
    Object null;
    null.name = "null";
    null.kind = "null";
    null.size = APInt(1, 0);
    objects.push_back(null); // object 0
    preparePrints();
  }

  RunResult run();

private:
  RunResult &finish(RunStatus status, Operation *at, std::string message = {}) {
    result.status = status;
    result.at = at;
    result.message = std::move(message);
    result.objects.assign(objects.begin(), objects.end());
    // where in the source: the definition of the failing condition, else
    // the last located op before `at` (source locations first, see
    // isSourceLoc); and the recent branches
    result.failLoc = lastSourceLoc  ? lastSourceLoc
                     : lastNamedLoc ? lastNamedLoc
                                    : lastOpLoc;
    if (status == RunStatus::AssertFailed) {
      result.assertLoc = mergedAssertLoc ? *mergedAssertLoc : at->getLoc();
      result.assertOp = mergedAssertLoc ? mergedAssertOp : at;
      Value cond;
      if (mergedAssertLoc)
        cond = mergedCond;
      else if (auto a = dyn_cast<AssertOp>(at))
        cond = a.getCond();
      if (Operation *def = failingConditionDef(cond))
        result.failLoc = def->getLoc();
    }
    // a program with source locations: the path lists only those (a
    // branch without one was resolved, or left out, by noteBranch)
    bool anySource = lastSourceLoc.has_value() ||
                     llvm::any_of(branchLocs, [](const SourcePositions &p) {
                       return isSourceLoc(p.front());
                     });
    for (const SourcePositions &f : llvm::reverse(branchLocs)) {
      if (result.branchPath.size() == RunResult::kBranchPathLength)
        break;
      if (anySource && !isSourceLoc(f.front()))
        continue;
      if (!llvm::is_contained(result.branchPath, f))
        result.branchPath.push_back(f);
    }
    if (status == RunStatus::Error)
      result.errorKind =
          errKind == RunErrorKind::None ? RunErrorKind::Limit : errKind;
    // Deferred violations (RunOptions::deferMemoryErrors): a run that
    // reaches the end or a failing assert past one is a finding, the first
    // violation. It is reported instead of the assert failure: the assert's
    // condition may depend on the substitute values, and the violation came
    // first (fix it, and the rerun shows the assert).
    if (result.deferredCount &&
        (status == RunStatus::Halted || status == RunStatus::AssertFailed)) {
      result.reachedAt = at;
      result.reachedStatus = status;
      result.status = RunStatus::MemoryError;
      result.at = result.deferredAt;
      result.message = result.deferredMessage;
      result.errorKind = result.deferredKind;
    }
    return result;
  }

  /// RunOptions::deferMemoryErrors and a memory-safety error (errKind, the
  /// message in `error`) at `op`: records it (the first one in full) and
  /// returns true; the caller goes on with the op's substitute effect.
  /// Otherwise returns false (the run stops).
  ///
  /// The assumption the classification rests on: past the first violation
  /// any behaviour is allowed (the abstract model leaves it open; its
  /// formal semantics is not settled). The substitutes pick one
  /// continuation, and only decide whether a later assume blocks the run.
  /// A finding means: the violation is reached on an exact prefix of the
  /// model, and some continuation reaches the end. Under that assumption a
  /// blocked run can hide a finding (another continuation might pass the
  /// assume), and a finding is never made up. A finding need not be a
  /// counterexample of a VC with array semantics past the violation: an
  /// out-of-bounds store is dropped here, while sea-vc's arrays read it
  /// back (store 7 out of bounds, load it, assume it is 0: a finding here,
  /// infeasible there). Fixed substitutes (0, null, no effect) keep runs
  /// deterministic, so a finding replays from its input.
  bool deferViolation(Operation &op, std::string &error) {
    if (!options.deferMemoryErrors || !isMemorySafetyKind(errKind))
      return false;
    if (!result.deferredCount++) {
      result.deferredAt = &op;
      result.deferredKind = errKind;
      result.deferredMessage = error;
    }
    errKind = RunErrorKind::None;
    error.clear();
    return true;
  }

  /// Fails (errKind, `error`) unless the range of `len` index units at
  /// `idx` lies in a live object (abstract pointers; nothing to check for
  /// an address or an empty range). The check of the cell keys (cells of
  /// `units` index units), for a whole update at once (deferMemoryErrors:
  /// an update with a violation does nothing). The message is key()'s,
  /// at the first cell out of bounds, as the per-cell loops of store,
  /// memset and memmove report it (memhavoc without the flag reports its
  /// last byte).
  LogicalResult checkRange(Value idx, const APInt &len, uint64_t units,
                           std::string &error);

  /// The op that computed the failing condition `cond` (false on the run):
  /// through a chain of sea.and, the first false conjunct's definition.
  /// None for a constant (an assert of false reached by a branch: the
  /// branch says where), a block argument, or a definition whose location
  /// has neither a file position nor a name.
  Operation *failingConditionDef(Value cond) {
    if (!cond)
      return nullptr;
    while (auto a = cond.getDefiningOp<AndOp>()) {
      Value next;
      for (Value v : {a.getLhs(), a.getRhs()})
        if (auto it = env.find(v); it != env.end() && it->second.isZero()) {
          next = v;
          break;
        }
      if (!next)
        break;
      cond = next;
    }
    Operation *def = cond.getDefiningOp();
    if (!def || isa<ConstOp>(def) ||
        (!fileLocOf(def->getLoc()) && !locNameOf(def->getLoc())))
      return nullptr;
    return def;
  }

  /// A location in the program's source, not one the MLIR parser gave an
  /// op without an explicit location (a position in the .mlir file itself).
  static bool isSourceLoc(FileLineColLoc f) {
    return !f.getFilename().getValue().ends_with(".mlir");
  }

  /// Records the location of an executed op (before the next one runs).
  /// The branches and constants sea-merge-asserts made of an assert are the
  /// assert, not the place it was reached from: skipped.
  void noteOp(Operation &op) {
    // print ops change nothing: never a location of the run
    if (isPrintOp(&op))
      return;
    if (auto n = dyn_cast<NameLoc>(op.getLoc());
        n && n.getName() == kMergedAssertLocName)
      return;
    std::optional<FileLineColLoc> f = fileLocOf(op.getLoc());
    if (!f || (mergedAssertFile && *f == *mergedAssertFile))
      return;
    lastOpLoc = op.getLoc();
    if (isSourceLoc(*f)) {
      lastSourceLoc = op.getLoc();
      lastSourceOp = &op;
    } else if (locNameOf(op.getLoc()))
      lastNamedLoc = op.getLoc();
  }

  /// Records an executed branch with a file location (consecutive repeats
  /// once; a bounded window, so a long loop does not grow it). A cond_br
  /// without a source location is recorded at decisionSourceOf's, if any.
  void noteBranch(Operation &op) {
    std::optional<FileLineColLoc> f = fileLocOf(op.getLoc());
    // a branch sea-merge-asserts put in an assert's place: the assert, not
    // a branch of the source; it stays the failing assert's location until
    // the run takes another branch
    // (the marker wraps the original assert's whole location, which may
    // be a name without a file position). The landing block's sea.br
    // follows the cond_br that chose it: the cond_br names the assert (a
    // landing block may serve several asserts once identical blocks are
    // merged).
    if (auto n = dyn_cast<NameLoc>(op.getLoc());
        n && n.getName() == kMergedAssertLocName) {
      if (isa<BrOp>(op) && mergedAssertLoc)
        return;
      mergedAssertLoc = n.getChildLoc();
      mergedAssertOp = &op;
      mergedAssertFile = f;
      // the original assert's condition, when the cond_br takes its
      // landing (false) side
      mergedCond = {};
      if (auto cbr = dyn_cast<CondBrOp>(op);
          cbr && env.lookup(cbr.getCondition()).isZero())
        mergedCond = cbr.getCondition();
      return;
    }
    mergedAssertLoc.reset();
    mergedAssertOp = nullptr;
    mergedAssertFile.reset();
    mergedCond = {};
    if (!f)
      return;
    SourcePositions p = positionsOf(op.getLoc());
    if (!isSourceLoc(p.front()))
      if (std::optional<SourcePositions> s = decisionSourceOf(op))
        p = std::move(*s);
    if (!branchLocs.empty() && branchLocs.back() == p)
      return;
    if (branchLocs.size() >= 256)
      branchLocs.erase(branchLocs.begin(), branchLocs.begin() + 128);
    branchLocs.push_back(std::move(p));
  }

  /// The source positions of a sea.cond_br without one of its own (a
  /// branch the exporter left unlocated, shown at a position in the .mlir
  /// file): its condition's definition, else the last op executed before
  /// it in its block, if either has a source location. None for an
  /// unconditional branch (it decides nothing).
  std::optional<SourcePositions> decisionSourceOf(Operation &op) {
    auto cbr = dyn_cast<CondBrOp>(op);
    if (!cbr)
      return std::nullopt;
    auto source = [](Operation *o) -> std::optional<SourcePositions> {
      SourcePositions p = positionsOf(o->getLoc());
      if (p.empty() || !isSourceLoc(p.front()))
        return std::nullopt;
      return p;
    };
    if (Operation *def = cbr.getCondition().getDefiningOp())
      if (auto p = source(def))
        return p;
    // lastSourceOp ran in this visit of the block: a block's ops run in
    // order, and noteOp has seen every op before the branch
    if (lastSourceOp && lastSourceOp->getBlock() == op.getBlock())
      return source(lastSourceOp);
    return std::nullopt;
  }

  SmallVector<APInt> valuesOf(ValueRange values) {
    SmallVector<APInt> out;
    for (Value v : values)
      out.push_back(env.lookup(v));
    return out;
  }

  void define(Value v, APInt value) {
    if (trackProv)
      prov.erase(v);
    env[v] = value;
    if (options.onValue)
      options.onValue(v, value);
  }

  /// The leaves of a register value (a struct's, flattened in field order;
  /// one for a scalar).
  SmallVector<Leaf> leavesOf(Value v) {
    if (isa<StructType>(v.getType()))
      return structs.lookup(v);
    if (isa<PtrType>(v.getType()))
      return {{APInt(), ptrs.lookup(v)}};
    return {{env.lookup(v), std::nullopt}};
  }
  void defineLeaves(Value v, ArrayRef<Leaf> leaves) {
    if (isa<StructType>(v.getType()))
      structs[v] = SmallVector<Leaf>(leaves);
    else if (isa<PtrType>(v.getType()))
      ptrs[v] = *leaves.front().ptr;
    else
      define(v, leaves.front().bits);
  }
  /// Executes struct.create / extract / insert.
  void execStruct(Operation &op);

  /// The key of cell `k` of a range starting at index value `idx`, checked
  /// against its object for abstract memories.
  FailureOr<APInt> key(Value idx, uint64_t k, std::string &error,
                       uint64_t span = 1);
  /// The key of the cell `k` units from `idx` in `b`, checked against the
  /// layout: whole cell in its object; aligned: the address (abstract: the
  /// offset, and the object's alignment) a multiple of the cell; unaligned:
  /// no other cell overlapping it (the matching-access condition).
  FailureOr<APInt> cellKey(Buffer &b, Value idx, uint64_t k,
                           std::string &error);
  /// The number of cells of a range op of `len` units; fails unless it is
  /// whole cells (word layouts) and within the interpreter's limit.
  FailureOr<uint64_t> rangeCells(Buffer &b, const APInt &len, StringRef what,
                                 std::string &error);
  /// Warns (interp.align-promise) if the access `op` at `idx` breaks its
  /// `align` promise (bytes): the address is not a multiple of it, or (before
  /// placement) its object is less aligned, so placement may break it.
  void checkAlign(Operation &op, Value idx, std::optional<uint64_t> align,
                  StringRef name = "align");
  /// The object name and offset of a key (object empty for concrete).
  std::pair<std::string, APInt> splitKey(const Buffer &b, const APInt &key);

  /// Reads the cell at `key`; fails with a message in `error`.
  FailureOr<CellValue> read(Buffer &b, const APInt &key, std::string &error);
  void write(Buffer &b, const APInt &key, CellValue value) {
    b.cells[key] = {std::move(value), ++b.seq};
  }
  /// Cell `i` of value `v` (an integer, address or pointer), little-endian.
  CellValue cellOf(Value v, unsigned i, unsigned cellWidth) {
    if (isa<PtrType>(v.getType()))
      return {APInt::getZero(cellWidth), ptrs.lookup(v), i};
    std::optional<Prov> p;
    if (trackProv)
      if (auto it = prov.find(v); it != prov.end())
        p = Prov{it->second.obj, it->second.bit + i * cellWidth};
    return {env.lookup(v).extractBits(cellWidth, i * cellWidth), std::nullopt,
            0, p};
  }

  /// Hands the buffer of version `from` over to version `to`. Fails unless
  /// `from` is the buffer's current version (each version is consumed at
  /// most once).
  FailureOr<Buffer *> consume(Value from, Value to, std::string &error);

  /// Executes a memory op; returns failure (with a message in `error`) if
  /// the run must stop.
  LogicalResult execMem(Operation &op, std::string &error);
  /// Executes an op of the abstract level (allocation, pointers).
  LogicalResult execPtr(Operation &op, std::string &error);

  /// Binds `dest`'s arguments to `operands` (a parallel copy).
  LogicalResult jump(Block *dest, ValueRange operands, std::string &error);

  unsigned newObject(std::string name, APInt size, bool heap, uint64_t align,
                     StringRef kind, Operation *at) {
    Object o;
    o.name = std::move(name);
    o.size = std::move(size);
    o.heap = heap;
    o.align = align;
    o.kind = kind.str();
    if (at) {
      o.loc = fileLocOf(at->getLoc());
      o.inlinedAt = inlinedAtOf(at->getLoc());
      o.locName = locNameOf(at->getLoc()).value_or("");
      if (auto l = at->getAttrOfType<StringAttr>("label"))
        o.label = l.str();
    }
    objects.push_back(std::move(o));
    return objects.size() - 1;
  }

  //===- Counterexample prints (docs/design/Print.md) ---------------------===//

  /// Sets up names and provenance tracking for the program's prints.
  void preparePrints();
  /// Called once an op's results are defined: binds their names, reports
  /// pointers (RunOptions::onPointer).
  void afterValue(Value v);
  /// Propagates provenance through a pure op that just defined its result.
  void propagateProv(Operation &op, ArrayRef<APInt> operands);
  /// Executes a print op (never fails).
  void execPrint(Operation &op);
  /// The token of a pointer / address value; adds the objects it refers to.
  /// `f`, if given, gets the structured fields of the token (jsonl).
  std::string pointerText(Value v, SmallVectorImpl<unsigned> &refs);
  std::string ptrText(const Ptr &p, SmallVectorImpl<unsigned> &refs,
                      out::Fields *f = nullptr);
  std::string addrText(const APInt &bits, std::optional<Prov> p,
                       SmallVectorImpl<unsigned> &refs,
                       out::Fields *f = nullptr);
  /// The rendering of a leaf of type `t`.
  std::string leafText(Type t, const Leaf &leaf, std::optional<Prov> p,
                       StringRef fmt, SmallVectorImpl<unsigned> &refs,
                       out::Fields *f = nullptr);
  /// One `<name> = <value>` line of a print: its parts and fields.
  struct ValueLine {
    std::string name, value;
    out::Fields fields;
  };
  /// Lines `<tag> = <value>` (one per leaf of a struct) for `v`.
  void valueLines(StringRef tag, Value v, PrintOp print,
                  SmallVectorImpl<ValueLine> &out,
                  SmallVectorImpl<unsigned> &refs);
  void execPrintMem(PrintMemOp op);
  /// A cell's value as a read would see it, without reading the source
  /// (peekCell): none if unknown.
  std::optional<CellValue> peekCell(Buffer &b, const APInt &key, bool &dflt);
  /// One byte of memory for a print: abstract (`obj`, `off`) or placed
  /// (`off` the address; obj ignored).
  struct PeekedByte {
    DumpByte byte;
    std::optional<Ptr> ptr; // abstract pointer piece: pointer byte ptrByte
    unsigned ptrByte = 0;
    std::optional<Prov> prov; // placed address bits: prov.bit is the bit
  };
  PeekedByte peekByte(Buffer &b, unsigned obj, const APInt &off);
  /// Emits a line (and the object records it needs first).
  /// `name`: the left side (PrintRecord::name); `text` the whole line.
  void emitLine(std::string name, std::string text, ArrayRef<unsigned> refs,
                Operation *op, out::Fields fields = {}, std::string jkind = {});
  void emit(PrintRecord::Kind kind, std::string text, Operation *op,
            std::string name = {}, out::Fields fields = {},
            std::string jkind = {});
  /// The most recent definition of `name` on the executed path.
  Value lookupName(StringRef name) { return named.lookup(name); }

  ProgramOp program;
  NondetSource &nondets;
  const RunOptions &options;
  RunResult result;
  /// Recent executed branches with a file location, oldest first.
  SmallVector<SourcePositions> branchLocs;
  /// The file location of the last executed op before the current one
  /// (noteOp), and the last such source location.
  std::optional<Location> lastOpLoc, lastSourceLoc;
  /// The op of lastSourceLoc.
  Operation *lastSourceOp = nullptr;
  /// The last such location with a name (NameLoc) but only a .mlir position.
  std::optional<Location> lastNamedLoc;
  Operation *prevOp = nullptr;
  /// The original assert of the last branch taken, if sea-merge-asserts
  /// made it (see noteBranch).
  std::optional<Location> mergedAssertLoc;
  /// The branch it came from.
  Operation *mergedAssertOp = nullptr;
  /// Its file location, if any.
  std::optional<FileLineColLoc> mergedAssertFile;
  /// Its condition, when the run took the branch to the merged assert's
  /// failing side.
  Value mergedCond;
  /// The kind of the error being reported (set where it is found).
  RunErrorKind errKind = RunErrorKind::None;
  llvm::DenseMap<Value, APInt> env;
  llvm::DenseMap<Value, Ptr> ptrs;
  llvm::DenseMap<Value, SmallVector<Leaf>> structs;
  llvm::DenseMap<Value, std::shared_ptr<Buffer>> mems;
  std::vector<Object> objects;
  llvm::StringMap<unsigned> namedObjects; // globals and functions
  llvm::DenseMap<Operation *, unsigned> allocCount;

  // prints
  bool trackProv = false; // a placed program: address provenance
  llvm::DenseMap<Value, Prov> prov;
  llvm::DenseMap<Operation *, unsigned> segmentObject;
  /// RunResult::earlierVersionLoads, as a set
  llvm::DenseSet<Operation *> earlierVersionSeen;
  llvm::DenseMap<Value, SmallVector<std::string, 2>> valueNames; // weak prints
  llvm::StringMap<Value> named; // name -> most recent definition
  SmallVector<std::string> scopes;
  llvm::DenseSet<unsigned> listedObjects;
  uint64_t printRecords = 0;
};
} // namespace

FailureOr<APInt> Interpreter::key(Value idx, uint64_t k, std::string &error,
                                  uint64_t span) {
  if (!isa<PtrType>(idx.getType()))
    return env.lookup(idx) + k;
  Ptr p = ptrs.lookup(idx);
  if (p.obj == 0) {
    errKind = RunErrorKind::NullAccess;
    error = std::string(kMemorySafety) + "through the null pointer";
    return failure();
  }
  const Object &o = objects[p.obj];
  if (!o.alive) {
    errKind = RunErrorKind::UseAfterFree;
    error = std::string(kMemorySafety) + "to " + o.name + " after it was freed";
    return failure();
  }
  APInt off = p.off + k;
  APInt last = off + (span - 1);
  if (off.uge(o.size.zextOrTrunc(off.getBitWidth())) ||
      last.uge(o.size.zextOrTrunc(off.getBitWidth())) || last.ult(off)) {
    errKind = RunErrorKind::OutOfBounds;
    error = std::string(kMemorySafety) + "out of bounds: offset " +
            llvm::toString(off, 10, false) + " in " + o.name + " of size " +
            llvm::toString(o.size, 10, false);
    return failure();
  }
  unsigned w = off.getBitWidth();
  return (APInt(w + 32, p.obj) << w) | off.zext(w + 32);
}

LogicalResult Interpreter::checkRange(Value idx, const APInt &len,
                                      uint64_t units, std::string &error) {
  if (!isa<PtrType>(idx.getType()) || len.isZero())
    return success();
  Ptr p = ptrs.lookup(idx);
  if (p.obj == 0) {
    errKind = RunErrorKind::NullAccess;
    error = std::string(kMemorySafety) + "through the null pointer";
    return failure();
  }
  const Object &o = objects[p.obj];
  if (!o.alive) {
    errKind = RunErrorKind::UseAfterFree;
    error = std::string(kMemorySafety) + "to " + o.name + " after it was freed";
    return failure();
  }
  // [off, off + len) within [0, size), without wrapping
  unsigned w =
      std::max({p.off.getBitWidth(), len.getBitWidth(), o.size.getBitWidth()}) +
      1;
  APInt off = p.off.zext(w), size = o.size.zext(w);
  if ((off + len.zext(w)).ugt(size)) {
    // the first cell not wholly inside: the start, or the cell that
    // crosses (or starts at) the end
    APInt bad = off;
    if (off.ult(size)) {
      APInt u(w, units);
      bad = off + (size - off).udiv(u) * u;
    }
    errKind = RunErrorKind::OutOfBounds;
    error = std::string(kMemorySafety) + "out of bounds: offset " +
            llvm::toString(bad.trunc(p.off.getBitWidth()), 10, false) + " in " +
            o.name + " of size " + llvm::toString(o.size, 10, false);
    return failure();
  }
  return success();
}

void Interpreter::checkAlign(Operation &op, Value idx,
                             std::optional<uint64_t> align, StringRef name) {
  if (!align || *align == 1)
    return;
  std::string broken;
  if (auto a = dyn_cast<AddrType>(idx.getType())) {
    APInt bits = env.lookup(idx).zext(a.getWidth() + 64) * a.getUnit();
    if (bits.urem(*align * 8) != 0)
      broken = "the address " + llvm::toString(env.lookup(idx), 10, false) +
               " is not a multiple of it";
  } else if (Ptr p = ptrs.lookup(idx); p.obj != 0) {
    const Object &o = objects[p.obj];
    if (o.align % *align != 0)
      broken = o.name + " is aligned to " + std::to_string(o.align) + " only";
    else if (p.off.urem(*align) != 0)
      broken = "offset " + llvm::toString(p.off, 10, false) + " in " + o.name +
               " is not a multiple of it";
  } // null: the access itself fails
  if (!broken.empty())
    diag::warn("interp.align-promise",
               op.getName().getStringRef() + " " + name + " " + Twine(*align) +
                   ": the promise is broken (" + broken + ")",
               op.getLoc());
}

FailureOr<APInt> Interpreter::cellKey(Buffer &b, Value idx, uint64_t k,
                                      std::string &error) {
  FailureOr<APInt> at = key(idx, k, error, b.units);
  // keyed: a cell per address, independent (no overlap to check)
  if (failed(at) || b.layout == MemLayout::exact ||
      b.layout == MemLayout::keyed)
    return at;
  auto [object, off] = splitKey(b, *at);
  if (b.layout == MemLayout::aligned) {
    if (off.urem(b.units) != 0) {
      errKind = RunErrorKind::Layout;
      error = "aligned " + std::to_string(b.units) +
              "-byte word layout: access at " +
              (object.empty() ? "" : object + "+") +
              llvm::toString(off, 10, false) + " is not word-aligned";
      return failure();
    }
    if (b.abstract) {
      const Object &o = objects[ptrs.lookup(idx).obj];
      if (o.align % b.units != 0) {
        errKind = RunErrorKind::Layout;
        error = "aligned " + std::to_string(b.units) +
                "-byte word layout: " + o.name + " is aligned to " +
                std::to_string(o.align) + " only";
        return failure();
      }
    }
    return at;
  }
  // unaligned: a cell overlapping this one, starting elsewhere, means two
  // accesses to these bytes do not match
  for (uint64_t d = 1; d < b.units; ++d)
    for (APInt other : {*at - d, *at + d})
      if (b.cells.count(other)) {
        auto [o2, off2] = splitKey(b, other);
        errKind = RunErrorKind::Layout;
        error = "unaligned word layout: the access at " +
                (object.empty() ? "" : object + "+") +
                llvm::toString(off, 10, false) + " overlaps the " +
                std::to_string(b.units) + "-byte cell at " +
                llvm::toString(off2, 10, false) + " (accesses do not match)";
        return failure();
      }
  return at;
}

FailureOr<uint64_t> Interpreter::rangeCells(Buffer &b, const APInt &len,
                                            StringRef what,
                                            std::string &error) {
  if (len.urem(b.units) != 0) {
    errKind = RunErrorKind::Layout;
    error = (what + " of " + llvm::toString(len, 10, false) +
             " bytes is not whole " + std::to_string(b.units) + "-byte cells")
                .str();
    return failure();
  }
  APInt n = len.udiv(b.units);
  if (n.ugt(options.maxMemOpCells)) {
    error = (what + " of " + llvm::toString(n, 10, false) +
             " cells exceeds the interpreter limit")
                .str();
    return failure();
  }
  return n.getZExtValue();
}

std::pair<std::string, APInt> Interpreter::splitKey(const Buffer &b,
                                                    const APInt &key) {
  if (!b.abstract)
    return {"", key};
  unsigned w = key.getBitWidth() - 32;
  unsigned obj = key.lshr(w).getZExtValue();
  return {objects[obj].name, key.trunc(w)};
}

FailureOr<CellValue> Interpreter::read(Buffer &b, const APInt &addr,
                                       std::string &error) {
  auto it = b.cells.find(addr);
  uint64_t cellSeq = it == b.cells.end() ? 0 : it->second.seq;
  // The newest havoc layer covering `addr`, if newer than the written cell.
  const Buffer::Layer *layer = nullptr;
  for (const Buffer::Layer &l : llvm::reverse(b.layers))
    if (l.seq > cellSeq && (addr - l.start).ult(l.len)) {
      layer = &l;
      break;
    }
  if (!layer && it != b.cells.end())
    return it->second.value;

  if (!layer && b.fill)
    return CellValue{*b.fill, std::nullopt};
  Operation *siteOp = layer ? layer->siteOp : b.baseOp;
  auto [object, at] = splitKey(b, addr);
  std::string origin;
  FailureOr<APInt> v =
      nondets.getCell(siteOp, at, object, b.cellWidth, origin, error);
  if (failed(v))
    return failure();
  result.cellReads.push_back({siteOp, object, at, *v, origin});
  // Cache with the stamp of what it came from, so newer writes still win.
  b.cells[addr] = {CellValue{*v, std::nullopt}, layer ? layer->seq : 0,
                   origin == "default"};
  return CellValue{*v, std::nullopt};
}

FailureOr<Buffer *> Interpreter::consume(Value from, Value to,
                                         std::string &error) {
  auto it = mems.find(from);
  assert(it != mems.end() && "memory version without a buffer");
  std::shared_ptr<Buffer> b = it->second;
  if (b->current != from) {
    error = "memory version consumed twice (not the current version of its "
            "chain); the interpreter executes memory in place";
    return failure();
  }
  b->current = to;
  mems[to] = b;
  return b.get();
}

LogicalResult Interpreter::execMem(Operation &op, std::string &error) {
  if (isa<MemNondetOp, MemConstOp>(op)) {
    auto memTy = cast<MemType>(op.getResult(0).getType());
    auto b = std::make_shared<Buffer>();
    b->current = op.getResult(0);
    b->baseOp = &op;
    if (auto mc = dyn_cast<MemConstOp>(op))
      b->fill = env.lookup(mc.getValue());
    b->cellWidth = memTy.getCell().getWidth();
    b->units = memTy.getCellUnits();
    b->layout = memTy.getLayout();
    b->abstract = isa<PtrType>(memTy.getIndex());
    mems[op.getResult(0)] = b;
    return success();
  }
  if (auto ld = dyn_cast<LoadOp>(op)) {
    checkAlign(op, ld.getAddr(), ld.getAlign());
    Buffer &buf = *mems.lookup(ld.getMem());
    if (buf.current != ld.getMem() && earlierVersionSeen.insert(&op).second)
      result.earlierVersionLoads.push_back(&op);
    if (buf.current != ld.getMem() && options.warnEarlierVersion)
      diag::warn("interp.load-earlier-version",
                 "load of an earlier memory version: the in-place "
                 "interpreter reads the current contents (correct only if "
                 "the cell was not clobbered since, as Mem-SSA claims)",
                 op.getLoc());
    // The value spans n cells from the address, little-endian.
    unsigned width = bitWidthOf(ld.getType());
    unsigned n = width / buf.cellWidth;
    SmallVector<CellValue> cells;
    for (unsigned i = 0; i < n; ++i) {
      FailureOr<APInt> k = cellKey(buf, ld.getAddr(), i * buf.units, error);
      if (failed(k)) {
        if (!deferViolation(op, error))
          return failure();
        // the substitute: 0, or the null pointer
        if (isa<PtrType>(ld.getType()))
          ptrs[ld.getResult()] = Ptr{0, APInt(width, 0)};
        else
          define(ld.getResult(), APInt::getZero(width));
        return success();
      }
      FailureOr<CellValue> v = read(buf, *k, error);
      if (failed(v))
        return failure();
      cells.push_back(*v);
    }
    if (isa<PtrType>(ld.getType())) {
      // addr0-is-null (RunOptions::addr0IsNull): all-zero integer bytes are
      // the null pointer
      if (options.addr0IsNull && llvm::all_of(cells, [](const CellValue &c) {
            return !c.ptr && c.bits.isZero();
          })) {
        ++result.addr0IsNullUses;
        ptrs[ld.getResult()] = Ptr{0, APInt::getZero(width)};
        return success();
      }
      for (unsigned i = 0; i < n; ++i)
        if (!cells[i].ptr || cells[i].piece != i ||
            !(*cells[i].ptr == *cells[0].ptr)) {
          error = "load of a pointer from cells that do not hold one whole "
                  "pointer";
          return failure();
        }
      ptrs[ld.getResult()] = *cells[0].ptr;
      return success();
    }
    APInt bits = APInt::getZero(width);
    bool nullBytes = false; // counted once per load
    for (unsigned i = 0; i < n; ++i) {
      if (cells[i].ptr) {
        // addr0-is-null: the bytes of the null pointer are 0
        if (options.addr0IsNull && cells[i].ptr->obj == 0 &&
            cells[i].ptr->off.isZero()) {
          nullBytes = true;
          continue; // bits stay 0
        }
        error = "load of a pointer cell as an integer: pointers have no "
                "address before placement";
        return failure();
      }
      bits.insertBits(cells[i].bits, i * buf.cellWidth);
    }
    result.addr0IsNullUses += nullBytes;
    define(ld.getResult(), bits);
    if (trackProv && cells[0].prov) {
      bool whole = true;
      for (unsigned i = 1; i < n; ++i)
        whole &= cells[i].prov && cells[i].prov->obj == cells[0].prov->obj &&
                 cells[i].prov->bit == cells[0].prov->bit + i * buf.cellWidth;
      if (whole)
        prov[ld.getResult()] = *cells[0].prov;
    }
    return success();
  }

  if (auto st = dyn_cast<StoreOp>(op))
    checkAlign(op, st.getAddr(), st.getAlign());
  else if (auto ms = dyn_cast<MemSetOp>(op))
    checkAlign(op, ms.getAddr(), ms.getAlign());
  else if (auto mv = dyn_cast<MemMoveOp>(op)) {
    checkAlign(op, mv.getAddr(), mv.getAlign());
    checkAlign(op, mv.getSrcAddr(), mv.getSrcAlign(), "src_align");
  }

  if (auto im = dyn_cast<IsModifiedOp>(op)) {
    Buffer &buf = *mems.lookup(im.getMem());
    if (!buf.abstract) {
      error = "modification tracking on placed memory is not supported yet";
      return failure();
    }
    Ptr p = ptrs.lookup(im.getPtr());
    define(im.getResult(), APInt(1, buf.modified.contains(p.obj)));
    return success();
  }

  // Updates: in place only.
  if (!op.hasAttr("inplace")) {
    error = "memory update without inplace; the interpreter executes memory "
            "in place only";
    return failure();
  }
  Value mem = op.getOperand(0);
  // deferMemoryErrors: an update with a violation does nothing (checked
  // before the length limit: a violation is the program's, whatever the
  // length)
  // memhavoc(NULL, n), as after a failed allocation: a no-op under
  // RunOptions::memhavocNullNoop (an open semantics decision, counted and
  // warned), before any range check or limit. Only null itself: null + k
  // is an address computed from null, a different case, and stays an
  // error. A placed program havocs address 0, as the VC does.
  bool nullNoop = false;
  if (auto mh = dyn_cast<MemHavocOp>(op);
      mh && options.memhavocNullNoop && isa<PtrType>(mh.getAddr().getType()) &&
      !env.lookup(mh.getLen()).isZero()) {
    Ptr p = ptrs.lookup(mh.getAddr());
    nullNoop = p.obj == 0 && p.off.getBitWidth() && p.off.isZero();
  }
  bool skip = false;
  if (options.deferMemoryErrors && !nullNoop) {
    LogicalResult inObject = success();
    if (auto st = dyn_cast<StoreOp>(op)) {
      Buffer &b = *mems.lookup(mem);
      unsigned units = bitWidthOf(st.getValue().getType()) / b.cellWidth;
      inObject = checkRange(st.getAddr(), APInt(64, uint64_t(units) * b.units),
                            b.units, error);
    } else if (auto ms = dyn_cast<MemSetOp>(op)) {
      inObject = checkRange(ms.getAddr(), env.lookup(ms.getLen()),
                            mems.lookup(mem)->units, error);
    } else if (auto mv = dyn_cast<MemMoveOp>(op)) {
      inObject = checkRange(mv.getSrcAddr(), env.lookup(mv.getLen()),
                            mems.lookup(mv.getSrc())->units, error);
      if (succeeded(inObject))
        inObject = checkRange(mv.getAddr(), env.lookup(mv.getLen()),
                              mems.lookup(mem)->units, error);
    } else if (auto mh = dyn_cast<MemHavocOp>(op)) {
      inObject = checkRange(mh.getAddr(), env.lookup(mh.getLen()),
                            mems.lookup(mem)->units, error);
    }
    if (failed(inObject)) {
      if (!deferViolation(op, error))
        return failure();
      skip = true;
    }
  }
  // memmove reads its source before the destination is handed over (the
  // source may be the same version).
  SmallVector<CellValue> moved;
  if (auto mv = dyn_cast<MemMoveOp>(op); mv && !skip) {
    Buffer &src = *mems.lookup(mv.getSrc());
    FailureOr<uint64_t> n =
        rangeCells(src, env.lookup(mv.getLen()), "memmove", error);
    if (failed(n))
      return failure();
    for (uint64_t k = 0; k < *n; ++k) {
      FailureOr<APInt> from =
          cellKey(src, mv.getSrcAddr(), k * src.units, error);
      if (failed(from))
        return failure();
      FailureOr<CellValue> v = read(src, *from, error);
      if (failed(v))
        return failure();
      moved.push_back(*v);
    }
  }
  FailureOr<Buffer *> b = consume(mem, op.getResult(0), error);
  if (failed(b))
    return failure();
  Buffer &buf = **b;
  if (skip)
    return success(); // the new version is the old contents
  if (nullNoop) {
    ++result.memhavocNullNoopUses;
    diag::warn("interp.memhavoc-null-noop",
               "sea.memhavoc of the null pointer runs as a no-op (an open "
               "semantics decision; --memhavoc-null=error makes it a "
               "memory-safety error)",
               op.getLoc());
    return success(); // the new version is the old contents
  }

  // Modification tracking: a tracked update that writes at least one byte
  // marks its object.
  if (op.hasAttr("tracked") || isa<ResetModifiedOp>(op)) {
    if (!buf.abstract) {
      error = "modification tracking on placed memory is not supported yet";
      return failure();
    }
    unsigned obj = ptrs.lookup(op.getOperand(1)).obj;
    if (isa<ResetModifiedOp>(op)) {
      buf.modified.erase(obj);
      return success();
    }
    Value len; // none for a store: it always writes
    if (auto ms = dyn_cast<MemSetOp>(op))
      len = ms.getLen();
    else if (auto mv = dyn_cast<MemMoveOp>(op))
      len = mv.getLen();
    else if (auto mh = dyn_cast<MemHavocOp>(op))
      len = mh.getLen();
    if (!len || !env.lookup(len).isZero())
      buf.modified.insert(obj);
  }

  if (auto st = dyn_cast<StoreOp>(op)) {
    unsigned n = bitWidthOf(st.getValue().getType()) / buf.cellWidth;
    for (unsigned i = 0; i < n; ++i) {
      FailureOr<APInt> k = cellKey(buf, st.getAddr(), i * buf.units, error);
      if (failed(k))
        return failure();
      write(buf, *k, cellOf(st.getValue(), i, buf.cellWidth));
    }
    return success();
  }
  if (auto ms = dyn_cast<MemSetOp>(op)) {
    FailureOr<uint64_t> n =
        rangeCells(buf, env.lookup(ms.getLen()), "memset", error);
    if (failed(n))
      return failure();
    // the value is one index unit; a word cell holds k copies of it
    APInt unit = env.lookup(ms.getValue());
    CellValue v{unit.getBitWidth() == buf.cellWidth
                    ? unit
                    : APInt::getSplat(buf.cellWidth, unit),
                std::nullopt, 0};
    for (uint64_t k = 0; k < *n; ++k) {
      FailureOr<APInt> at = cellKey(buf, ms.getAddr(), k * buf.units, error);
      if (failed(at))
        return failure();
      write(buf, *at, v);
    }
    return success();
  }
  if (auto mv = dyn_cast<MemMoveOp>(op)) {
    for (auto [k, v] : llvm::enumerate(moved)) {
      FailureOr<APInt> at = cellKey(buf, mv.getAddr(), k * buf.units, error);
      if (failed(at))
        return failure();
      write(buf, *at, v);
    }
    return success();
  }
  if (auto mh = dyn_cast<MemHavocOp>(op)) {
    APInt len = env.lookup(mh.getLen());
    // An empty range touches no cell (its start may be one past the end).
    if (len.isZero())
      return success();
    if (failed(rangeCells(buf, len, "memhavoc", error)))
      return failure();
    if (failed(cellKey(buf, mh.getAddr(), 0, error)))
      return failure();
    FailureOr<APInt> start = key(mh.getAddr(), 0, error);
    if (failed(start))
      return failure();
    if (buf.abstract &&
        failed(key(mh.getAddr(), len.getZExtValue() - 1, error)))
      return failure(); // the whole range must lie in the object
    buf.layers.push_back(
        {&op, *start, len.zextOrTrunc(start->getBitWidth()), ++buf.seq});
    return success();
  }
  error = "unsupported memory op";
  return failure();
}

LogicalResult Interpreter::execPtr(Operation &op, std::string &error) {
  auto setPtr = [&](Ptr p) { ptrs[op.getResult(0)] = p; };
  auto ptrOf = [&](Value v) { return ptrs.lookup(v); };
  auto alloc = [&](StringRef kind, Value size, uint64_t site, bool heap,
                   uint64_t align) {
    // Objects are named as in the segment map; an op executed again (in a
    // loop) gets #n.
    unsigned n = ++allocCount[&op];
    std::string name = (kind + "." + llvm::Twine(site)).str();
    if (n > 1)
      name += "#" + std::to_string(n);
    unsigned w = bitWidthOf(op.getResult(0).getType());
    setPtr({newObject(name, env.lookup(size), heap, align, kind, &op),
            APInt(w, 0)});
  };
  auto named = [&](std::string name, uint64_t size, uint64_t align) {
    unsigned w = bitWidthOf(op.getResult(0).getType());
    auto [it, inserted] = namedObjects.try_emplace(name, 0);
    if (inserted)
      it->second = newObject(name, APInt(w, size), false, align,
                             StringRef(name).split('.').first, &op);
    else if (auto l = op.getAttrOfType<StringAttr>("label");
             l && objects[it->second].label.empty())
      objects[it->second].label = l.str();
    setPtr({it->second, APInt(w, 0)});
  };

  if (auto a = dyn_cast<AllocaOp>(op)) {
    alloc("stack", a.getSize(), a.getSite(), false, a.getAlign());
    return success();
  }
  if (auto m = dyn_cast<MallocOp>(op)) {
    alloc("heap", m.getSize(), m.getSite(), true, m.getAlign());
    return success();
  }
  if (auto pn = dyn_cast<PtrNondetOp>(op)) {
    // a fresh opaque object of size 0: nothing through it is in bounds
    unsigned n = ++allocCount[&op];
    std::string name = "nondet." + std::to_string(pn.getSite());
    if (n > 1)
      name += "#" + std::to_string(n);
    unsigned w = bitWidthOf(pn.getType());
    setPtr(
        {newObject(name, APInt(w, 0), false, 1, "nondet", &op), APInt(w, 0)});
    return success();
  }
  if (auto g = dyn_cast<GlobalOp>(op)) {
    named(("global." + g.getName()).str(), g.getSize(), g.getAlign());
    return success();
  }
  if (auto f = dyn_cast<FuncAddrOp>(op)) {
    named(("func." + f.getName()).str(), 0, 1);
    return success();
  }
  if (auto fr = dyn_cast<FreeOp>(op)) {
    Ptr p = ptrOf(fr.getPtr());
    if (p.obj == 0)
      return success(); // free(NULL) does nothing
    Object &o = objects[p.obj];
    if (!o.alive) {
      errKind = RunErrorKind::DoubleFree;
      error = "double free of " + o.name;
      return deferViolation(op, error) ? success() : failure();
    }
    if (!o.heap || !p.off.isZero()) {
      errKind = RunErrorKind::BadFree;
      error = "free of a pointer that is not the start of a heap object (" +
              o.name + ")";
      return deferViolation(op, error) ? success() : failure();
    }
    o.alive = false;
    return success();
  }
  if (auto pa = dyn_cast<PtrAddOp>(op)) {
    Ptr p = ptrOf(pa.getPtr());
    p.off = p.off + env.lookup(pa.getOffset());
    setPtr(p);
    return success();
  }
  if (auto pi = dyn_cast<PtrIndexOp>(op)) {
    // docs/design/PtrIndex.md: the value is modular; a broken promise is a
    // warning (in LLVM a poison pointer, UB only when used), never a stop
    SmallVector<APInt> idx = valuesOf(pi.getIndices());
    APInt off = ptrIndexOffset(pi, idx);
    std::optional<APInt> base;
    if (isa<AddrType>(pi.getBase().getType()))
      base = env.lookup(pi.getBase());
    if (std::optional<StringRef> broken = brokenPtrIndexPromise(pi, base, idx))
      diag::warn("interp.ptr-index-promise",
                 "sea.ptr.index " + *broken +
                     ": the promise is broken (a poison pointer in LLVM)",
                 op.getLoc());
    if (base) {
      define(pi.getResult(), *base + off);
      if (trackProv)
        if (auto it = prov.find(pi.getBase()); it != prov.end())
          prov[pi.getResult()] = it->second;
      return success();
    }
    // an abstract pointer: the offset moves inside the object (P3 needs an
    // address: not checked before placement)
    Ptr p = ptrOf(pi.getBase());
    p.off = p.off + off;
    setPtr(p);
    return success();
  }
  if (auto pd = dyn_cast<PtrDiffOp>(op)) {
    Ptr a = ptrOf(pd.getLhs()), b = ptrOf(pd.getRhs());
    if (a.obj != b.obj) {
      error = "ptr.diff of pointers into different objects";
      return failure();
    }
    define(pd.getResult(), a.off - b.off);
    return success();
  }
  if (auto pc = dyn_cast<PtrCmpOp>(op)) {
    Ptr a = ptrOf(pc.getLhs()), b = ptrOf(pc.getRhs());
    ICmpPredicate pred = pc.getPredicate();
    bool r;
    if (pred == ICmpPredicate::eq || pred == ICmpPredicate::ne) {
      r = (a == b) == (pred == ICmpPredicate::eq);
    } else {
      if (a.obj != b.obj) {
        error = "ordered ptr.cmp of pointers into different objects";
        return failure();
      }
      // Same object. The VC compares addresses (ADDR(p) = base + offset
      // after placement); an abstract object has no base, so the semantics
      // table gets the addresses at a notional base 2^(w-2): offsets in
      // [-2^(w-2), 2^(w-2)) cross neither the unsigned (0) nor the signed
      // (2^(w-1)) boundary, as placement keeps objects off both. Inside the
      // object ([0, size]) every placement orders them the same way;
      // outside it the order depends on where the object is: warned.
      unsigned w = a.off.getBitWidth();
      APInt base = w >= 2 ? APInt::getOneBitSet(w, w - 2) : APInt(w, 0);
      if (a.obj != 0) {
        APInt size = objects[a.obj].size.zextOrTrunc(w);
        if (a.off.ugt(size) || b.off.ugt(size))
          diag::warn("interp.ptr-cmp-outside",
                     "ordered sea.ptr.cmp of a pointer outside its object: "
                     "the order of the addresses depends on the placement "
                     "(taken: no address wraps)",
                     op.getLoc());
      }
      r = evalPureOp(&op, {base + a.off, base + b.off})->isOne();
    }
    define(pc.getResult(), APInt(1, r));
    return success();
  }
  if (isa<PtrNullOp>(op)) {
    setPtr({0, APInt(bitWidthOf(op.getResult(0).getType()), 0)});
    return success();
  }
  if (auto pb = dyn_cast<PtrBaseOp>(op)) {
    Ptr p = ptrOf(pb.getPtr());
    setPtr({p.obj, APInt::getZero(p.off.getBitWidth())});
    return success();
  }
  if (auto pd = dyn_cast<PtrDereferenceableOp>(op)) {
    Ptr p = ptrOf(pd.getPtr());
    APInt len = env.lookup(pd.getLen());
    bool ok = false;
    if (p.obj != 0) {
      APInt size = objects[p.obj].size.zextOrTrunc(p.off.getBitWidth());
      ok = p.off.ule(size) && len.ule(size - p.off);
    }
    define(pd.getResult(), APInt(1, ok));
    return success();
  }
  if (auto ti = dyn_cast<PtrToIntOp>(op)) {
    // addr0-is-null: the address of the null pointer is 0
    Ptr p = ptrOf(ti.getPtr());
    if (options.addr0IsNull && p.obj == 0 && p.off.isZero()) {
      ++result.addr0IsNullUses;
      define(ti.getResult(),
             APInt::getZero(ti.getType().getIntOrFloatBitWidth()));
      return success();
    }
    error = "ptr.to_int: pointers have no address before placement";
    return failure();
  }
  if (auto fi = dyn_cast<PtrFromIntOp>(op)) {
    APInt v = env.lookup(fi.getValue());
    if (!v.isZero()) {
      error = "ptr.from_int of a non-zero value: pointers have no address "
              "before placement";
      return failure();
    }
    setPtr({0, APInt(v.getBitWidth(), 0)});
    return success();
  }
  error = "unsupported pointer op";
  return failure();
}

void Interpreter::execStruct(Operation &op) {
  if (auto c = dyn_cast<StructCreateOp>(op)) {
    SmallVector<Leaf> leaves;
    for (Value f : c.getFields())
      leaves.append(leavesOf(f));
    defineLeaves(c.getResult(), leaves);
    return;
  }
  auto leafRange = [](Value v) {
    return isa<StructType>(v.getType())
               ? cast<StructType>(v.getType()).getNumLeaves()
               : 1u;
  };
  if (auto e = dyn_cast<StructExtractOp>(op)) {
    SmallVector<Leaf> leaves = structs.lookup(e.getInput());
    unsigned at = e.getInput().getType().getLeafOffset(e.getIndex());
    defineLeaves(e.getResult(),
                 ArrayRef<Leaf>(leaves).slice(at, leafRange(e.getResult())));
    return;
  }
  auto ins = cast<StructInsertOp>(op);
  SmallVector<Leaf> leaves = structs.lookup(ins.getInput());
  unsigned at = ins.getInput().getType().getLeafOffset(ins.getIndex());
  SmallVector<Leaf> v = leavesOf(ins.getValue());
  for (unsigned i = 0; i < v.size(); ++i)
    leaves[at + i] = v[i];
  defineLeaves(ins.getResult(), leaves);
}

LogicalResult Interpreter::jump(Block *dest, ValueRange operands,
                                std::string &error) {
  SmallVector<APInt> vals = valuesOf(operands);
  for (auto [arg, from, val] :
       llvm::zip_equal(dest->getArguments(), operands, vals)) {
    if (isa<MemType>(arg.getType())) {
      if (failed(consume(from, arg, error)))
        return failure();
      continue;
    }
    if (isa<PtrType>(arg.getType())) {
      ptrs[arg] = ptrs.lookup(from);
      continue;
    }
    if (isa<StructType>(arg.getType())) {
      structs[arg] = structs.lookup(from);
      continue;
    }
    define(arg, val);
    if (trackProv)
      if (auto it = prov.find(from); it != prov.end())
        prov[arg] = it->second;
  }
  for (BlockArgument arg : dest->getArguments())
    afterValue(arg);
  return success();
}

RunResult Interpreter::run() {
  Block *block = &program.getBody().front();
  Block *from = nullptr;
  while (true) {
    if (options.onBlock)
      options.onBlock(from, block);
    Block *next = nullptr;
    for (Operation &op : *block) {
      if (++result.steps > options.maxSteps)
        return finish(RunStatus::StepLimit, &op);
      if (prevOp) {
        noteOp(*prevOp);
        for (Value r : prevOp->getResults())
          afterValue(r);
      }
      prevOp = &op;
      if (options.onOp)
        options.onOp(op, [&](Value v) -> std::optional<APInt> {
          auto it = env.find(v);
          if (it == env.end())
            return std::nullopt;
          return it->second;
        });
      if (isPrintOp(&op)) {
        if (options.prints)
          execPrint(op);
        continue;
      }

      std::string error;
      // (A sea.segment is the base address of a placed object: evalPureOp.)
      if (isa<HaltOp>(op))
        return finish(RunStatus::Halted, &op);
      if (isa<UnreachableOp>(op))
        return finish(RunStatus::AssumeBlocked, &op);
      if (isa<BrOp, CondBrOp>(op))
        noteBranch(op);
      if (auto br = dyn_cast<BrOp>(op)) {
        if (failed(jump(br.getDest(), br.getDestOperands(), error)))
          return finish(RunStatus::Error, &op, error);
        next = br.getDest();
        break;
      }
      if (auto cbr = dyn_cast<CondBrOp>(op)) {
        bool taken = env.lookup(cbr.getCondition()).isOne();
        next = taken ? cbr.getTrueDest() : cbr.getFalseDest();
        if (failed(jump(next,
                        taken ? cbr.getTrueDestOperands()
                              : cbr.getFalseDestOperands(),
                        error)))
          return finish(RunStatus::Error, &op, error);
        break;
      }
      if (auto assume = dyn_cast<AssumeOp>(op)) {
        if (env.lookup(assume.getCond()).isZero())
          return finish(RunStatus::AssumeBlocked, &op);
        if (options.onAssume)
          options.onAssume(assume);
        continue;
      }
      if (auto check = dyn_cast<AssertOp>(op)) {
        if (env.lookup(check.getCond()).isZero())
          return finish(RunStatus::AssertFailed, &op);
        continue;
      }
      if (isa<TrackingOnOp, TrackingOffOp>(op))
        return finish(RunStatus::Error, &op,
                      "tracking markers must be applied first "
                      "(sea-apply-tracking)");
      if (isa<MemNondetOp, MemConstOp, LoadOp, StoreOp, MemSetOp, MemMoveOp,
              MemHavocOp, IsModifiedOp, ResetModifiedOp>(op)) {
        if (failed(execMem(op, error)))
          return finish(RunStatus::Error, &op, error);
        continue;
      }

      if (isa<AllocaOp, MallocOp, GlobalOp, FuncAddrOp, FreeOp, PtrAddOp,
              PtrIndexOp, PtrDiffOp, PtrCmpOp, PtrNullOp, PtrToIntOp,
              PtrFromIntOp, PtrBaseOp, PtrDereferenceableOp, PtrNondetOp>(op)) {
        if (failed(execPtr(op, error)))
          return finish(RunStatus::Error, &op, error);
        continue;
      }
      if (auto sel = dyn_cast<SelectOp>(op);
          sel && isa<PtrType, StructType>(sel.getType())) {
        bool c = env.lookup(sel.getCondition()).isOne();
        defineLeaves(sel.getResult(),
                     leavesOf(c ? sel.getTrueValue() : sel.getFalseValue()));
        continue;
      }
      if (auto sn = dyn_cast<StructNondetOp>(op)) {
        // One bit-vector for the integer / address leaves, low bits first;
        // pointer leaves are null.
        APInt bits;
        if (unsigned w = sn.getBitsWidth()) {
          std::string origin;
          FailureOr<APInt> v = nondets.get(sn, origin, error);
          if (failed(v))
            return finish(RunStatus::Error, &op, error);
          result.reads.push_back(
              {sn, sn.getSite(), sn.getCallee(), *v, origin});
          bits = *v;
        }
        SmallVector<Type> types;
        std::function<void(Type)> flatten = [&](Type t) {
          if (auto st = dyn_cast<StructType>(t))
            for (Type f : st.getFields())
              flatten(f);
          else
            types.push_back(t);
        };
        flatten(sn.getType());
        SmallVector<Leaf> leaves;
        unsigned at = 0;
        for (Type t : types) {
          if (isa<PtrType>(t)) {
            leaves.push_back({APInt(), Ptr{0, APInt(bitWidthOf(t), 0)}});
            continue;
          }
          unsigned w = bitWidthOf(t);
          leaves.push_back({bits.extractBits(w, at), std::nullopt});
          at += w;
        }
        defineLeaves(sn.getResult(), leaves);
        continue;
      }
      if (isa<StructCreateOp, StructExtractOp, StructInsertOp>(op)) {
        execStruct(op);
        continue;
      }
      if (auto nd = dyn_cast<NondetOp>(op)) {
        std::string origin;
        FailureOr<APInt> v = nondets.get(nd, origin, error);
        if (failed(v))
          return finish(RunStatus::Error, &op, error);
        result.reads.push_back({nd, nd.getSite(), nd.getCallee(), *v, origin});
        define(nd.getResult(), *v);
        continue;
      }
      SmallVector<APInt> operands = valuesOf(op.getOperands());
      std::optional<APInt> v = evalPureOp(&op, operands);
      if (std::optional<StringRef> broken = brokenWrapPromise(&op, operands))
        diag::warn("interp.wrap-promise",
                   op.getName().getStringRef() + " " + *broken +
                       ": the promise is broken (the value wraps; run "
                       "sea-wrap-flags to assume or assert it)",
                   op.getLoc());
      if (!v)
        return finish(RunStatus::Error, &op,
                      "unsupported op '" + op.getName().getStringRef().str() +
                          "'");
      define(op.getResult(0), *v);
      if (trackProv)
        propagateProv(op, operands);
    }
    if (!next)
      return finish(RunStatus::Error, block->empty() ? nullptr : &block->back(),
                    "block ended without a terminator");
    from = block;
    block = next;
  }
}

//===----------------------------------------------------------------------===//
// Counterexample prints (docs/design/Print.md)
//===----------------------------------------------------------------------===//

ProgramFacts programFactsOf(ProgramOp program) {
  ProgramFacts f;
  program.walk([&](Operation *op) {
    if (isa<SegmentOp>(op))
      f.hasSegments = true;
    if (auto p = dyn_cast<PrintOp>(op))
      f.hasWeakPrints |= p.getRef().has_value();
    if (auto p = dyn_cast<PrintMemOp>(op))
      f.hasWeakPrints |= !p.isStrong();
  });
  return f;
}

void Interpreter::preparePrints() {
  ProgramFacts facts = options.facts ? *options.facts : programFactsOf(program);
  trackProv = facts.hasSegments;
  bool weak = facts.hasWeakPrints;
  // the pointer tokens of RunOptions::onPointer need provenance too
  if (!weak || !options.prints)
    return;
  auto add = [&](Value v) {
    SmallVector<std::string, 2> n = printNamesOf(v);
    if (!n.empty())
      valueNames[v] = std::move(n);
  };
  program.walk([&](Operation *op) {
    for (Value r : op->getResults())
      add(r);
    for (Region &r : op->getRegions())
      for (Block &b : r)
        for (BlockArgument a : b.getArguments())
          add(a);
  });
}

void Interpreter::afterValue(Value v) {
  if (!valueNames.empty())
    if (auto it = valueNames.find(v); it != valueNames.end())
      for (const std::string &n : it->second)
        named[n] = v;
  if (!options.onPointer)
    return;
  SmallVector<unsigned> refs;
  if (isa<PtrType>(v.getType())) {
    if (ptrs.count(v))
      options.onPointer(v, nullptr, pointerText(v, refs));
  } else if (isa<AddrType>(v.getType()) && env.count(v)) {
    APInt bits = env.lookup(v);
    options.onPointer(v, &bits, pointerText(v, refs));
  }
}

void Interpreter::propagateProv(Operation &op, ArrayRef<APInt> operands) {
  Value r = op.getResult(0);
  auto from = [&](Value v) -> std::optional<Prov> {
    auto it = prov.find(v);
    if (it == prov.end())
      return std::nullopt;
    return it->second;
  };
  std::optional<Prov> p;
  if (auto seg = dyn_cast<SegmentOp>(op)) {
    auto [it, inserted] = segmentObject.try_emplace(&op, 0);
    if (inserted) {
      it->second =
          newObject(seg.getName().str(), seg.getSize(), seg.getKind() == "heap",
                    seg.getAlign(), seg.getKind(), &op);
      objects[it->second].base = seg.getBase();
    }
    p = Prov{it->second, 0};
  } else if (isa<AddrAddOp, AddrToBitsOp, AddrFromBitsOp>(op)) {
    p = from(op.getOperand(0));
  } else if (auto sel = dyn_cast<SelectOp>(op)) {
    p = from(operands[0].isOne() ? sel.getTrueValue() : sel.getFalseValue());
  } else if (auto ex = dyn_cast<ExtractOp>(op)) {
    if ((p = from(ex.getInput())))
      p->bit += ex.getLow();
  } else if (auto tr = dyn_cast<TruncOp>(op)) {
    p = from(tr.getInput());
  } else if (auto cc = dyn_cast<ConcatOp>(op)) {
    std::optional<Prov> hi = from(cc.getHi()), lo = from(cc.getLo());
    if (hi && lo && hi->obj == lo->obj &&
        hi->bit == lo->bit + bitWidthOf(cc.getLo().getType()))
      p = lo;
  }
  if (p)
    prov[r] = *p;
}

void Interpreter::emit(PrintRecord::Kind kind, std::string text, Operation *op,
                       std::string name, out::Fields fields,
                       std::string jkind) {
  if (kind != PrintRecord::Scope && options.printLimit &&
      printRecords >= options.printLimit) {
    ++result.printsTruncated;
    return;
  }
  if (kind != PrintRecord::Scope)
    ++printRecords;
  PrintRecord r{kind,
                (unsigned)scopes.size(),
                llvm::join(scopes, "."),
                std::move(text),
                op,
                std::move(name),
                std::move(jkind),
                std::move(fields)};
  if (kind == PrintRecord::Scope) // the scope's own line sits outside it
    r.depth = scopes.size() - 1;
  result.prints.push_back(std::move(r));
}

void Interpreter::emitLine(std::string name, std::string text,
                           ArrayRef<unsigned> refs, Operation *op,
                           out::Fields fields, std::string jkind) {
  if (options.printLimit && printRecords >= options.printLimit) {
    ++result.printsTruncated;
    return;
  }
  for (unsigned o : refs)
    if (objects[o].kind != "null" && listedObjects.insert(o).second)
      emit(PrintRecord::Object, formatObject(objects[o]), op, {},
           objectFields(objects[o]));
  emit(PrintRecord::Line, std::move(text), op, std::move(name),
       std::move(fields), std::move(jkind));
}

std::string Interpreter::ptrText(const Ptr &p, SmallVectorImpl<unsigned> &refs,
                                 out::Fields *f) {
  refs.push_back(p.obj);
  if (f) {
    f->push_back({"type", out::jstr("ptr")});
    out::Fields pf = pointerFields(objects[p.obj], p.off);
    f->insert(f->end(), pf.begin(), pf.end());
  }
  return pointerToken(objects[p.obj], p.off);
}

std::string Interpreter::addrText(const APInt &bits, std::optional<Prov> p,
                                  SmallVectorImpl<unsigned> &refs,
                                  out::Fields *f) {
  if (f) {
    f->push_back({"type", out::jstr("addr")});
    f->push_back({"addr", out::jint(bits)});
  }
  if (p && p->bit == 0 && p->obj < objects.size() && objects[p->obj].base) {
    const Object &o = objects[p->obj];
    refs.push_back(p->obj);
    APInt off = bits - o.base->zextOrTrunc(bits.getBitWidth());
    if (f) {
      out::Fields pf = pointerFields(o, off);
      f->insert(f->end(), pf.begin(), pf.end());
    }
    return pointerToken(o, off);
  }
  // no provenance: the number, and the placed object it falls in as a
  // hint (`~`), which also goes to the table
  if (bits.isZero()) {
    if (f)
      f->push_back({"obj", out::jstr("null")});
    return "null";
  }
  std::string t = "0x" + llvm::toString(bits, 16, false);
  for (unsigned i = 0; i < objects.size(); ++i) {
    const Object &o = objects[i];
    if (!o.base || o.kind == "null")
      continue;
    unsigned w = std::max(bits.getBitWidth(), o.base->getBitWidth()) + 1;
    APInt a = bits.zext(w), b = o.base->zext(w);
    if (a.uge(b) && a.ult(b + o.size.zextOrTrunc(w))) {
      refs.push_back(i);
      t += " ~" + o.name + "[" +
           llvm::toString((a - b).trunc(w - 1), 10, false) + "]";
      if (f) {
        f->push_back({"near", out::jstr(o.name)});
        f->push_back({"near_off", out::jint((a - b).trunc(w - 1))});
      }
      break;
    }
  }
  return t;
}

std::string Interpreter::pointerText(Value v, SmallVectorImpl<unsigned> &refs) {
  if (isa<PtrType>(v.getType()))
    return ptrText(ptrs.lookup(v), refs);
  std::optional<Prov> p;
  if (auto it = prov.find(v); it != prov.end())
    p = it->second;
  return addrText(env.lookup(v), p, refs);
}

std::string Interpreter::leafText(Type t, const Leaf &leaf,
                                  std::optional<Prov> p, StringRef fmt,
                                  SmallVectorImpl<unsigned> &refs,
                                  out::Fields *f) {
  if (leaf.ptr)
    return ptrText(*leaf.ptr, refs, f);
  if (isa<AddrType, PtrType>(t))
    return addrText(leaf.bits, p, refs, f);
  if (f) {
    out::Fields i = printIntFields(leaf.bits, fmt);
    f->insert(f->end(), i.begin(), i.end());
  }
  return formatPrintInt(leaf.bits, fmt);
}

/// Do a print's expected type and a definition's type agree? A pointer and
/// an address of one width do (placement), leaf by leaf in structs.
static bool printTypesAgree(Type expected, Type actual) {
  if (expected == actual)
    return true;
  auto leaves = [](Type t) {
    SmallVector<Type> out;
    std::function<void(Type)> walk = [&](Type t) {
      if (auto st = dyn_cast<StructType>(t))
        for (Type f : st.getFields())
          walk(f);
      else
        out.push_back(t);
    };
    walk(t);
    return out;
  };
  SmallVector<Type> e = leaves(expected), a = leaves(actual);
  if (e.size() != a.size())
    return false;
  for (auto [x, y] : llvm::zip_equal(e, a)) {
    if (x == y)
      continue;
    if (!isa<AddrType, PtrType>(x) || !isa<AddrType, PtrType>(y) ||
        bitWidthOf(x) != bitWidthOf(y))
      return false;
  }
  return true;
}

void Interpreter::valueLines(StringRef tag, Value v, PrintOp print,
                             SmallVectorImpl<ValueLine> &out,
                             SmallVectorImpl<unsigned> &refs) {
  StringRef fmt = print.getFmt() ? *print.getFmt() : "auto";
  if (!isa<StructType>(v.getType())) {
    Leaf leaf;
    if (isa<PtrType>(v.getType()))
      leaf.ptr = ptrs.lookup(v);
    else
      leaf.bits = env.lookup(v);
    std::optional<Prov> p;
    if (auto it = prov.find(v); it != prov.end())
      p = it->second;
    ValueLine l{tag.str(), "", {}};
    l.value = leafText(v.getType(), leaf, p, fmt, refs, &l.fields);
    out.push_back(std::move(l));
    return;
  }
  auto st = cast<StructType>(v.getType());
  SmallVector<Leaf> leaves = structs.lookup(v);
  SmallVector<Type> types;
  std::function<void(Type)> walk = [&](Type t) {
    if (auto s = dyn_cast<StructType>(t))
      for (Type f : s.getFields())
        walk(f);
    else
      types.push_back(t);
  };
  walk(st);
  for (unsigned i = 0; i < leaves.size() && i < types.size(); ++i) {
    std::string name = print.getNames()
                           ? cast<StringAttr>((*print.getNames())[i]).str()
                           : std::to_string(i);
    ValueLine l{(tag + "." + name).str(), "", {}};
    l.value = leafText(types[i], leaves[i], std::nullopt, fmt, refs, &l.fields);
    out.push_back(std::move(l));
  }
}

void Interpreter::execPrint(Operation &op) {
  if (auto sc = dyn_cast<PrintScopeOp>(op)) {
    scopes.push_back(sc.getTag().str());
    emit(PrintRecord::Scope, sc.getTag().str(), &op);
    return;
  }
  if (auto end = dyn_cast<PrintEndOp>(op)) {
    auto it = llvm::find(llvm::reverse(scopes), end.getTag());
    if (it == scopes.rend()) {
      emit(PrintRecord::Note,
           "unmatched sea.print.end \"" + end.getTag().str() + "\"", &op);
      return;
    }
    if (it != scopes.rbegin())
      emit(PrintRecord::Note,
           "sea.print.end \"" + end.getTag().str() +
               "\" closes inner scopes too",
           &op);
    scopes.erase(std::prev(it.base()), scopes.end());
    return;
  }
  if (auto pm = dyn_cast<PrintMemOp>(op)) {
    execPrintMem(pm);
    return;
  }
  auto print = cast<PrintOp>(op);
  std::string tag = print.getTag().str();
  if (!print.getRef() && !print.isStrong()) {
    emitLine(tag, tag, {}, &op);
    return;
  }
  SmallVector<ValueLine> lines;
  SmallVector<unsigned> refs;
  if (print.getRef()) {
    Value v = lookupName(*print.getRef());
    if (!v || !printTypesAgree(*print.getType(), v.getType())) {
      emitLine(tag, tag + " = <dangling: " + print.getRef()->str() + ">", {},
               &op,
               {{"error", out::jstr("dangling")},
                {"ref", out::jstr(*print.getRef())}});
      return;
    }
    valueLines(tag, v, print, lines, refs);
  } else if (print.getArgs().size() == 1 && !print.getType()) {
    valueLines(tag, print.getArgs()[0], print, lines, refs);
  } else {
    // a struct by its leaves (sea-scalarize-structs)
    StringRef fmt = print.getFmt() ? *print.getFmt() : "auto";
    for (auto [i, v] : llvm::enumerate(print.getArgs())) {
      std::string name = print.getNames()
                             ? cast<StringAttr>((*print.getNames())[i]).str()
                             : std::to_string(i);
      Leaf leaf;
      if (isa<PtrType>(v.getType()))
        leaf.ptr = ptrs.lookup(v);
      else
        leaf.bits = env.lookup(v);
      std::optional<Prov> p;
      if (auto it = prov.find(v); it != prov.end())
        p = it->second;
      ValueLine l{tag + "." + name, "", {}};
      l.value = leafText(v.getType(), leaf, p, fmt, refs, &l.fields);
      lines.push_back(std::move(l));
    }
  }
  for (ValueLine &l : lines) {
    l.fields.push_back({"repr", out::jstr(l.value)});
    emitLine(l.name, l.name + " = " + l.value, refs, &op, std::move(l.fields));
  }
}

std::optional<CellValue> Interpreter::peekCell(Buffer &b, const APInt &key,
                                               bool &dflt) {
  dflt = false;
  auto it = b.cells.find(key);
  uint64_t cellSeq = it == b.cells.end() ? 0 : it->second.seq;
  const Buffer::Layer *layer = nullptr;
  for (const Buffer::Layer &l : llvm::reverse(b.layers))
    if (l.seq > cellSeq && (key - l.start).ult(l.len)) {
      layer = &l;
      break;
    }
  if (!layer && it != b.cells.end()) {
    dflt = it->second.dflt;
    return it->second.value;
  }
  if (!layer && b.fill)
    return CellValue{*b.fill, std::nullopt};
  Operation *siteOp = layer ? layer->siteOp : b.baseOp;
  auto [object, at] = splitKey(b, key);
  std::string origin;
  std::optional<APInt> v =
      nondets.peekCell(siteOp, at, object, b.cellWidth, origin);
  if (!v)
    return std::nullopt;
  dflt = origin == "default";
  return CellValue{*v, std::nullopt};
}

Interpreter::PeekedByte Interpreter::peekByte(Buffer &b, unsigned obj,
                                              const APInt &off) {
  PeekedByte out;
  // byte-addressed data cells only: a cell is `units` bytes
  if (b.cellWidth != 8 * b.units)
    return out;
  unsigned w = off.getBitWidth();
  if (b.abstract) {
    const Object &o = objects[obj];
    if (obj == 0 || !o.alive || off.uge(o.size.zextOrTrunc(w)))
      return out;
  }
  uint64_t k = off.urem(b.units);
  APInt start = off - k;
  if (b.layout == MemLayout::unaligned) {
    // a cell may start anywhere: the one written over this byte, if any
    bool found = false;
    for (uint64_t d = 0; d < b.units && !found; ++d) {
      APInt s = off - d;
      APInt key = b.abstract ? (APInt(w + 32, obj) << w) | s.zext(w + 32) : s;
      if (b.cells.count(key)) {
        start = s;
        k = d;
        found = true;
      }
    }
    if (!found && b.units > 1)
      return out;
  }
  APInt key =
      b.abstract ? (APInt(w + 32, obj) << w) | start.zext(w + 32) : start;
  bool dflt = false;
  std::optional<CellValue> c = peekCell(b, key, dflt);
  if (!c)
    return out;
  if (c->ptr) {
    out.ptr = c->ptr;
    out.ptrByte = c->piece * b.cellWidth / 8 + k;
    out.byte.value = 0;
    return out;
  }
  out.byte.value = c->bits.extractBitsAsZExtValue(8, 8 * k);
  out.byte.dflt = dflt;
  if (c->prov)
    out.prov = Prov{c->prov->obj, c->prov->bit + unsigned(8 * k)};
  return out;
}

void Interpreter::execPrintMem(PrintMemOp op) {
  std::string tag = op.getTag().str();
  auto dangling = [&](StringRef name) {
    emitLine(tag, tag + " = <dangling: " + name.str() + ">", {}, op,
             {{"error", out::jstr("dangling")}, {"ref", out::jstr(name)}});
  };
  // the memory, the address, the count
  Value mem = op.getMemValue(), addr = op.getAddrValue(),
        countV = op.getCountValue();
  if (!op.isStrong()) {
    mem = lookupName(*op.getMem());
    if (!mem || !isa<MemType>(mem.getType()) || !mems.count(mem))
      return dangling(*op.getMem());
    addr = lookupName(*op.getAddr());
    if (!addr || !isa<AddrType, PtrType>(addr.getType()))
      return dangling(*op.getAddr());
    if (op.getCountRef()) {
      countV = lookupName(*op.getCountRef());
      if (!countV || !isa<IntegerType>(countV.getType()))
        return dangling(*op.getCountRef());
    }
  }
  Buffer &buf = *mems.lookup(mem);
  if (op.isStrong() && buf.current != mem) {
    // the version was consumed: its contents are gone (in-place memory)
    emitLine(tag, tag + " = <stale: an earlier memory version>", {}, op,
             {{"error", out::jstr("stale")}});
    return;
  }
  uint64_t count = op.getCount() ? *op.getCount() : 1;
  if (countV) {
    APInt c = env.lookup(countV);
    constexpr uint64_t kMaxCount = 1 << 16;
    count = c.getActiveBits() > 32
                ? kMaxCount
                : std::min<uint64_t>(c.getZExtValue(), kMaxCount);
    if (c.ugt(kMaxCount))
      emit(PrintRecord::Note,
           tag + ": count " + llvm::toString(c, 10, false) + " cut to " +
               std::to_string(kMaxCount),
           op);
  }
  Type elemTy = op.getType();
  unsigned elemBytes = bitWidthOf(elemTy) / 8;
  int64_t extra = op.getOffset() ? *op.getOffset() : 0;
  // where: an abstract pointer, or address bits with their provenance
  unsigned obj = 0;
  APInt start;
  std::optional<Prov> p;
  SmallVector<unsigned> refs;
  if (isa<PtrType>(addr.getType())) {
    Ptr ptr = ptrs.lookup(addr);
    obj = ptr.obj;
    start = ptr.off + APInt(ptr.off.getBitWidth(), extra, /*isSigned=*/true);
    refs.push_back(obj);
  } else {
    APInt a = env.lookup(addr);
    start = a + APInt(a.getBitWidth(), extra, /*isSigned=*/true);
    if (auto it = prov.find(addr); it != prov.end() && it->second.bit == 0) {
      p = it->second;
      refs.push_back(p->obj);
    }
  }
  unsigned w = start.getBitWidth();
  // a byte range: `heap.3[8..28]` (`[0x..]` without provenance)
  auto rangeText = [&](uint64_t len) -> std::string {
    if (buf.abstract || p) {
      const Object &o = objects[buf.abstract ? obj : p->obj];
      APInt off = buf.abstract
                      ? start
                      : start - o.base->zextOrTrunc(start.getBitWidth());
      return o.name + "[" + llvm::toString(off, 10, true) + ".." +
             llvm::toString(off + len, 10, true) + "]";
    }
    return "[0x" + llvm::toString(start, 16, false) + "..0x" +
           llvm::toString(start + len, 16, false) + "]";
  };
  SmallVector<PeekedByte> bytes;
  for (uint64_t i = 0; i < count * elemBytes; ++i)
    bytes.push_back(peekByte(buf, obj, start + APInt(w, i)));

  if (op.getFmt() && *op.getFmt() == "hexdump") {
    SmallVector<DumpByte> dump;
    for (const PeekedByte &b : bytes)
      dump.push_back(b.ptr ? DumpByte{} : b.byte);
    // jsonl: the range, the bytes as hex ("00" where none), the unknown
    // (`??`) and model-default (`..`) byte ranges
    out::Fields f;
    if (buf.abstract || p) {
      const Object &o = objects[buf.abstract ? obj : p->obj];
      APInt off = buf.abstract
                      ? start
                      : start - o.base->zextOrTrunc(start.getBitWidth());
      f.push_back({"obj", out::jstr(o.name)});
      f.push_back({"lo", out::jint(off, /*isSigned=*/true)});
      f.push_back({"hi", out::jint(off + APInt(w, count), /*isSigned=*/true)});
    } else {
      f.push_back({"addr_lo", out::jint(start)});
      f.push_back({"addr_hi", out::jint(start + APInt(w, count))});
    }
    std::string hexBytes;
    auto ranges = [&](auto pred) {
      std::string r = "[";
      for (size_t i = 0; i < dump.size();) {
        if (!pred(dump[i])) {
          ++i;
          continue;
        }
        size_t j = i;
        while (j < dump.size() && pred(dump[j]))
          ++j;
        r += (r.size() > 1 ? ",[" : "[") + std::to_string(i) + "," +
             std::to_string(j) + "]";
        i = j;
      }
      return r + "]";
    };
    for (const DumpByte &b : dump)
      hexBytes +=
          llvm::utohexstr(b.value ? *b.value : 0, /*LowerCase=*/true, 2);
    f.push_back({"bytes", out::jstr(hexBytes)});
    f.push_back(
        {"unknown", ranges([](const DumpByte &b) { return !b.value; })});
    f.push_back({"default",
                 ranges([](const DumpByte &b) { return b.value && b.dflt; })});
    f.push_back({"repr", out::jstr(rangeText(count))});
    emitLine(tag, tag + " = " + rangeText(count), refs, op, std::move(f),
             "print.mem");
    for (const std::string &row : hexDumpRows(dump))
      emitLine(tag, tag + " " + row, {}, op, {{"row", out::jstr(row)}},
               "print.mem.row");
    return;
  }
  StringRef fmt = op.getFmt() ? *op.getFmt() : "auto";
  for (uint64_t e = 0; e < count; ++e) {
    ArrayRef<PeekedByte> el =
        ArrayRef<PeekedByte>(bytes).slice(e * elemBytes, elemBytes);
    SmallVector<unsigned> elRefs;
    std::string text;
    out::Fields f;
    bool unreadable = llvm::any_of(
        el, [](const PeekedByte &b) { return !b.byte.value && !b.ptr; });
    bool anyPtr = llvm::any_of(el, [](const PeekedByte &b) { return b.ptr; });
    bool dflt =
        llvm::any_of(el, [](const PeekedByte &b) { return b.byte.dflt; });
    if (unreadable) {
      text = "?";
      f.push_back({"unknown", "true"});
    } else if (anyPtr) {
      // an abstract pointer, if the bytes are its bytes in order
      bool whole = isa<AddrType, PtrType>(elemTy);
      for (auto [j, b] : llvm::enumerate(el))
        whole &= b.ptr && *b.ptr == *el[0].ptr && b.ptrByte == j;
      if (whole)
        text = ptrText(*el[0].ptr, elRefs, &f);
      else {
        text = "<pointer bytes>";
        f.push_back({"error", out::jstr("pointer-bytes")});
      }
    } else {
      APInt bits(8 * elemBytes, 0);
      for (auto [j, b] : llvm::enumerate(el))
        bits.insertBits(APInt(8, *b.byte.value), 8 * j);
      if (isa<AddrType, PtrType>(elemTy)) {
        std::optional<Prov> ep;
        if (el[0].prov && el[0].prov->bit == 0) {
          ep = el[0].prov;
          for (auto [j, b] : llvm::enumerate(el))
            if (!b.prov || b.prov->obj != ep->obj || b.prov->bit != 8 * j)
              ep.reset();
        }
        if (buf.abstract) {
          text = llvm::toString(bits, 16, false) + " (not a pointer)";
          f.push_back({"error", out::jstr("not-a-pointer")});
          f.push_back({"value", out::jint(bits)});
        } else {
          text = addrText(bits, ep, elRefs, &f);
        }
      } else {
        text = formatPrintInt(bits, fmt);
        out::Fields i = printIntFields(bits, fmt);
        f.insert(f.end(), i.begin(), i.end());
      }
      if (dflt) {
        text += " (default)";
        f.push_back({"default", "true"});
      }
    }
    std::string lhs = count == 1 && !op.getCountRef() && !countV
                          ? tag
                          : tag + "[" + std::to_string(e) + "]";
    elRefs.append(refs.begin(), refs.end());
    f.push_back({"repr", out::jstr(text)});
    emitLine(lhs, lhs + " = " + text, elRefs, op, std::move(f));
  }
}

RunResult interpret(ProgramOp program, NondetSource &nondets,
                    const RunOptions &options) {
  return Interpreter(program, nondets, options).run();
}

} // namespace sea
