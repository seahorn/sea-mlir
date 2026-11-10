// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Interpreter.h - concrete interpreter for sea programs ----*- C++ -*-===//
//
// Runs one sea.program on concrete values. Its jobs are golden semantics and
// counterexample replay, so it stays small: values come from Semantics.h, and
// nondet values come from a NondetSource (user inputs or a solver model).
//
// Memory is executed in place (docs/design/Memory.md): one buffer per chain
// of versions. Every memory update must carry `inplace` and consume the
// current version of its buffer; anything else stops the run with an error.
// Loads may read earlier versions (they see the buffer as it is now).
//
// Abstract programs (docs/design/Memory-Step2.md) run too: a pointer is an
// (object, offset) pair, objects are created by allocation ops, and every
// access is checked against its object (null, freed, out of bounds are
// errors). Pointers have no address: ptr.to_int, ptr.from_int of a non-zero
// value, and reading a pointer cell as an integer stop the run.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_INTERP_INTERPRETER_H
#define SEA_INTERP_INTERPRETER_H

#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Output.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>

namespace sea {

/// Supplies the value of each executed sea.nondet.
class NondetSource {
public:
  virtual ~NondetSource();
  /// Returns the value for `op` (of the op's width) and sets `origin` to a
  /// short name of where it came from, or fails with a message in `error`.
  virtual mlir::FailureOr<llvm::APInt> get(NondetOp op, std::string &origin,
                                           std::string &error) = 0;
  /// The nondet bit-vector of a sea.struct.nondet (of op.getBitsWidth()
  /// bits). Sources that only see programs without structs (a VC model)
  /// fail.
  virtual mlir::FailureOr<llvm::APInt>
  get(StructNondetOp op, std::string &origin, std::string &error);

  /// Returns the value of the cell at `addr` in the memory created by
  /// `siteOp` (a sea.mem.nondet, or the havocked range of a sea.memhavoc).
  /// For an abstract memory, `object` names the object and `addr` is the
  /// offset in it; for a concrete memory `object` is empty. Asked at most
  /// once per cell and version.
  virtual mlir::FailureOr<llvm::APInt>
  getCell(mlir::Operation *siteOp, const llvm::APInt &addr,
          llvm::StringRef object, unsigned cellWidth, std::string &origin,
          std::string &error) = 0;

  /// The value of a cell as getCell would give it, for a print
  /// (docs/design/Print.md): never fails and never changes the source (no
  /// random draw, nothing recorded). None if the source has no value for it.
  virtual std::optional<llvm::APInt>
  peekCell(mlir::Operation *siteOp, const llvm::APInt &addr,
           llvm::StringRef object, unsigned cellWidth, std::string &origin);
};

/// Values given per nondet site, with a policy for sites that have none.
class InputSource : public NondetSource {
public:
  enum class Missing { Error, Zero, Random };

  InputSource(Missing missing = Missing::Error, uint64_t seed = 0)
      : missing(missing), rng(seed) {}

  /// Sets the text of the value for `site`: decimal (optionally negative) or
  /// 0x-prefixed hex. It is checked against the width when it is read.
  void set(uint64_t site, std::string text) { inputs[site] = std::move(text); }
  /// Sets the text of the value of cell `addr` of the memory created at
  /// `site`. `addr` is the address in unsigned decimal, or, for an abstract
  /// memory, `<object>+<offset>` with the offset in unsigned decimal.
  void setCell(uint64_t site, std::string addr, std::string text) {
    cells[{site, std::move(addr)}] = std::move(text);
  }

  mlir::FailureOr<llvm::APInt> get(NondetOp op, std::string &origin,
                                   std::string &error) override;
  mlir::FailureOr<llvm::APInt> get(StructNondetOp op, std::string &origin,
                                   std::string &error) override;
  mlir::FailureOr<llvm::APInt> getCell(mlir::Operation *siteOp,
                                       const llvm::APInt &addr,
                                       llvm::StringRef object,
                                       unsigned cellWidth, std::string &origin,
                                       std::string &error) override;
  std::optional<llvm::APInt> peekCell(mlir::Operation *siteOp,
                                      const llvm::APInt &addr,
                                      llvm::StringRef object,
                                      unsigned cellWidth,
                                      std::string &origin) override;

private:
  mlir::FailureOr<llvm::APInt> bySite(uint64_t site, unsigned width,
                                      std::string &origin, std::string &error);
  mlir::FailureOr<llvm::APInt> missingValue(unsigned width,
                                            const std::string &what,
                                            std::string &origin,
                                            std::string &error);

  llvm::DenseMap<uint64_t, std::string> inputs;
  std::map<std::pair<uint64_t, std::string>, std::string> cells;
  Missing missing;
  std::mt19937_64 rng;
};

/// Parses `text` as a value of `width` bits: unsigned in [0, 2^w) or negative
/// in [-2^(w-1), 0). Fails if the text is malformed or does not fit.
mlir::FailureOr<llvm::APInt> parseBitVector(llvm::StringRef text,
                                            unsigned width);

enum class RunStatus {
  Halted,        ///< reached sea.halt
  AssertFailed,  ///< a sea.assert saw 0
  AssumeBlocked, ///< a sea.assume saw 0: the execution is infeasible
  StepLimit,     ///< ran out of steps (the program may loop)
  /// RunOptions::deferMemoryErrors: the run made a memory-safety violation
  /// and then reached sea.halt or a failing sea.assert (a finding: the
  /// violation is on a run no assume blocks; RunResult::reachedAt)
  MemoryError,
  Error, ///< could not continue (missing input, unknown op)
  // the last value: sea::kRunStatusCount (Fuzz.h) counts them
};

llvm::StringRef stringifyRunStatus(RunStatus status);

/// Why a run stopped with RunStatus::Error, or the kind of a memory-safety
/// violation (RunStatus::MemoryError, a deferred one).
enum class RunErrorKind {
  None,
  // memory safety (isMemorySafetyKind): the program's error
  NullAccess,   ///< an access through the null pointer
  OutOfBounds,  ///< an access out of its object's bounds
  UseAfterFree, ///< an access to a freed object
  DoubleFree,   ///< sea.free of a freed object
  BadFree,      ///< sea.free of a pointer that is not a heap object's start
  // not memory safety: never deferred
  Layout, ///< a word-layout memory accessed off a word, or not matching
  Limit,  ///< the interpreter cannot run it: a length limit, ptr.to_int
          ///< before placement, an unsupported op, a missing input, ...
};

/// `null`, `out-of-bounds`, `use-after-free`, `double-free`, `bad-free`,
/// `layout`, `limit` (`none`).
llvm::StringRef stringifyRunErrorKind(RunErrorKind kind);

/// Null, out-of-bounds, use-after-free, double free, bad free.
bool isMemorySafetyKind(RunErrorKind kind);

/// The value a run read for a sea.nondet or a sea.struct.nondet.
struct NondetRead {
  mlir::Operation *op;
  uint64_t site;
  llvm::StringRef callee;
  llvm::APInt value;
  std::string origin;
};

/// The site of a sea.mem.nondet or sea.memhavoc.
uint64_t memSiteOf(mlir::Operation *siteOp);

/// A memory cell the run read from a nondet source (an initial memory or a
/// havocked range).
struct CellRead {
  mlir::Operation *siteOp;
  std::string object; // abstract memories: the object; addr is the offset
  llvm::APInt addr;
  llvm::APInt value;
  std::string origin;
};

/// A file position and the call sites it was inlined at, innermost first
/// (positionsOf).
using SourcePositions = llvm::SmallVector<mlir::FileLineColLoc, 2>;

/// An object of a run: allocated (abstract programs) or placed (a
/// sea.segment). Index 0 is the null object of abstract pointers.
struct ObjectInfo {
  std::string name;  // stack.<site>, heap.<site>, global.<name>, ... (#n)
  std::string kind;  // stack, heap, global, func, null, nondet
  std::string label; // the producer's label, if any
  std::optional<mlir::FileLineColLoc> loc; // the allocation site
  /// the call sites the allocation was inlined at (inlinedAtOf)
  llvm::SmallVector<mlir::FileLineColLoc> inlinedAt;
  std::string locName; // its location's name, if any
  llvm::APInt size;
  std::optional<llvm::APInt> base; // placed objects
  uint64_t align = 1;
  bool alive = true;
};

/// One line of counterexample print output (docs/design/Print.md).
struct PrintRecord {
  enum Kind {
    Line,   ///< `print: <path>.<text>`
    Object, ///< `print.object: <text>` (first reference to an object)
    Scope,  ///< a sea.print.scope (tree output only)
    Note,   ///< `print.note: <text>` (unmatched sea.print.end)
  };
  Kind kind;
  unsigned depth = 0; // scope depth
  std::string path;   // dotted scope tags, empty at top level
  std::string text;
  mlir::Operation *op = nullptr;
  /// Structured form (jsonl output, sea/Support/Output.h): `name` the
  /// left side of a Line (`tag`, `tag.leaf`, `tag[3]`), `jkind` the record
  /// kind when not the default of `kind` (`print.mem`, `print.mem.row`),
  /// `fields` the value's fields (type, value, obj, off, ...).
  std::string name;
  std::string jkind;
  out::Fields fields;
};

struct RunResult {
  RunStatus status = RunStatus::Error;
  /// The op that ended the run (halt, the failing assert/assume, ...).
  mlir::Operation *at = nullptr;
  std::string message;
  llvm::SmallVector<NondetRead> reads;
  llvm::SmallVector<CellRead> cellReads;
  uint64_t steps = 0;
  /// Where the run failed, in source terms (`assert-at`). For a failing
  /// assert whose condition is computed (not a constant, not a block
  /// argument): the location of the op that computed it, or of its first
  /// false conjunct through a chain of sea.and (the original assert's
  /// condition under sea-merge-asserts). Otherwise (SeaHorn merges all
  /// assertions into one `sea.assert false`, so `at` does not say which
  /// failed): the location of the last executed op before `at` that has a
  /// file location (a branch, or the computation of the condition); source
  /// locations win over named locations, which win over bare positions in
  /// the .mlir file (ops the input left unlocated); print ops and the
  /// branches sea-merge-asserts made of an assert do not count.
  std::optional<mlir::Location> failLoc;
  /// File locations of the most recent executed branches (sea.br /
  /// sea.cond_br), distinct, most recent first (at most kBranchPathLength;
  /// source locations only if there are any). Each is positionsOf the
  /// branch's location: its position, then the call sites it was inlined
  /// at.
  llvm::SmallVector<SourcePositions> branchPath;
  /// The location of the assert that failed: the original assert's (its
  /// whole location, a name-only one included), when the run reached `at`
  /// through a branch sea-merge-asserts put in its place
  /// (kMergedAssertLocName), else `at`'s own. Set for AssertFailed.
  std::optional<mlir::Location> assertLoc;
  /// The op that stands for that assert: the branch sea-merge-asserts put
  /// in its place (the one assertLoc comes from), else the failing
  /// sea.assert. It carries the assert's id (kAssertIdAttr), if any. Set
  /// with assertLoc (a MemoryError that reached a failing assert too).
  mlir::Operation *assertOp = nullptr;
  static constexpr unsigned kBranchPathLength = 5;
  /// The output of the print ops the run executed (RunOptions::prints),
  /// and the number of records dropped past RunOptions::printLimit.
  std::vector<PrintRecord> prints;
  uint64_t printsTruncated = 0;
  /// The objects of the run (index 0: the abstract null object).
  std::vector<ObjectInfo> objects;
  /// The kind of the error (status Error; RunErrorKind::Limit unless the
  /// interpreter named another) or of the violation (status MemoryError).
  RunErrorKind errorKind = RunErrorKind::None;
  /// RunOptions::deferMemoryErrors: the memory-safety violations the run
  /// went past, and the first one (op, kind, message). A run with any
  /// ends as MemoryError if it reaches sea.halt or a failing sea.assert
  /// (at = the violation's op), else keeps its status (assume-blocked:
  /// infeasible, not a finding; step-limit, error: not classified).
  uint64_t deferredCount = 0;
  mlir::Operation *deferredAt = nullptr;
  RunErrorKind deferredKind = RunErrorKind::None;
  std::string deferredMessage;
  /// MemoryError: the op that made the violation reachable (the sea.halt or
  /// the failing sea.assert), and which of the two it was. A failing
  /// assert's location is assertLoc, as for AssertFailed.
  mlir::Operation *reachedAt = nullptr;
  RunStatus reachedStatus = RunStatus::Halted;
  /// The sea.load ops that read an earlier memory version (warning
  /// interp.load-earlier-version), each once, in the order first executed:
  /// the in-place interpreter read the current contents, right only if the
  /// cell was not clobbered since (Mem-SSA's no-clobber claim, Memory.md).
  llvm::SmallVector<mlir::Operation *> earlierVersionLoads;
  /// Steps that rested on addr0-is-null (RunOptions::addr0IsNull).
  uint64_t addr0IsNullUses = 0;
};

/// The file location in `loc` (directly, or inside a name / fused /
/// call-site location), if any; of an inlined op's callsite(<callee> at
/// <caller>), the innermost position (the callee's).
std::optional<mlir::FileLineColLoc> fileLocOf(mlir::Location loc);

/// The file positions of `loc`: fileLocOf, then the call sites it was
/// inlined at (a CallSiteLoc's callers, innermost first, outermost last;
/// nested either way: callsite(a at callsite(b at c)) and
/// callsite(callsite(a at b) at c) both give a, b, c). Looks through names
/// and fused locations (their first located element; a fused location's
/// metadata, such as the exporter's `llvm @f %bb #i` link, is never a
/// position). Empty without a file location.
SourcePositions positionsOf(mlir::Location loc);

/// The call sites of `loc` (positionsOf without its first element).
llvm::SmallVector<mlir::FileLineColLoc> inlinedAtOf(mlir::Location loc);

/// The location of the op that ended `result`'s run, as the `at:` line
/// shows it: for a failing assert the original assert's
/// (RunResult::assertLoc), else `at`'s own; none without `at`.
std::optional<mlir::Location> stopLocOf(const RunResult &result);

/// MemoryError: the location of the op that made the violation reachable
/// (a failing assert: RunResult::assertLoc), else none.
std::optional<mlir::Location> reachedLocOf(const RunResult &result);

/// The name of a NameLoc at the top of `loc` (directly, or one of a fused
/// location's; the marker sea-merge-asserts puts on branches is looked
/// through): `claim.x` of loc("claim.x"), also of loc("claim.x"("a.py":3:1)).
std::optional<std::string> locNameOf(mlir::Location loc);

/// A location as report lines show it: `<file>:<line>:<col>` (fileLocOf;
/// `-` when there is none), then ` inlined-at=<file:line:col>[,...]` when
/// it was inlined (inlinedAtOf, outermost last), then ` <nameKey>=<name>`
/// when it has a name (locNameOf). Empty when it has neither.
std::string formatLoc(mlir::Location loc, llvm::StringRef nameKey = "name");

/// The jsonl form of formatLoc: field `key` the position, `<key>_inlined`
/// the call sites (an array of strings), `<key>_name` the name, each only
/// if present.
void addLocFields(out::Record &rec, llvm::StringRef key, mlir::Location loc);

/// Prints `<file>:<line>:<col>`.
void printFileLoc(llvm::raw_ostream &os, mlir::FileLineColLoc loc);

/// `<file>:<line>:<col>`.
std::string fileLocText(mlir::FileLineColLoc loc);

/// Prints ` inlined-at=<file:line:col>[,<file:line:col>...]`; nothing for
/// no call sites.
void printInlinedAt(llvm::raw_ostream &os,
                    llvm::ArrayRef<mlir::FileLineColLoc> callSites);

/// Prints a non-empty positionsOf: the position, then printInlinedAt of
/// the rest.
void printPositions(llvm::raw_ostream &os,
                    llvm::ArrayRef<mlir::FileLineColLoc> positions);

/// The jsonl array of call sites (`<key>_inlined`): their fileLocText.
std::string jsonCallSites(llvm::ArrayRef<mlir::FileLineColLoc> callSites);

/// The jsonl fields of RunResult::branchPath (none when empty): `path`,
/// the positions (strings); `path_inlined`, only when some entry was
/// inlined, parallel to `path`: each entry's call sites (an array, empty
/// for one not inlined).
void addPathFields(out::Record &rec,
                   llvm::ArrayRef<SourcePositions> branchPath);

/// Prints the assert-failed location lines of `result` (a run that ended
/// at a failing sea.assert), each `<prefix><key>: <value>`:
///   <prefix>assert-op: <file>:<line>:<col> [inlined-at=..] [name=<name>]
///                                               (RunResult::assertLoc)
///   <prefix>assert-path: <loc> <- <loc> ...      (recent branches; each
///                                   <file:line:col>[ inlined-at=..])
/// Nothing for a missing location.
void printAssertContext(llvm::raw_ostream &os, llvm::StringRef prefix,
                        const RunResult &result);

/// Facts of a program each run needs before it starts (one walk of the
/// program). A caller that runs one program many times (sea-run --fuzz)
/// computes them once and passes them in RunOptions::facts.
struct ProgramFacts {
  bool hasSegments = false;   ///< a placed program: track provenance
  bool hasWeakPrints = false; ///< print ops that name values
};
ProgramFacts programFactsOf(ProgramOp program);

struct RunOptions {
  uint64_t maxSteps = 1'000'000;
  /// programFactsOf(the program), if the caller has it (else each run
  /// walks the program).
  std::optional<ProgramFacts> facts;
  /// Largest memset / memmove length (in cells) the interpreter executes.
  uint64_t maxMemOpCells = 1 << 20;
  /// Called with each value the run computes: op results and block arguments
  /// (bound on entry to their block). Memory versions are not reported.
  std::function<void(mlir::Value, const llvm::APInt &)> onValue;
  /// Called with each op the run is about to execute (after its operands,
  /// before its effect; a branch before its block arguments are bound),
  /// with the run's current value of an integer / address SSA value (none
  /// if it has none). Monitors of the replay (the A-PTRINT check of
  /// sea-vc --ptr-companion=check) observe the run through it.
  std::function<void(
      mlir::Operation &,
      llvm::function_ref<std::optional<llvm::APInt>(mlir::Value)>)>
      onOp;
  /// Execute print ops (else they are skipped).
  bool prints = true;
  /// Warn interp.load-earlier-version at each load of an earlier memory
  /// version (a Mem-SSA no-clobber claim). Off when replay validation
  /// checks every loaded value (sea-vc): a broken claim is then a mismatch
  /// (replay: no-clobber-violated), and the loads are counted instead.
  bool warnEarlierVersion = true;
  /// At most this many print / object / note records (0: no limit).
  uint64_t printLimit = 1000;
  /// Called with each pointer (!sea.ptr) and address (!sea.addr) value the
  /// run computes, once its provenance is known: `bits` is the address
  /// (null for an abstract pointer), `token` its rendering (`&heap.3[8]`).
  std::function<void(mlir::Value, const llvm::APInt *bits,
                     const std::string &token)>
      onPointer;
  /// Coverage feedback (sea-run --fuzz): called on entry to each block,
  /// with the block the run came from (null for the entry block). Unset,
  /// it costs one test per block entered.
  std::function<void(mlir::Block *from, mlir::Block *to)> onBlock;
  /// Called after each sea.assume whose condition held (unset: one test
  /// per assume).
  std::function<void(AssumeOp)> onAssume;
  /// Defer memory-safety violations (sea-run --defer-memory-errors): record
  /// the first (and count them) and go on, so a later assume can show the
  /// run infeasible. A load past a violation reads 0 (a pointer: null); a
  /// store, memset, memmove, memhavoc or free with a violation does nothing.
  /// Classification: RunResult::deferredCount. Layout errors and limits
  /// still stop the run.
  bool deferMemoryErrors = false;
  /// The named assumption addr0-is-null (on by default; sea-run / sea-vc
  /// --addr0-is-null=false: off): address 0 is the null pointer. Almost
  /// always true; false in some embedded and kernel environments. Under it
  /// an abstract program recovers null where only bytes or an address are
  /// known: all-zero integer bytes loaded as a pointer are null, a null
  /// pointer's bytes loaded as an integer are 0, ptr.to_int of null is 0.
  /// Each such step counts (RunResult::addr0IsNullUses). Off: they stop
  /// the run, as without it.
  bool addr0IsNull = true;
};

//===- Counterexample printing (docs/design/Print.md) ---------------------===//

/// Renders an integer in a print format (auto, u, s, x, b, c, bits), with
/// its MLIR type suffix: `5 : ui32`, `-1 : si32`, `0xff : i8`.
std::string formatPrintInt(const llvm::APInt &v, llvm::StringRef fmt);

/// The pointer token of offset `off` in `o`: `&heap.3[8]`, `&stack.5[16]
/// end`, `&stack.5[-4] oob`, `null`, `&null[8]`, `&func.foo`, `... freed`.
std::string pointerToken(const ObjectInfo &o, const llvm::APInt &off);

/// An address without provenance: `0x1000`, plus `~heap.3[8]` when it
/// falls inside a placed object (a hint, not provenance); 0 is `null`.
std::string addressToken(llvm::ArrayRef<ObjectInfo> objects,
                         const llvm::APInt &addr);

/// The object a byte address lies in, as `&heap.3[8]` (empty if none):
/// placed objects only. For memory locations (a cell's address), not
/// pointer values.
std::string locateAddress(llvm::ArrayRef<ObjectInfo> objects,
                          const llvm::APInt &addr);
/// The jsonl fields of locateAddress: obj (the object id) and off (the
/// byte offset); empty if none.
out::Fields locateAddressFields(llvm::ArrayRef<ObjectInfo> objects,
                                const llvm::APInt &addr);

/// The `print.object:` text of an object.
std::string formatObject(const ObjectInfo &o);
/// The structured fields of an object (jsonl `print.object` record).
out::Fields objectFields(const ObjectInfo &o);
/// The fields of an integer printed in format `fmt`: type, fmt, value.
out::Fields printIntFields(const llvm::APInt &v, llvm::StringRef fmt);
/// The fields of pointer token pointerToken(o, off): obj, off, state.
out::Fields pointerFields(const ObjectInfo &o, const llvm::APInt &off);

/// One byte of a hex dump: its value, or none (unreadable: `??`);
/// `dflt`: the model leaves it unconstrained (`..`).
struct DumpByte {
  std::optional<uint8_t> value;
  bool dflt = false;
};

/// SeaHorn's HexDump layout (lib/seahorn/Smt/HexDump.cc, LLVM
/// format_bytes_with_ascii), on concrete bytes: rows of 16 bytes
/// `+<off>: xx xx .. |ascii|`, a lone `*` for a run of rows equal to the
/// one before (hexdump(1) squeeze; the last row is always shown).
llvm::SmallVector<std::string> hexDumpRows(llvm::ArrayRef<DumpByte> bytes);

enum class PrintMode { Lines, Tree, Off };

/// Prints the records of a run, one `key: value` line each: `print:`,
/// `print.object:`, `print.note:`, then `print.truncated: <n>` if any were
/// dropped. Tree: the same lines, indented by scope, with a line per scope.
/// `locs`: append `at=<file>:<line>:<col>` of the print op. jsonl: one
/// record per line (kinds print, print.scope, print.mem, print.mem.row,
/// print.object, print.note, print.truncated).
void printPrintRecords(out::Emitter &out, const RunResult &result,
                       PrintMode mode, bool locs);

RunResult interpret(ProgramOp program, NondetSource &nondets,
                    const RunOptions &options = {});

/// Did the run stop at a memory-safety error (status Error, or a deferred
/// violation that was reached: MemoryError): an access out of its object's
/// bounds, through the null pointer, or to a freed object, a double free, a
/// free of a pointer that is not a heap object's start (abstract programs
/// only: a placed program has no objects to check against)?
bool isMemorySafetyError(const RunResult &result);

/// Did the run stop (status Error) at an access outside every object: null,
/// out of bounds, or to a freed object (the accesses A-INB rules out; not a
/// bad free)?
bool isOutOfObjectAccess(const RunResult &result);

} // namespace sea

#endif // SEA_INTERP_INTERPRETER_H
