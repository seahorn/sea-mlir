// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- sea-vc.cpp - verify a sea program with a VC --------------*- C++ -*-===//
//
// Encodes a sea program (in BMC form) as a SeaHorn-style VC, solves it, and on
// sat replays the model in the interpreter with validation.
//
//===----------------------------------------------------------------------===//

#include "ArithStats.h"
#include "EqAudit.h"
#include "sea/Dialect/Sea/SeaDialect.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Expr/Ackermann.hh"
#include "sea/Expr/ExprAPInt.hh"
#include "sea/Expr/InitialWords.hh"
#include "sea/Expr/ReadCells.hh"
#include "sea/Expr/ReadTable.hh"
#include "sea/Expr/SeaMRules.hh"
#include "sea/Expr/Smt/Yices2SolverImpl.hh"
#ifdef WITH_BITWUZLA
#include "sea/Expr/Smt/BitwuzlaSolverImpl.hh"
#endif
#include "sea/Expr/Smt/EZ3.hh"
#include "sea/Expr/Smt/Z3.hh"
#include "sea/Expr/Smt/Z3SolverImpl.hh"
#include "sea/Interp/ParseInput.h"
#include "sea/Support/Diag.h"
#include "sea/Support/Log.h"
#include "sea/Support/Output.h"
#include "sea/Support/ProcMem.h"
#include "sea/Support/Version.h"
#include "sea/Transforms/Passes.h"
#include "sea/Transforms/RangeFold.h"

#include "mlir/Pass/PassManager.h"
#include "sea/VC/Reach.h"
#include "sea/VC/Replay.h"
#include "sea/VC/Split.h"
#include "sea/VC/VCGen.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>

#include "mlir/IR/AsmState.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Support/Timing.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using namespace llvm;

static cl::opt<std::string>
    inputFilename(cl::Positional, cl::desc("<input .mlir>"), cl::init("-"));

static cl::opt<std::string>
    programName("program", cl::desc("Program to check (default: the only one)"),
                cl::value_desc("name"));

enum class Backend { Z3, Yices, Bitwuzla };
static cl::opt<Backend>
    backend("solver", cl::desc("Solver"), cl::init(Backend::Z3),
            cl::values(clEnumValN(Backend::Z3, "z3", "z3 (default)"),
                       clEnumValN(Backend::Yices, "yices", "Yices 2"),
                       clEnumValN(Backend::Bitwuzla, "bitwuzla",
                                  "Bitwuzla (if built with BITWUZLA_ROOT)")));

static cl::opt<bool> smt2("smt2",
                          cl::desc("Print the query as SMT-LIB and exit"),
                          cl::init(false));

static cl::opt<bool>
    lower("lower",
          cl::desc("Run the VC lowering pipeline first (sea-opt "
                   "--sea-vc-lower; lib/Transforms/Passes.cpp): "
                   "sea-apply-tracking, sccp, sea-scalarize-structs; for "
                   "abstract pointers sea-drop-fresh-memhavoc, "
                   "sea-lower-tracking, sea-lower-fat-pointers, "
                   "sea-place-static; sea-widen-memory (--widen-memory=k); "
                   "sea-expand-memset (not with --seam); sea-merge-asserts, "
                   "sea-split-wide-access (wide accesses kept whole with "
                   "--seam --seam-wide), sea-expand-intrinsics, "
                   "sea-split-critical-edges; cse after sccp and "
                   "canonicalize + cse before splitting edges "
                   "(--lower-opt)"),
          cl::init(false));

static cl::opt<bool> lowerOpt(
    "lower-opt",
    cl::desc("With --lower: upstream cse after sccp, canonicalize + cse "
             "after the lowerings (merge equal values, fold (constant "
             "assumes / asserts, straight blocks, a load of a just-stored "
             "value), erase dead ops and unreachable blocks; default on; "
             "=false keeps every op)"),
    cl::init(true));

static cl::opt<sea::RangeFoldMode> rangeFoldOpt(
    "range-fold",
    cl::desc("With --lower: sea-range-fold before the final canonicalize: a "
             "sea.icmp, i1 and / or / xor or select condition that the "
             "integer ranges settle, with the simple bounds of dominating "
             "sea.assume ops, is folded (docs/design/RangeFold.md); prints "
             "vc.range-fold. Untuned default: off"),
    cl::values(clEnumValN(sea::RangeFoldMode::Off, "off", "not run (default)"),
               clEnumValN(sea::RangeFoldMode::Fold, "on", "fold"),
               clEnumValN(sea::RangeFoldMode::Check, "check",
                          "the paranoid job: assert every fold instead, "
                          "drop the program's asserts; unsat = every fold "
                          "valid")),
    cl::init(sea::RangeFoldMode::Off));

static cl::opt<unsigned> expandMemMoveOpt(
    "expand-memmove",
    cl::desc("With --lower: a sea.memmove of a constant length <= K index "
             "units (bytes for byte-addressed memories) becomes loads of "
             "every source cell, then stores in address order "
             "(sea-expand-small-memmove; exact, overlap included; K <= "
             "1024). Every memmove is kept with --ptr-companion; a "
             "fat-pointer companion memmove is expanded only with its data "
             "memmove. "
             "For --seam, whose memmove term (mcopy) leaves Ackermann "
             "residue when the range test is undecided; prints "
             "vc.expand-memmove. Untuned default: 0 (off)"),
    cl::value_desc("K"), cl::init(0));

static cl::opt<unsigned> boxAllExpandOpt(
    "box-all-expand",
    cl::desc("With --lower: a sea.box.all whose upper bounds are constants "
             "and whose trip count (the product over its dimensions) is at "
             "most K is expanded: the body at every index, and-reduced "
             "(exact); larger or symbolic boxes get one nondet index. The "
             "polarity rule is the same for both. 0: never. Prints "
             "vc.box-all expanded= (docs/design/BoxAll.md). Untuned default"),
    cl::value_desc("K"), cl::init(sea::kBoxAllExpandDefault));

static cl::list<std::string> lowerDisable(
    "lower-disable",
    cl::desc("With --lower: leave these optimization passes out of the "
             "lowering (sccp, cse, canonicalize; "
             "comma-separated): a lowering with and without one, compared "
             "by utils/cover-diff.py, validates it"),
    cl::value_desc("pass,..."), cl::CommaSeparated);

static cl::opt<std::string> placeOut(
    "place-out",
    cl::desc("With --lower: write the placement of the program's objects "
             "(sea-place-static place-out) to this file, one object per "
             "line, for another lowering's --place-in"),
    cl::value_desc("file"));

static cl::opt<std::string> printLowered(
    "print-lowered",
    cl::desc("Write the program the VC encodes (after the lowering and "
             "every other pass of sea-vc) to this file, with its locations, "
             "named as the VC names it: the names of --split / "
             "--split-leaves selectors and of the split.* and other report "
             "lines (a nondet result is nd.<site> in the VC). The run goes "
             "on (--vcgen-only: no solver)"),
    cl::value_desc("file"));

static cl::opt<std::string> placeIn(
    "place-in",
    cl::desc("With --lower: place the objects at the bases of this file "
             "(written by --place-out; sea-place-static place-in), checked "
             "as placement does; an object the file does not name is an "
             "error (place.in-unplaced)"),
    cl::value_desc("file"));

static cl::opt<bool> addr0IsNull(
    "addr0-is-null",
    cl::desc("The named assumption addr0-is-null (default on) in the "
             "replays only: address 0 is the null pointer (zero bytes loaded "
             "as a pointer are null, null's bytes as an integer 0, ptr.to_int "
             "of null 0; `assume.addr0-is-null: uses=<n>` when relied on); "
             "=false: such a replay stops. The VC identifies null with "
             "address 0 either way (placement; `assume.addr0-is-null: vc`, "
             "warning vc.addr0-is-null-off)"),
    cl::init(true));

enum class MemhavocNull { Noop, Error };
static cl::opt<MemhavocNull> memhavocNull(
    "memhavoc-null",
    cl::desc("sea.memhavoc of the null pointer in an abstract program, in "
             "the lowering (sea-guard-null-memhavoc) and the replays: "
             "noop (default; an open semantics decision, counted as "
             "`assume.memhavoc-null-noop: uses=<n>`, warning "
             "interp.memhavoc-null-noop) or error (a memory-safety error, "
             "as any access through null)"),
    cl::values(clEnumValN(MemhavocNull::Noop, "noop", "a no-op"),
               clEnumValN(MemhavocNull::Error, "error",
                          "a memory-safety error")),
    cl::init(MemhavocNull::Noop));

static cl::opt<bool> replayOriginalOpt(
    "replay-original",
    cl::desc("With --lower (or --sink-assumes), on sat: also run the input "
             "program as it was before any pass on the model's inputs "
             "(nondet sites; cells <object>+<offset> at the placed bases) "
             "and check that it fails the same assertion "
             "(replay.original:; exit 6 if not: a lowering bug)"),
    cl::init(false));

static cl::opt<unsigned> widenMemory(
    "widen-memory",
    cl::desc("With --lower: give byte memories whose accesses are all "
             "aligned whole k-byte words k-byte cells (sea-widen-memory)"),
    cl::init(0));

static cl::opt<sea::JoinEncoding> joinEncoding(
    "vc-join", cl::desc("How joins define block arguments"),
    cl::values(clEnumValN(sea::JoinEncoding::Ite, "ite",
                          "ite over the incoming edges (default)"),
               clEnumValN(sea::JoinEncoding::Eq, "eq",
                          "fresh constant, guarded equality per edge")),
    cl::init(sea::JoinEncoding::Ite));

static cl::opt<bool> vcJoinCases(
    "vc-join-cases",
    cl::desc("Experimental, default off: build each ite join (block "
             "arguments, pointer companions) as a cases term, expanded at "
             "once to the same ite chain (the query does not change), and "
             "print its `cases.<name>: <n>` counters"),
    cl::init(false));

static cl::opt<bool> vcCasesReorder(
    "vc-cases-reorder",
    cl::desc("Experimental, default off, needs --vc-join-cases: merge a pair "
             "of a cases join whose value is an earlier pair's value into "
             "that pair, conjoining the negations of the conditions it skips "
             "(first-match semantics; changes the query)"),
    cl::init(false));

static cl::opt<bool> vcCasesDropDefault(
    "vc-cases-drop-default",
    cl::desc("Experimental, default off, needs --vc-join-cases: drop a pair "
             "of a cases join whose value is the default, conjoining the "
             "negation of its condition into the later pairs (changes the "
             "query)"),
    cl::init(false));

static cl::opt<bool> seamWide(
    "seam-wide",
    cl::desc("With --seam: a load / store of a byte memory wider than a byte "
             "is one width term (wselect / wstore), not split into bytes "
             "(docs/design/SeaM-mixed-width.md); the boundary expands what "
             "is left. Experimental, default off"),
    cl::init(false));

enum class WordsMode { Off, Auto, All };
static cl::opt<WordsMode> seamWords(
    "seam-words",
    cl::desc("With --seam: initial byte memories (nd.<site>, memhavoc "
             "arrays) only read in the query are re-encoded as 8-byte word "
             "arrays at the solver boundary, exactly "
             "(docs/design/SeaM-initial-words.md). Experimental"),
    cl::values(clEnumValN(WordsMode::Off, "off", "never (default)"),
               clEnumValN(WordsMode::Auto, "auto",
                          "arrays whose aligned 8-byte reads cover the bytes "
                          "read otherwise"),
               clEnumValN(WordsMode::All, "all", "every eligible array")),
    cl::init(WordsMode::Off));

static cl::opt<std::string> ackDumpPairs(
    "ack-dump-pairs",
    cl::desc("With the Ackermann lowering on: write every lowered read "
             "(`read <array> <k> <index>`) and every constrained pair "
             "(`pair <array> <k> <l>`) to <file> (pair-class census)"),
    cl::value_desc("file"));

static cl::opt<bool> wordsCheck(
    "words-check",
    cl::desc("With --seam-words: check the word query against the byte query "
             "(z3: the defining equations A_w[j] = concat(A[j+7..j]) of every "
             "word index read, and the two queries differ: expected unsat). "
             "Exit 2 if not"),
    cl::init(false));
static cl::opt<unsigned> wordsCheckTimeout(
    "words-check-timeout",
    cl::desc("Seconds for --words-check (0: none; default 60)"), cl::init(60));

static cl::opt<bool> checkAlign(
    "check-align",
    cl::desc("With --lower: the A-ALIGN discharge job: assert every `align` "
             "promise of a load / store / memset / memmove (after placement, "
             "before widening), drop the program's own asserts; unsat = "
             "every promise holds (within the bound)"),
    cl::init(false));

static cl::opt<bool> checkPtrIndex(
    "check-ptr-index",
    cl::desc("With --lower: the paranoid job of the trusted GEP flags: assert "
             "the no-wrap promise (P1-P3) of every inbounds / nusw / nuw "
             "sea.ptr.index at the accesses that use it, drop the program's "
             "own asserts; unsat = every used promise holds (within the "
             "bound). Not with --check-align (docs/design/PtrIndex.md "
             "section 6)"),
    cl::init(false));

static cl::opt<bool> vcTypecheck(
    "vc-typecheck",
    cl::desc("Type-check the whole query before the solver (Expr's "
             "TypeChecker): an ill-typed term is the error vc.ill-typed, a "
             "bug in the VC (PtrSort rev 5, S0: the check the pointer sort "
             "relies on). Default on: 2.3 s over vcc-pi2's 228 queries, "
             "against ~19 s of VC time; --vc-typecheck=false off"),
    cl::init(true));

enum class PtrMode { Auto, Opaque, Address };
static cl::opt<PtrMode> ptrMode(
    "ptr-mode",
    cl::desc("The pointer model (docs/design/PtrSort.md rev 5): opaque "
             "pointers (provenance + offset + meta; no int-to-ptr) or "
             "addresses (pointers may be packed into integers). Classified "
             "on the input program and reported (vc.ptr-mode); the encoding "
             "of addr.from_int reads it (PTR_FROM_INT in address mode)"),
    cl::values(clEnumValN(PtrMode::Auto, "auto",
                          "address mode iff the program has an int-to-ptr "
                          "(sea.ptr.from_int / sea.addr.from_int) (default)"),
               clEnumValN(PtrMode::Opaque, "opaque",
                          "opaque pointers; an int-to-ptr is the error "
                          "vc.int-to-ptr"),
               clEnumValN(PtrMode::Address, "address", "address mode")),
    cl::init(PtrMode::Auto));

static cl::opt<bool> vcTypecheckPlant(
    "vc-typecheck-plant", cl::Hidden,
    cl::desc("Test hook: conjoin an ill-typed term to the query (an 8-bit "
             "and a 16-bit numeral compared), so a test shows --vc-typecheck "
             "catches it"),
    cl::init(false));

static cl::opt<bool> checkPtrIndexAssumeAsserts(
    "check-ptr-index-assume-asserts", cl::Hidden,
    cl::desc("Diagnostic for --check-ptr-index, never a discharge: the "
             "program's asserts become assumes (circular with the main "
             "job)"),
    cl::init(false));

static cl::opt<bool> widenMemoryReport(
    "widen-memory-report",
    cl::desc("With --widen-memory: a remark per memory, widened or kept as "
             "bytes and why (the first reason found)"),
    cl::init(false));

static cl::opt<std::string> z3Tactic(
    "z3-tactic",
    cl::desc("z3's default tactic for the solver (global parameter "
             "tactic.default_tactic, as SeaHorn's --horn-bmc-tactic; e.g. "
             "smtfd, sat; a tactic ending in `sat)` or `sat` also sets "
             "sat.euf=true, as SeaHorn). Applies to every z3 solver of the "
             "run. default: z3's own"),
    cl::init("default"));

static cl::list<std::string>
    z3Params("z3-param",
             cl::desc("A global z3 parameter, key=value (repeatable, or comma "
                      "separated), set before any z3 solver is made"),
             cl::CommaSeparated, cl::value_desc("key=value"));

static cl::opt<bool> sinkAssumes(
    "sink-assumes",
    cl::desc("Before the VC: replace every sea.assume by one assume of the "
             "conjunction of the assumes on the path, right before the final "
             "assert (sea-sink-assumes, SeaHorn's UnifyAssumes; exact). A "
             "solver heuristic tradeoff, not tuned (on TX-PARSE: Yices' median "
             "over seeds 2-3x faster, z3 mixed); prints vc.sink-assumes: <n "
             "moved>. default: off"),
    cl::init(false));

static cl::list<std::string> yicesParams(
    "yices-param",
    cl::desc("With --solver=yices: a search parameter of every Yices check, "
             "key=value (repeatable, or comma separated; yices_set_param, "
             "e.g. random-seed=7 for seed variance). Printed as "
             "solver.yices-param lines. default: Yices' own"),
    cl::CommaSeparated, cl::value_desc("key=value"));

static cl::list<std::string> yicesConfig(
    "yices-config",
    cl::desc("With --solver=yices: a context configuration key of every "
             "Yices context, key=value (repeatable, or comma separated; "
             "yices_set_config, e.g. solver-type, mode, array-solver). "
             "default: Yices' general context (all theories, push-pop)"),
    cl::CommaSeparated, cl::value_desc("key=value"));

static cl::opt<std::string> yicesLogic(
    "yices-logic",
    cl::desc("With --solver=yices: configure every Yices context for this "
             "SMT-LIB logic first (yices_default_config_for_logic, e.g. "
             "QF_BV: the bit-vector solver without the egraph; a query "
             "outside the logic is an error). default: none (Yices' general "
             "context)"),
    cl::init(""));

enum class MemEncoding { Array, Lambda };
static cl::opt<MemEncoding> memEncoding(
    "mem", cl::desc("Memory encoding of the VC"),
    cl::values(clEnumValN(MemEncoding::Array, "array",
                          "SMT arrays (default; --seam rewrites them)"),
               clEnumValN(MemEncoding::Lambda, "lambda",
                          "unary functions, SeaHorn's lambda encoding "
                          "(docs/design/Lambda.md): z3 only, "
                          "no assumption")),
    cl::init(MemEncoding::Array));

enum class LambdaSimplify { Z3, None };
static cl::opt<LambdaSimplify> lambdaSimplify(
    "lambda-simplify",
    cl::desc("--mem=lambda: the simplifier applied to every value written to "
             "a register"),
    cl::values(clEnumValN(LambdaSimplify::Z3, "z3",
                          "z3's simplifier, one cache per VC (SeaHorn's "
                          "--horn-bv2-simplify; default)"),
               clEnumValN(LambdaSimplify::None, "none", "none")),
    cl::init(LambdaSimplify::Z3));

static cl::opt<unsigned> lambdaSimplifyEvery(
    "lambda-simplify-every",
    cl::desc("--mem=lambda: simplify every k-th write only (default 1: "
             "every write)"),
    cl::init(1));

static cl::opt<bool> lambdaCheckScopes(
    "lambda-check-scopes",
    cl::desc("--mem=lambda: check that no value written to a register has a "
             "free bound variable (error vc.lambda-free-bvar; a debug "
             "check, a walk per write)"),
    cl::init(false));

static cl::opt<bool> lambdaFinalSimplify(
    "lambda-final-simplify",
    cl::desc("--mem=lambda: also simplify the whole query with z3 before "
             "solving (a separate step from z3's own preprocessing)"),
    cl::init(false));

static cl::opt<bool> seam(
    "seam",
    cl::desc("SeaM (docs/design/SeaM.md): encode each object base "
             "(sea.segment) as a named constant obj.<name> with a side "
             "equality to its address, and rewrite every load and store as "
             "it is encoded (object-aware read-over-write, pointer "
             "normalization, ite and Boolean simplification). Requires "
             "--vc-join=ite"),
    cl::init(false));

static cl::opt<bool> simplifyOpt(
    "simplify",
    cl::desc("Normalize every term of the VC (side conditions and query) with "
             "the bit-vector and Boolean rules of include/sea/Expr/BvRules.hh "
             "(extract / concat, linear equalities, argument order of bvadd / "
             "bvmul / bvand / bvor / bvxor / and / or, Boolean ite), each "
             "valid over plain bit-vectors. Not with --seam (it has this on by "
             "default; "
             "--seam-simplify=0 turns it off)"),
    cl::init(false));

static cl::opt<bool> seamSimplify(
    "seam-simplify",
    cl::desc("With --seam: the --simplify rules (default on). "
             "--seam-simplify=0 keeps the rules of the memory terms only"),
    cl::init(true));

static cl::opt<bool> ackStride(
    "ack-stride",
    cl::desc("The stride rule in the Ackermann lowering (with --seam): two "
             "reads whose addresses are sea.ptr.index nodes with a promise "
             "(inbounds / nusw / nuw, trusted), over one base, whose offsets "
             "can never be equal as integers (the gcd of the strides does not "
             "divide the constant difference) get no constraint "
             "(docs/design/PtrIndex.md section 5); counted "
             "vc.ackermann-stride-pairs; default on, --ack-stride=false off"),
    cl::init(true));

static cl::opt<bool> seamStride(
    "seam-stride",
    cl::desc("R-STRIDE in SeaM (with --seam): a store-map entry b + o is "
             "skipped for a read at sea.ptr.index nodes with a promise over b "
             "whose offsets never reach o (docs/design/PtrIndex.md section "
             "0c). Uses the promises (premise.ptr-index) and, for a node "
             "with a byte offset K != 0, A-INB (only for b + size - 2^n <= K "
             "<= b, where A-INB excludes a wrap); --seam-check checks the "
             "firings under them"),
    cl::init(false));

enum class AckMode { Auto, On, Off };
static cl::opt<AckMode> ackermannize(
    "ackermannize",
    cl::desc("Lower reads of read-only array constants (initial memories, "
             "memhavoc arrays) to fresh constants with functional-consistency "
             "constraints, after the solver boundary "
             "(docs/design/Ackermann.md)"),
    cl::values(clEnumValN(AckMode::Auto, "auto",
                          "on when the query has no store, except with "
                          "--solver=bitwuzla (default)"),
               clEnumValN(AckMode::On, "on",
                          "always (arrays used other than by select are kept)"),
               clEnumValN(AckMode::Off, "off", "never")),
    cl::init(AckMode::Auto));

static cl::opt<bool> dagStats(
    "dag-stats",
    cl::desc("Print the size of the query as a DAG (vc.dag-size) and the "
             "distinct DAG nodes per operator (vc.dag-op: <op> <n>)"),
    cl::init(false));

static cl::opt<bool> arithStats(
    "arith-stats",
    cl::desc("Print the multiplications, divisions and remainders of the "
             "query the solver gets, by operator, width and operand kind "
             "(numeral, ite of numerals, ite with symbolic leaves, two "
             "non-constant terms), with the distinct ite conditions, in "
             "total (arith.total, arith.op) and per source location of the "
             "sea op that made them (arith.loc; nodes made or rewritten by a "
             "later stage: unattributed). Works with --vcgen-only"),
    cl::init(false));
static cl::opt<bool> iteStats(
    "ite-stats",
    cl::desc("Report the values whose VC terms are ite trees (memories, "
             "addresses, integers feeding mul/div/rem) per source location, "
             "with leaves and conditions, and the loads at non-ground "
             "addresses by kind and location (ite.* lines; also with "
             "--vcgen-only; not with a split)"),
    cl::init(false));
static cl::opt<unsigned> iteStatsMax(
    "ite-stats-max",
    cl::desc("--ite-stats: the most ite.site lines per kind and ite.load-site "
             "lines (default 40; 0: all); the summary lines count the rest"),
    cl::init(40));

static cl::opt<bool> seamStats(
    "seam-stats",
    cl::desc("With --seam or --simplify: print the rewriter's counters as "
             "`seam.<name>: <n>` lines (stderr with --smt2, else stdout)"),
    cl::init(false));

static cl::opt<bool> seamInb(
    "seam-inb",
    cl::desc("With --seam: use the assumption A-INB (pointers into different "
             "objects are different). --seam-inb=0 uses no assumption: the "
             "rewriting is valid over plain bit-vectors and arrays"),
    cl::init(true));

static cl::opt<bool> seamPruneSize0(
    "seam-prune-size0",
    cl::desc(
        "With --seam: rule addr.prune-size0 (docs/design/SeaM-provenance.md, "
        "step 0; uses A-INB): drop the ite leaves of an access address "
        "(load, store, memory-operation range) that point into an object of "
        "size 0, such as null (default on; --seam-prune-size0=0 off)"),
    cl::init(true));

static cl::opt<bool> seamLayout(
    "seam-layout",
    cl::desc("With --seam and without A-INB (--seam-inb=0, or the "
             "vc.seam-inb-violated fallback): decide the address compares of "
             "accesses and memop range tests between two placed objects at "
             "numeral offsets by their placed numerals (cmp.layout, "
             "mem.in-layout; uses: LAYOUT, a fact of the VC). Default on; "
             "never for program compares"),
    cl::init(true));

static cl::opt<bool> seamRowExpand(
    "seam-row-expand",
    cl::desc("With --seam: expand a read over a store whose address compare is "
             "unknown into ite(i = j, v, select(a, j)) (default). "
             "--seam-row-expand=0 keeps such a select for the solver (smaller "
             "queries when most compares stay unknown; ARM, --seam-arm, "
             "decides some)"),
    cl::init(true));

static cl::opt<bool> seamRowGroup(
    "seam-row-group",
    cl::desc("With --seam: an expanded read over consecutive undecided "
             "writes of one value is one ite over the disjunction of their "
             "compares, built at once (row.store-group / row.smap-group; "
             "exact; linear in the chain, where merging one ite per write "
             "was quadratic). Default on; =0 for comparison"),
    cl::init(true));

static cl::opt<bool> seamCheckInbPremise(
    "seam-check-inb-premise",
    cl::desc("With --seam: check that every cmp.diff-base decision's two "
             "addresses lie in accesses whose A-INB condition the run "
             "recorded (counters premise.inb-checked / "
             "premise.inb-unrecorded; warning vc.inb-premise-unrecorded). "
             "Default off: the matcher is still too narrow (keyed companions, "
             "other normal forms of one address)"),
    cl::init(false));

static cl::opt<bool> seamPullIndex(
    "seam-pull-index",
    cl::desc("With --seam: pull a PTR_INDEX node through its first ite, the "
             "base or an index of any stride, within the leaf bound "
             "(PtrSort S4; counter ptrindex.pull-ite-index). "
             "Off: the parity rule only (the base ite, a stride-1 index "
             "ite)"),
    cl::init(false));

static cl::opt<unsigned> seamResolveIteNodes(
    "seam-resolve-ite-nodes",
    cl::desc("With --seam: resolve a loaded pointer through an ite of "
             "loaded values with at most N distinct ite nodes (a memoized "
             "map over the DAG; PtrSort S4, counter "
             "ptr.load-resolve-ite-dag). 0 (default): the leaf bound "
             "(at most 4 tree leaves) only"),
    cl::init(0));

static cl::opt<unsigned> seamPullConcat(
    "seam-pull-concat",
    cl::desc("With --seam: resolve a split pointer load (a concat of bytes) "
             "by pulling an ite condition out of the concat: one its ite "
             "bytes share, else the first of its first ite byte (counter "
             "ptr.load-resolve-concat.unshared); at most N pulls per load, "
             "kept only when a side reassembles a stored pointer (PtrSort "
             "S6 probe, counter ptr.load-resolve-concat). 0 (default): off"),
    cl::init(0));

static cl::opt<bool> seamCongruence(
    "seam-congruence",
    cl::desc("With --seam: decide two addresses apart when their "
             "difference is sum c_k x_k + d with 2^t dividing every c_k "
             "and not d (exact, no premise; rule cmp.congruence; e.g. "
             "T + 8i + 1 vs T + 8j). Default off"),
    cl::init(false));

static cl::opt<bool> seamWideIteOpen(
    "seam-wide-ite-open",
    cl::desc("With --seam --seam-wide: W-ITE (a wide read over a write, an "
             "ite address split into leaf pairs) also when a pair is "
             "undecided: that leaf becomes the read at its own address "
             "(counter wide.ite-leaf-open). Default off: W-ITE only when "
             "every pair is decided (an open question, untuned)"),
    cl::init(false));

static cl::opt<unsigned> seamOffsetLeaves(
    "seam-offset-leaves",
    cl::desc("With --seam: the boundary's byte expansion of a width term "
             "at an ite address pushes the byte offset into the ite leaves, "
             "ite(c, x, y) + k -> ite(c, x + k, y + k), when the ite has at "
             "most this many distinct leaves (counters "
             "wide.expanded-ite-offset, wide.expanded-ite-offset-refused over "
             "the cap). 0: off. Default 8 (untuned: the cap of the access "
             "decisions). Only with --seam-wide (without it no width term "
             "reaches the boundary)."),
    cl::init(8));

static cl::opt<bool> seamLeafSplit(
    "seam-leaf-split",
    cl::desc("With --seam: R-LEAVES, a read at an ite address is the ite "
             "of the reads at its leaves when every leaf read is decided "
             "down the whole memory and a write gives every leaf (exact; "
             "counters row.leaf-split, ptr.load-resolve-leaves for a split "
             "pointer load whose bytes then share one ite spine; "
             "row.leaf-split-refused: a leaf reads the memory without "
             "writes). The address may be a node over an ite base; at most "
             "maxDecideLeaves (8) distinct leaves (counter "
             "row.leaf-split-node). Default on"),
    cl::init(true));

static cl::opt<bool> seamLeafSplitPartial(
    "seam-leaf-split-partial",
    cl::desc("With --seam: R-LEAVES also when some leaf reads are "
             "undecided: the decided leaves split, the undecided ones stay "
             "one read at the ite of them (exact; counters "
             "row.leaf-split-partial, row.leaf-split-residual-leaves). "
             "Default off (a tradeoff, untuned)"),
    cl::init(false));

enum class CompanionMode { Off, Exact, Check, Assume };
static cl::opt<CompanionMode> ptrCompanion(
    "ptr-companion",
    cl::desc("With --seam, opaque pointer mode: the pointer companion "
             "(PtrSort S6): an Expr-only memory of the stored pointers "
             "beside each pointer-indexed memory, read at split pointer "
             "loads (counters vc.ptr-companion, vc.ptr-companion-arrays)"),
    cl::values(clEnumValN(CompanionMode::Off, "off", "no companion (default)"),
               clEnumValN(CompanionMode::Exact, "exact",
                          "from_bits(bits) at q is ite(to_bits(c) = bits, c, "
                          "from_bits(bits)), c the companion's cell at q "
                          "(value-exact)"),
               clEnumValN(CompanionMode::Check, "check",
                          "the discharge of A-PTRINT: exact, and the "
                          "property is that every split pointer load reads "
                          "the companion's pointer (the program's "
                          "assertions are not checked): unsat = A-PTRINT "
                          "holds (seam.ptrint: holds / not discharged); "
                          "a sat replay names the failing loads "
                          "(seam.ptrint-violated / -unstored)"),
               clEnumValN(CompanionMode::Assume, "assume",
                          "A-PTRINT: the load is c (a pointer loaded from "
                          "memory is the one last stored there; assumed, "
                          "reported as seam.ptrint; a sat answer is "
                          "rerun with exact)")),
    cl::init(CompanionMode::Off));

static cl::opt<bool> seamArm(
    "seam-arm",
    cl::desc(
        "With --seam: ARM (thesis Ch. 5, include/sea/Expr/AddrRangeMap.hh): "
        "when the syntactic checks of a read-over-write compare leave it "
        "unknown, decide it false if the address range maps of the two "
        "addresses are disjoint (counter arm.skip). Same-base intervals "
        "need no assumption; different bases use A-INB, which is "
        "assumed and reported (seam.inb: assumed ...): an unsat holds "
        "if every access stays in its object. Never used for a compare of the "
        "program. "
        "--seam-arm=0 turns it off"),
    cl::init(true));

static cl::opt<bool> seamStoreMap(
    "seam-store-map",
    cl::desc("With --seam: Store-Map (thesis Ch. 6): compress writes to one "
             "object at numeral offsets into store-map terms (SMAP-NEW, "
             "SMAP-HIT), commute writes to different objects into allocation "
             "order (uses A-INB), and read store-maps by R-HIT / R-MISS / "
             "R-SKIP / R-ABORT. Faster rewriting of long write sequences; the "
             "query is the same. Store-maps are expanded into stores before "
             "the solver sees the query. --seam-store-map=0 keeps linear "
             "chains of stores"),
    cl::init(true));

static cl::opt<std::string> seamTrail(
    "seam-trail",
    cl::desc("With --seam or --simplify: write the rule trail to "
             "<file>, one JSON record per rule firing, and the terms (one "
             "JSON record per node) to <file>.terms"),
    cl::value_desc("file"));

static cl::opt<bool> seamTrailText(
    "seam-trail-text",
    cl::desc("With --seam-trail: also print each firing's lhs and rhs terms "
             "(lhs_text / rhs_text, cut at 2000 characters)"),
    cl::init(false));

static cl::list<std::string> seamTrace(
    "seam-trace",
    cl::desc("With --seam or --simplify: print the firings of these rules to "
             "stderr as they happen (`seam-trace:` lines; `prefix*` matches a "
             "prefix, `all` every firing)"),
    cl::CommaSeparated, cl::value_desc("rule,..."));

static cl::opt<std::string> seamCheck(
    "seam-check",
    cl::desc("With --seam or --simplify: check every rule firing (or N of "
             "them, evenly spaced: --seam-check=N) with the solver: "
             "`premises => lhs = rhs` on its terms. Exit 5 if one "
             "is invalid"),
    cl::ValueOptional, cl::value_desc("N"));

static cl::opt<std::string> eqAudit(
    "eq-audit",
    cl::desc("With --seam: audit the equalities left undecided (Ackermann "
             "pairs, reads over a store or a memory-operation term the rules "
             "left undecided): N of each source (--eq-audit=N, default 200), "
             "each classified by the lowest level of facts that decides it "
             "(L0 none, L1 layout, L2 A-INB; `eq-audit:` lines)"),
    cl::ValueOptional, cl::value_desc("N"));

static cl::opt<unsigned>
    eqAuditTimeout("eq-audit-timeout",
                   cl::desc("Seconds per --eq-audit check (z3; default 5)"),
                   cl::init(5));

static cl::opt<unsigned> eqAuditShapes(
    "eq-audit-shapes",
    cl::desc("--eq-audit: the most frequent shapes printed per level "
             "(default 8)"),
    cl::init(8));

static cl::opt<unsigned> seamCheckTimeout(
    "seam-check-timeout",
    cl::desc("Seconds per --seam-check query (default 10; z3 and Bitwuzla)"),
    cl::init(10));

static cl::opt<std::string> seamPlantBug(
    "seam-plant-bug",
    cl::desc("Test only: make one rule wrong (sub.const, row.store-hit), to "
             "show that --seam-check catches it"),
    cl::Hidden);

static cl::opt<unsigned> seamRuleLimit(
    "seam-rule-limit",
    cl::desc("Test only: stop each rewrite after this many rule calls (the "
             "rewriter's maxRuleCalls; 0 = no limit), to show that the "
             "solver boundary does not depend on the rewriting finishing"),
    cl::init(0), cl::Hidden);

// EXPERIMENT (hidden):
// a guarded read table at the solver boundary (include/sea/Expr/ReadTable.hh)
// in place of the semantics-only translation and the Ackermann lowering.
static cl::opt<std::string> xReadTable(
    "x-read-table",
    cl::desc("Experiment: plain|flat|arrays|objack|mux (needs --seam; "
             "include/sea/Expr/ReadTable.hh)"),
    cl::init(""), cl::Hidden);
static cl::opt<unsigned>
    xReadTableCap("x-read-table-cap",
                  cl::desc("Experiment: leaves per address term"), cl::init(64),
                  cl::Hidden);
static cl::opt<bool> xReadTableInb(
    "x-read-table-inb",
    cl::desc("Experiment: dead branches by A-INB become fresh constants "
             "(default on; off: every branch translated exactly)"),
    cl::init(true), cl::Hidden);
static cl::opt<unsigned> xReadTableMux(
    "x-read-table-mux",
    cl::desc("Experiment: objects of at most this many cells get a mux"),
    cl::init(128), cl::Hidden);
static cl::opt<unsigned> xReadTableCheck(
    "x-read-table-check",
    cl::desc("Experiment: premise checks per class (omitted pairs, dead "
             "branches, mux out-of-range), sampled; 0 = none"),
    cl::init(0), cl::Hidden);
static cl::opt<std::string> xReadTableCheckPremise(
    "x-read-table-check-premise",
    cl::desc("Experiment: the premise of the checks, letters i (A-INB of "
             "every access), l (layout), s (side conditions without the "
             "assumes), a (with the assumes too); default ils. "
             "Fewer letters show which part the checks need"),
    cl::init("ils"), cl::Hidden);
static cl::opt<unsigned> xReadTableCheckTimeout(
    "x-read-table-check-timeout",
    cl::desc("Experiment: seconds per premise check (z3 only)"), cl::init(30),
    cl::Hidden);

static cl::opt<sea::out::Format>
    outFormat("format", cl::desc("Output format of stdout"),
              cl::init(sea::out::Format::Text),
              cl::values(clEnumValN(sea::out::Format::Text, "text",
                                    "`key: value` lines (default)"),
                         clEnumValN(sea::out::Format::Jsonl, "jsonl",
                                    "one JSON object per line (see --agent)")));

/// stdout: every line goes through it (sea/Support/Output.h); set in main
static sea::out::Emitter *gOut = nullptr;
/// text lines of stdout (stat records under --format=jsonl)
static raw_ostream &vout() { return gOut->lines(); }

static cl::opt<bool> brunchStats(
    "brunch-stats",
    cl::desc("Also print BRUNCH_STAT <key> <value> lines (brunch's format)"),
    cl::init(false));

static void brunchStat(raw_ostream &os, const Twine &key, const Twine &value) {
  if (brunchStats)
    os << "BRUNCH_STAT " << key << " " << value << "\n";
}

static cl::opt<unsigned> solveTimeout(
    "timeout",
    cl::desc("Wall-clock limit of the solver's check in seconds (0: none); "
             "VC generation (lowering, encoding, the SeaM rewrite) is not "
             "covered; on expiry `result: unknown` and `solve-timeout: true`"),
    cl::init(0), cl::value_desc("seconds"));

static cl::opt<unsigned> memLimit(
    "mem-limit",
    cl::desc("Memory limit in GB (0: none): RLIMIT_AS of the process, z3's "
             "memory_max_size, Bitwuzla's memory-limit; when hit, "
             "`result: unknown` and `mem-limit: hit`"),
    cl::init(0), cl::value_desc("GB"));

static cl::opt<bool> vcgenOnly(
    "vcgen-only",
    cl::desc("Stop once the VC is generated (after the solver boundary and "
             "the checks of the query): print the statistics and "
             "`result: none`, run no solver"),
    cl::init(false));

// --reach (lib/VC/Reach.cpp): reachability of check sites or blocks, not
// the safety query
static cl::opt<sea::ReachKind> reachKind(
    "reach",
    cl::desc("Decide, per target, whether an execution reaches it with every "
             "earlier assume holding (one query per target; not the safety "
             "query, no `result:` line)"),
    cl::init(sea::ReachKind::Off), cl::ValueOptional,
    cl::values(clEnumValN(sea::ReachKind::Off, "off",
                          "the safety query (default)"),
               clEnumValN(sea::ReachKind::Checks, "checks",
                          "every sea.assume, sea.assert and sea.unreachable "
                          "after the lowering (--reach alone)"),
               clEnumValN(sea::ReachKind::Checks, "", ""),
               clEnumValN(sea::ReachKind::Blocks, "blocks",
                          "every block after the lowering")));
static cl::opt<unsigned> reachRlimit(
    "reach-rlimit",
    cl::desc("z3's resource limit per reach query (0: none; deterministic, "
             "unlike --timeout); a query over it is `unknown reason=rlimit`"),
    cl::init(0), cl::value_desc("n"));
static cl::opt<std::string> reachWitness(
    "reach-witness",
    cl::desc("Write the inputs of each reachable target's replayed run to "
             "<dir>/reach-<k>.txt (sea-run --input-file syntax)"),
    cl::value_desc("dir"));
// TEST ONLY: plant a fault to exercise --reach's checks of its answers
// (skip-assumes: the reach query without the assume facts; block-last: the
// last target's guard forced false; mismatch: one mismatch per replay)
static cl::opt<std::string> reachPlant("x-reach-plant", cl::Hidden,
                                       cl::init(""));
// concrete runs first (lib/VC/Reach.cpp, runReachRuns): the targets runs
// reach are not queried
static cl::list<std::string> reachCorpus(
    "reach-corpus",
    cl::desc("Before any reach query, run these corpus files (or every file "
             "of these directories; sea-run --input-file syntax, e.g. an "
             "earlier --reach-witness directory) on the program the targets "
             "are on; a target a run reaches is reachable via=run, not "
             "queried"),
    cl::value_desc("dir|file"), cl::CommaSeparated);
static cl::opt<uint64_t> reachFuzz(
    "reach-fuzz",
    cl::desc("After the --reach-corpus runs, N fuzz trials (sea-run --fuzz "
             "mutations) before any reach query; targets they reach are not "
             "queried"),
    cl::init(0), cl::value_desc("N"));
static cl::opt<uint64_t>
    reachSeed("reach-seed", cl::desc("The seed of --reach-fuzz (default 0)"),
              cl::init(0), cl::value_desc("n"));
static cl::opt<std::string> reachPrograms(
    "reach-programs",
    cl::desc("Write R_T, the program with every assert dropped and an "
             "assert(false) at T (sea-opt --sea-reach-target), for each "
             "reach target T to <dir>/reach-<k>.mlir and exit: no query"),
    cl::value_desc("dir"));
static cl::opt<bool> reachIncremental(
    "reach-incremental",
    cl::desc("One solver for every reach query, check-sat-assuming the "
             "target's guard (default without --reach-corpus / "
             "--reach-fuzz); =false: a new solver per target, the guard "
             "asserted (default with them: the queries left are mostly "
             "unreachable targets)"),
    cl::init(true));
static cl::opt<bool> reachModelRuns(
    "reach-model-runs",
    cl::desc("The replay of a reachable target's model decides every target "
             "it reaches whose query is still to come (via=model-run; "
             "default on)"),
    cl::init(true));
static cl::opt<bool> reachKeepSites(
    "reach-keep-sites",
    cl::desc("With --reach: the lowering's canonicalize replaces a tagged "
             "assume / assert of the constant true by a reach-site marker "
             "(sea.print with its sea.reach_site; inert in the VC) instead "
             "of erasing it, so the site stays a target (utils/cover-diff.py)"),
    cl::init(false));
static cl::opt<bool> reachFailures(
    "reach-failures",
    cl::desc("With --reach: the failure of every tagged assert is a target "
             "too (op=sea.print of=sea.assert.fail, id <assert id>.fail): "
             "reached iff an execution reaches the assert with its "
             "condition false (utils/cover-diff.py)"),
    cl::init(false));
static cl::opt<bool> reachQuery(
    "reach-query",
    cl::desc("=false: no reach query; the --reach-corpus / --reach-fuzz runs "
             "decide what they reach, every other target is `unknown "
             "reason=no-query` (a witness of one lowering replayed on "
             "another, utils/cover-diff.py)"),
    cl::init(true));
static cl::opt<sea::ReachOrder> reachOrder(
    "reach-order", cl::desc("The order of the reach queries"),
    cl::init(sea::ReachOrder::Deepest),
    cl::values(clEnumValN(sea::ReachOrder::Program, "program", "program order"),
               clEnumValN(sea::ReachOrder::Deepest, "deepest",
                          "reverse program order (default): a model's run "
                          "passes through the targets before its own")));

// Flow logs (docs/reference/logging.md). Every event is written whole and
// flushed, so the phase in progress is visible when a run is killed.
static sea::log::Tag logPhase(
    "vc.phase", 1,
    "the start and end of each sea-vc phase (lower, encode, simplify, "
    "solver-term, ackermann, solve, replay, replay-original); the end with "
    "time, memory and sizes; progress inside encode");
static sea::log::Tag logSolve("vc.solve", 1,
                              "each solver call of the verdict: backend, "
                              "result, time, Expr nodes made so far");

static cl::opt<bool> progress(
    "progress",
    cl::desc("Log the start and end of each phase on stderr, or the log "
             "file: an alias of --log=vc.phase (=false: vc.phase:0) (lower, "
             "encode, simplify, solver-term, "
             "ackermann, solve, replay, replay-original); the end with time, "
             "memory and sizes; progress lines inside encode "
             "(--progress-interval)"),
    cl::init(false), cl::cb<void, bool>([](bool on) {
      sea::log::select(on ? "vc.phase" : "vc.phase:0", "--progress");
    }));

static cl::opt<double> progressInterval(
    "progress-interval",
    cl::desc("vc.phase progress lines inside encode: one when this many "
             "seconds passed since the last (checked before each op and "
             "after each block); 0: one after every block (default 10, "
             "untuned); under --mem-limit also how often the "
             "mem-limit-progress note is refreshed"),
    cl::init(10.0), cl::value_desc("seconds"));

static cl::opt<uint64_t> memLimitPlant(
    "mem-limit-plant",
    cl::desc("Testing only: before encode op <n> (1, 2, ... in encoding "
             "order; 0: off), end the run as an allocation failing under "
             "--mem-limit would"),
    cl::init(0), cl::value_desc("n"), cl::Hidden);

static cl::opt<bool> memLimitPlantLlvm(
    "mem-limit-plant-llvm",
    cl::desc("Testing only: with --mem-limit-plant, fail through LLVM's "
             "out-of-memory path (llvm::report_bad_alloc_error, as an LLVM "
             "container's failed allocation does) instead of operator new's"),
    cl::init(false), cl::Hidden);

namespace {
/// A line the --mem-limit handler writes as is (no allocation there):
/// formatted ahead, in the output format in effect, when its contents
/// change. Two slots: a reader sees a whole line unless set() runs twice while
/// it writes (the main thread is the only writer, and during encode the only
/// allocator).
class HandlerNote {
public:
  /// `textLine`: one `key: value k=v ...` line, no newline.
  void set(StringRef textLine) {
    std::string line = outFormat == sea::out::Format::Jsonl
                           ? sea::out::parseLine(textLine).json()
                           : textLine.str();
    line += '\n';
    const int slot = cur.load(std::memory_order_relaxed) == 0 ? 1 : 0;
    size_t n = line.size();
    if (n > sizeof(buf[slot])) { // never a cut line (invalid JSON)
      sea::diag::warn("vc.mem-limit-note-long",
                      "a --mem-limit note line does not fit its buffer: a "
                      "hit will not print it");
      n = 0;
    }
    std::memcpy(buf[slot], line.data(), n);
    len[slot] = n;
    cur.store(slot, std::memory_order_release);
  }
  void clear() { cur.store(-1, std::memory_order_release); }
  void write(int fd) const {
    const int slot = cur.load(std::memory_order_acquire);
    if (slot >= 0 && len[slot])
      (void)!::write(fd, buf[slot], len[slot]);
  }

private:
  char buf[2][4096];
  size_t len[2] = {0, 0};
  std::atomic<int> cur{-1};
};
} // namespace

/// The phase in progress (`mem-limit-phase:`) and the last encode progress
/// tick of it (`mem-limit-progress:`), for memLimitHit; set only under
/// --mem-limit.
static HandlerNote memPhaseNote, memProgressNote;

/// The op being encoded and its number in the encoding order (EncodeMeter,
/// under --mem-limit): `mem-limit-op:`, formatted by the handler itself.
static std::atomic<Operation *> memCurOp{nullptr};
static std::atomic<uint64_t> memCurOpIndex{0};

namespace {
/// A fixed buffer the handler formats into (no allocation). Text written
/// with putSafe keeps only printable ASCII other than `"` `\\` and space
/// (others become `?`), so the text line is one token per field and the
/// same bytes are valid inside a JSON string.
struct RawLine {
  char b[1024];
  size_t n = 0;
  void put(StringRef s) {
    for (char c : s)
      if (n < sizeof(b))
        b[n++] = c;
  }
  void putSafe(StringRef s) {
    for (char c : s)
      if (n < sizeof(b))
        b[n++] = (c > ' ' && c < 127 && c != '"' && c != '\\') ? c : '?';
  }
  void putNum(uint64_t v) {
    char d[24];
    int k = 0;
    do
      d[k++] = char('0' + v % 10);
    while ((v /= 10) && k < 24);
    while (k && n < sizeof(b))
      b[n++] = d[--k];
  }
};

/// The first source position (or else name) of `loc`, without allocating:
/// looks through names, call sites (the callee) and fused locations (the
/// first). fileLocOf's choice may differ for fused locations.
void rawLoc(RawLine &out, Location loc) {
  StringRef name;
  for (unsigned depth = 0; depth < 16; ++depth) {
    if (auto f = dyn_cast<FileLineColLoc>(loc)) {
      StringRef file = f.getFilename().getValue();
      if (file.size() > 160)
        file = file.take_back(160);
      out.putSafe(file);
      out.put(":");
      out.putNum(f.getLine());
      out.put(":");
      out.putNum(f.getColumn());
      return;
    }
    if (auto nl = dyn_cast<NameLoc>(loc)) {
      if (name.empty())
        name = nl.getName().getValue();
      loc = nl.getChildLoc();
    } else if (auto cs = dyn_cast<CallSiteLoc>(loc)) {
      loc = cs.getCallee();
    } else if (auto fl = dyn_cast<FusedLoc>(loc)) {
      if (fl.getLocations().empty())
        break;
      loc = fl.getLocations().front();
    } else {
      break;
    }
  }
  if (name.empty())
    out.put("-");
  else
    out.putSafe(name.take_front(160));
}
} // namespace

/// `mem-limit-op: <name> ops=<n> at=<where>`: the op in progress when the
/// limit hit (encode only), in the output format in effect.
static void writeMemCurOp(int fd) {
  Operation *op = memCurOp.load(std::memory_order_acquire);
  if (!op)
    return;
  RawLine t;
  t.put("mem-limit-op: ");
  t.putSafe(op->getName().getStringRef());
  t.put(" ops=");
  t.putNum(memCurOpIndex.load(std::memory_order_relaxed));
  t.put(" at=");
  rawLoc(t, op->getLoc());
  if (outFormat != sea::out::Format::Jsonl) {
    t.put("\n");
    (void)!::write(fd, t.b, t.n);
    return;
  }
  // the record parseLine makes of the text line (jsonl-check)
  RawLine j;
  j.put("{\"kind\":\"stat\",\"key\":\"mem-limit-op\",\"value\":\"");
  j.putSafe(op->getName().getStringRef());
  j.put("\",\"ops\":");
  j.putNum(memCurOpIndex.load(std::memory_order_relaxed));
  j.put(",\"at\":\"");
  rawLoc(j, op->getLoc());
  j.put("\",\"text\":\"");
  j.put(StringRef(t.b, t.n));
  j.put("\"}\n");
  (void)!::write(fd, j.b, j.n);
}

/// The memory fields of vc.phase end and progress events: rss-mb vm-mb
/// (Linux only) and peak-rss-mb.
static sea::log::Event &withMem(sea::log::Event &e) {
  const sea::ProcMem m = sea::readProcMem();
  if (m.hasCurrent)
    e.kv("rss-mb", sea::ProcMem::mb(m.rssKB))
        .kv("vm-mb", sea::ProcMem::mb(m.vmKB));
  if (m.hasPeak)
    e.kv("peak-rss-mb", sea::ProcMem::mb(m.peakRssKB));
  return e;
}
static void memText(raw_ostream &os, const sea::ProcMem &m) {
  if (m.hasCurrent)
    os << " rss-mb=" << sea::ProcMem::mb(m.rssKB)
       << " vm-mb=" << sea::ProcMem::mb(m.vmKB);
  if (m.hasPeak)
    os << " peak-rss-mb=" << sea::ProcMem::mb(m.peakRssKB);
}

namespace {
/// A phase of the run: the start event is written when it is made; the end
/// event is SEA_PHASE_END(phase) with the phase's sizes after it.
/// \p run (encode only): the number of the VC run, 1, 2, ... (a fallback
/// or a retry starts the VC again). The clock is read only when vc.phase
/// is on. Under --mem-limit the phase is noted for the handler.
struct Phase {
  const char *name;
  std::chrono::steady_clock::time_point t0;
  explicit Phase(const char *name, unsigned run = 0) : name(name) {
    if (memLimit) {
      std::string t = std::string("mem-limit-phase: ") + name;
      if (run)
        t += " run=" + std::to_string(run);
      memPhaseNote.set(t);
      memProgressNote.clear();
    }
    if (!logPhase.on(sea::log::Flow))
      return;
    t0 = std::chrono::steady_clock::now();
    if (run)
      SEA_LOG(logPhase, sea::log::Flow)
          .msg("start")
          .kv("phase", name)
          .kv("run", run);
    else
      SEA_LOG(logPhase, sea::log::Flow).msg("start").kv("phase", name);
  }
  double seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
  }
};
} // namespace

/// The end event of phase P: the time, the memory (withMem), then the
/// sizes the caller adds; evaluated only when vc.phase is on.
#define SEA_PHASE_END(P)                                                       \
  !logPhase.on(sea::log::Flow)                                                 \
      ? (void)0                                                                \
      : ::sea::log::Voidify() &                                                \
            withMem(::sea::log::Event(logPhase, sea::log::Flow)                \
                        .msg("end")                                            \
                        .kv("phase", (P).name)                                 \
                        .kv("time", (P).seconds()))

[[noreturn]] static void memLimitHit();

namespace {
/// Progress inside encode (VCGen::setProgress), on when vc.phase is on or
/// under --mem-limit: a `vc.phase progress` event (vc.phase on) and the
/// handler's progress note (--mem-limit) at each tick, every
/// --progress-interval seconds (0: after every block). Between ticks it
/// keeps the op that took the longest and the op that made the most Expr
/// nodes so far: one op (a load the SeaM rewriter reads through a long
/// store chain) can take most of the phase, and no tick fires inside it.
class EncodeMeter {
  using Clock = std::chrono::steady_clock;

public:
  EncodeMeter(expr::ExprFactory &efac, unsigned run)
      : efac(efac), run(run), t0(Clock::now()), lastTick(t0) {}

  ~EncodeMeter() { memCurOp.store(nullptr, std::memory_order_release); }

  void operator()(const sea::EncodeProgress &p) {
    if (memLimit) {
      memCurOpIndex.store(p.ops, std::memory_order_relaxed);
      memCurOp.store(p.op, std::memory_order_release);
      if (memLimitPlant && p.op && p.ops == memLimitPlant)
        memLimitPlantLlvm ? llvm::report_bad_alloc_error("planted", false)
                          : memLimitHit();
    }
    const Clock::time_point now = Clock::now();
    const uint64_t nodes = efac.nodesMade();
    if (prevOp) {
      const double d = std::chrono::duration<double>(now - prevStart).count();
      if (d > slowest.amount)
        slowest = {d, prevOp};
      const uint64_t dn = nodes - prevNodes;
      if (double(dn) > biggest.amount)
        biggest = {double(dn), prevOp};
    }
    prevOp = p.op;
    prevStart = now;
    prevNodes = nodes;
    const bool due =
        progressInterval <= 0
            ? p.op == nullptr
            : std::chrono::duration<double>(now - lastTick).count() >=
                  progressInterval;
    if (!due)
      return;
    lastTick = now;
    tick(p, std::chrono::duration<double>(now - t0).count(), nodes);
  }

private:
  struct Top {
    double amount = 0;
    Operation *op = nullptr;
  };
  static std::string where(Operation *op) {
    // the source position, else the location's name (a SeaHorn export's
    // loc("sm460")), else -
    std::string t = "-";
    if (std::optional<FileLineColLoc> fl = sea::fileLocOf(op->getLoc()))
      t = sea::fileLocText(*fl);
    else if (std::optional<std::string> n = sea::locNameOf(op->getLoc()))
      t = *n;
    if (t.size() > 160) // the handler's note has a fixed size
      t = "..." + t.substr(t.size() - 157);
    return t;
  }

  void tick(const sea::EncodeProgress &p, double secs, uint64_t nodes) {
    const sea::ProcMem m = sea::readProcMem();
    std::string s;
    raw_string_ostream os(s);
    os << "time=" << format("%.3f", secs) << " blocks=" << p.blocks
       << " blocks-total=" << p.blocksTotal << " ops=" << p.ops
       << " ops-total=" << p.opsTotal << " nodes-made=" << nodes;
    memText(os, m);
    if (slowest.op)
      os << " slowest-op=" << slowest.op->getName().getStringRef()
         << " slowest-op-time=" << format("%.3f", slowest.amount)
         << " slowest-op-at=" << sea::log::textValue(where(slowest.op));
    if (biggest.op)
      os << " biggest-op=" << biggest.op->getName().getStringRef()
         << " biggest-op-nodes=" << uint64_t(biggest.amount)
         << " biggest-op-at=" << sea::log::textValue(where(biggest.op));
    // the same fields, in the same order, in both
    if (logPhase.on(sea::log::Flow)) {
      sea::log::Event e(logPhase, sea::log::Flow);
      e.msg("progress")
          .kv("phase", "encode")
          .kv("run", run)
          .kv("time", secs)
          .kv("blocks", p.blocks)
          .kv("blocks-total", p.blocksTotal)
          .kv("ops", p.ops)
          .kv("ops-total", p.opsTotal)
          .kv("nodes-made", nodes);
      withMem(e);
      if (slowest.op)
        e.kv("slowest-op", slowest.op->getName().getStringRef())
            .kv("slowest-op-time", slowest.amount)
            .kv("slowest-op-at", where(slowest.op));
      if (biggest.op)
        e.kv("biggest-op", biggest.op->getName().getStringRef())
            .kv("biggest-op-nodes", uint64_t(biggest.amount))
            .kv("biggest-op-at", where(biggest.op));
    }
    if (memLimit) {
      memProgressNote.set("mem-limit-progress: " + s);
    }
  }

  expr::ExprFactory &efac;
  unsigned run;
  Clock::time_point t0, lastTick, prevStart;
  Operation *prevOp = nullptr;
  uint64_t prevNodes = 0;
  Top slowest, biggest;
};
} // namespace

static uint64_t countOps(Operation *op) {
  uint64_t n = 0;
  op->walk([&](Operation *) { ++n; });
  return n;
}

/// --mem-limit: an allocation that fails (operator new; Bitwuzla, the VC)
/// ends the run as unknown (LLVM containers fail through
/// llvm::report_bad_alloc_error: memLimitHitLlvm). Writes without allocating.
[[noreturn]] static void memLimitHit() {
  outs().flush();
  static const char msg[] = "result: unknown\nmem-limit: hit\n";
  static const char jmsg[] =
      "{\"kind\":\"result\",\"value\":\"unknown\",\"text\":\"result: "
      "unknown\"}\n{\"kind\":\"stat\",\"key\":\"mem-limit\",\"value\":"
      "\"hit\",\"text\":\"mem-limit: hit\"}\n";
  if (outFormat == sea::out::Format::Jsonl)
    (void)!::write(1, jmsg, sizeof(jmsg) - 1);
  else
    (void)!::write(1, msg, sizeof(msg) - 1);
  // where: the phase in progress, and the last progress tick of encode
  memPhaseNote.write(1);
  memProgressNote.write(1);
  writeMemCurOp(1);
  static const char err[] =
      "Error: memory limit (--mem-limit) hit [vc.mem-limit]\n";
  (void)!::write(2, err, sizeof(err) - 1);
  ::_exit(3);
}

/// The LLVM bad-alloc handler (llvm::safe_malloc, SmallVector growth, ...);
/// it must not return.
[[noreturn]] static void memLimitHitLlvm(void *, const char *, bool) {
  memLimitHit();
}

static cl::list<std::string> configFiles(
    "config",
    cl::desc("Read options from a config file (LLVM config-file syntax: # "
             "comments, @file includes relative to the file); repeatable, "
             "the configs' options come before the command line's, so later "
             "ones win. A bare name (no '/', no extension) is <name>.cfg in "
             "$SEA_MLIR_PROFILE_PATH, then <prefix>/share/sea-mlir/profiles, "
             "then the source tree's profiles/ (docs/Profiles.md)"),
    cl::value_desc("file|name"));

/// The directories a bare --config name is looked up in, in order.
static std::vector<std::string> profileDirs(const char *argv0) {
  std::vector<std::string> dirs;
  if (const char *env = std::getenv("SEA_MLIR_PROFILE_PATH")) {
    SmallVector<StringRef> parts;
    StringRef(env).split(parts, ':', -1, /*KeepEmpty=*/false);
    for (StringRef p : parts)
      dirs.push_back(p.str());
  }
  std::string exe =
      sys::fs::getMainExecutable(argv0, (void *)(intptr_t)&profileDirs);
  if (!exe.empty()) {
    SmallString<256> d(sys::path::parent_path(exe));
    sys::path::append(d, "..", "share", "sea-mlir", "profiles");
    sys::path::remove_dots(d, /*remove_dot_dot=*/true);
    dirs.push_back(std::string(d));
  }
#ifdef SEA_SOURCE_PROFILES_DIR
  dirs.push_back(SEA_SOURCE_PROFILES_DIR);
#endif
  return dirs;
}

/// --config: takes the --config options out of \p argv and returns the
/// arguments to parse: argv[0], each config's options in order, then the
/// rest of the command line (cl::opt keeps the last value: later configs,
/// then the command line, win). \p loaded gets the resolved paths. False
/// (after a diagnostic) if a config cannot be found or read.
static bool expandConfigs(int argc, char **argv, StringSaver &saver,
                          BumpPtrAllocator &alloc,
                          SmallVectorImpl<const char *> &out,
                          std::vector<std::string> &loaded) {
  SmallVector<StringRef> names;
  SmallVector<const char *> rest;
  bool positional = false;
  for (int i = 1; i < argc; ++i) {
    StringRef a = argv[i];
    if (!positional && a == "--")
      positional = true;
    StringRef opt = a;
    if (!positional && (opt.consume_front("--") || opt.consume_front("-"))) {
      if (opt.consume_front("config=")) {
        names.push_back(opt);
        continue;
      }
      if (opt == "config" && i + 1 < argc) {
        names.push_back(argv[++i]);
        continue;
      }
    }
    rest.push_back(argv[i]);
  }
  out.push_back(argv[0]);
  std::vector<std::string> dirs;
  for (StringRef name : names) {
    SmallString<256> path;
    const bool bare = !name.contains('/') && sys::path::extension(name).empty();
    if (bare) {
      if (dirs.empty())
        dirs = profileDirs(argv[0]);
      for (const std::string &d : dirs) {
        SmallString<256> p(d);
        sys::path::append(p, name + ".cfg");
        if (sys::fs::is_regular_file(p)) {
          path = p;
          break;
        }
      }
    } else if (sys::fs::is_regular_file(name)) {
      path = name;
    }
    if (path.empty()) {
      std::string where;
      if (bare)
        for (const std::string &d : dirs)
          where += (where.empty() ? "" : ", ") + d;
      sea::diag::error("vc.config-not-found",
                       "--config=" + name + ": " +
                           (bare
                                ? "no " + name + ".cfg in " +
                                      (where.empty() ? "(no directory)" : where)
                                : "no such file"));
      return false;
    }
    sys::fs::make_absolute(path);
    sys::path::remove_dots(path, /*remove_dot_dot=*/true);
    // a --config inside a config is LLVM's: a path, or a file name (with
    // its extension) in the profile directories; not printed (use @file)
    if (dirs.empty())
      dirs = profileDirs(argv[0]);
    SmallVector<StringRef> searchDirs(dirs.begin(), dirs.end());
    cl::ExpansionContext ectx(alloc, cl::tokenizeConfigFile);
    ectx.setSearchDirs(searchDirs);
    if (Error e = ectx.readConfigFile(saver.save(path.str()), out)) {
      sea::diag::error("vc.config-read",
                       "--config=" + name + ": " + toString(std::move(e)));
      return false;
    }
    loaded.push_back(std::string(path));
  }
  out.append(rest.begin(), rest.end());
  return true;
}

static cl::opt<sea::PrintMode> printMode(
    "print",
    cl::desc("Output of the program's print ops (sea.print*) in a sat "
             "replay"),
    cl::init(sea::PrintMode::Lines),
    cl::values(clEnumValN(sea::PrintMode::Lines, "lines",
                          "one `print:` line each, scopes as dotted paths "
                          "(default)"),
               clEnumValN(sea::PrintMode::Tree, "tree", "indented by scope"),
               clEnumValN(sea::PrintMode::Off, "off", "none")));

static cl::opt<uint64_t>
    printLimit("print-limit",
               cl::desc("At most this many print lines (0: no limit)"),
               cl::init(1000));

static cl::opt<bool>
    printLocs("print-locs",
              cl::desc("Append at=<file:line:col> [at_name=<name>] "
                       "to print lines"),
              cl::init(false));

static cl::opt<sea::PrintRefs> printRefs(
    "print-refs",
    cl::desc("Convert the program's prints before the lowering "
             "(docs/design/Print.md)"),
    cl::init(sea::PrintRefs::AsIs),
    cl::values(
        clEnumValN(sea::PrintRefs::AsIs, "asis", "as written (default)"),
        clEnumValN(sea::PrintRefs::Weak, "weak",
                   "by name (sea-print-weaken): nothing kept alive; a value "
                   "optimized away prints <dangling: name>"),
        clEnumValN(sea::PrintRefs::Strong, "strong",
                   "SSA operands (sea-print-strengthen): the values are kept "
                   "through the lowering")));

// Case splitting of the VC (sea/VC/Split.h, docs/design/Split.md)
static cl::list<std::string> splitBool(
    "split", cl::CommaSeparated,
    cl::desc("Split the VC on a Boolean: an i1 value or a block guard of the "
             "encoded program, by VC name (%x, nd.<site>, ^bb3) or "
             "@<file>:<line> (the one i1 op result there). Two parts: the "
             "value fixed to 1 / 0 (the guard to true / false) before SeaM "
             "rewrites its uses, each with its condition. Repeatable "
             "(nested splits)"),
    cl::value_desc("selector"));
static cl::list<std::string> splitLeaves(
    "split-leaves", cl::CommaSeparated,
    cl::desc("Split the VC on an address or integer (iN, N > 1) value whose "
             "term is an ite tree (an address: or ptr-from-bits of one): one "
             "part per distinct leaf, the value fixed to the leaf, with the "
             "disjunction of the leaf's path conditions. Selector as for "
             "--split (an address or integer op result or block argument)"),
    cl::value_desc("selector"));
static cl::opt<bool> splitAuto(
    "split-auto",
    cl::desc("Heuristic (needs --seam): one VC generation without a solver "
             "picks the ite-valued address whose term the most open alias "
             "questions (IS_OVERLAP) contain, then splits on its leaves "
             "(--split-leaves); ranks the candidates as split.candidate "
             "lines"),
    cl::init(false));
static cl::opt<bool> splitCandidatesOpt(
    "split-candidates",
    cl::desc("List split targets and exit (needs --seam; no solver, "
             "`result: none`): the pick run of --split-auto, then every "
             "ite-valued address (split.leaves-candidate, for "
             "--split-leaves) and the i1 values and block guards in the ite "
             "conditions behind open alias questions or in the indices of "
             "array reads (split.bool-candidate, for --split), ranked by "
             "those questions, then those reads; and the integer and i1 "
             "values and guards by the mul/div/rem nodes they control "
             "(split.arith-candidate) (heuristics)"),
    cl::init(false));
static cl::opt<unsigned> splitCandidatesMax(
    "split-candidates-max",
    cl::desc("--split-candidates: the most split.bool-candidate and "
             "split.arith-candidate lines (default 20; 0: all); the "
             "split.candidates and split.arith-candidates lines count the "
             "dropped ones"),
    cl::init(20));
static cl::opt<unsigned> splitMaxParts(
    "split-max-parts",
    cl::desc("Hard cap on the number of parts of a split (default 64, "
             "untuned): more is an error (vc.split-max-parts); raise it for "
             "nested splits or wide leaf sets"),
    cl::init(64));
static cl::opt<unsigned> splitMaxPaths(
    "split-max-paths",
    cl::desc("--split-leaves: the most root-to-leaf paths of an ite tree "
             "(default 1024)"),
    cl::init(1024));
static cl::opt<bool> splitCheck(
    "split-check",
    cl::desc("Check the construction of every split level with z3 (default "
             "on): its case conditions are exhaustive (valid disjunction) "
             "and the chosen case's condition implies term = value; "
             "split.check lines; a failed check is exit 5"),
    cl::init(true));
static cl::opt<unsigned> splitCheckRlimit(
    "split-check-rlimit",
    cl::desc("z3 rlimit of each --split-check query (default 10000000; 0: "
             "none); an exhausted one is unknown, reported"),
    cl::init(10000000));
static cl::opt<bool> splitStopOnSat(
    "split-stop-on-sat",
    cl::desc("Stop at the first part whose answer is sat (the verdict is sat "
             "then; the parts not run are not reported)"),
    cl::init(false));
static cl::opt<std::string>
    splitPlant("x-split-plant", cl::Hidden,
               cl::desc("Test only: plant a split construction bug "
                        "(drop-case, wrong-value) for --split-check"));
static cl::opt<std::string> splitSmt2(
    "split-smt2",
    cl::desc("Write the query of part N to <prefix>.part<N>.smt2 (the "
             "solver's SMT-LIB, as --smt2 prints it); with --vcgen-only, "
             "nothing is solved"),
    cl::value_desc("prefix"));

static cl::opt<bool> agent("agent",
                           cl::desc("Print a terse guide for agents and exit"),
                           cl::init(false));

static const char *const agentGuide = R"(sea-vc agent guide (plain, terse)

WHAT: verifies a sea.program with a SeaHorn-style verification condition:
symbolic execution of each block (OpSem) plus a VC that connects blocks
(block Booleans, edge conditions, block arguments). sat means the assertion
can fail; the counterexample is then replayed in the interpreter (sea-run's
semantics) and every computed value is checked against the model.

WHY BEAT MANUAL: the VC is exhaustive over all nondet values; the replay
turns a solver model into a concrete run you can trust (or exposes a VC bug).

INPUT FORM (checked; anything else is an error, never approximated):
  loop-free; one sea.halt; exactly one sea.assert immediately before it;
  no critical edges; sea.cond_br targets distinct. Also: no sea.halt and
  no sea.assert (every path ends in sea.unreachable): statically safe.
  nsw / nuw flags are binding semantics (a front end that does not trust
  them strips them: sea-wrap-flags mode=drop). The VC encoding ignores them
  (the value wraps, as in SeaHorn: an over-approximation); --range-fold=on
  uses them (counted: vc.range-fold wrap-flags=). sea-wrap-flags turns
  them into assumes or asserts.
  sea.box.all (a claim over a box of 1-2 i64 indices, docs/design/BoxAll.md)
  needs --lower: sea-lower-box-all replaces it by its body at one
  arbitrary index (nondet box.all.<tag>.<d>, assume t <u trip count; a box
  that may be empty is entered only when it is not) where its result
  reaches sea.assert through i1 sea.and / sea.or only; any other use is an
  error box-all.use-<assume|branch|negation|select|print|value> (no
  unrolling fallback). A box of constant upper bounds (the op folders
  reduce them to one before sccp: sea.const, flag-free arithmetic on
  constants) with at most K indices (--box-all-expand=K <= 1024, untuned
  default 16) is expanded instead: the body at every index, and-reduced
  (same polarity rule). Prints vc.box-all: dead= empty= expand-max=
  expanded= guarded= lowered= nondets=. Use it for claims whose instances
  are each expensive (a read of a memory through many writes: one index
  replaces them all); keep a
  claim whose instances are each matched by an assumption instance (e.g.
  pairwise distinctness assumed and asserted) as explicit conjunctions:
  one arbitrary instance hides the match and was measured slower.

TYPICAL:
  sea-vc p.mlir                  # solve with z3, replay if sat
  sea-vc p.mlir --solver=yices   # same with Yices 2
  sea-vc p.mlir --sink-assumes   # one assume (the conjunction of the
                                 #   assumes on the path) before the assert,
                                 #   instead of one per site (exact; a solver
                                 #   tradeoff: vc.sink-assumes: <n moved>)
  sea-vc p.mlir --solver=yices --yices-param=random-seed=7  # a Yices search
                                 #   parameter (seed variance of a timing);
                                 #   --yices-logic=QF_BV / --yices-config=
                                 #   key=value choose the context (default:
                                 #   Yices' general one). Printed as
                                 #   solver.yices-{param,logic,config} lines
  sea-vc p.mlir --solver=bitwuzla  # same with Bitwuzla (only if sea-mlir was
                                 #   built with -DBITWUZLA_ROOT=<install>;
                                 #   else an error)
  sea-vc p.mlir --smt2           # print the query (SMT-LIB) only
  sea-vc p.mlir --lower --seam --split-leaves=@f.cc:768  # one part per leaf
                                 #   of the address loaded there, each with
                                 #   its counters, answer and time (SPLIT;
                                 #   --split-auto picks one)
  sea-vc p.mlir --lower --seam --split-candidates  # list what --split and
                                 #   --split-leaves can split on, ranked by
                                 #   the open alias questions (none open:
                                 #   by the array reads whose index they
                                 #   are in), and by the mul/div/rem they
                                 #   control; no solver
  sea-vc p.mlir --lower --split-leaves=@f.cc:286  # one part per value of a
                                 #   path-merged integer (a column index):
                                 #   each part multiplies by a constant
  sea-vc p.mlir --lower --ite-stats --vcgen-only  # which memories,
                                 #   addresses and integers feeding
                                 #   arithmetic are ite trees over block
                                 #   guards, and the loads at non-ground
                                 #   addresses, per source line; no solver
  sea-vc p.mlir --format=jsonl   # every stdout line one JSON object (jq;
                                 #   OUTPUT FORMATS)
  sea-vc p.mlir --timeout=600 --mem-limit=16  # limits: the solver's check
                                 #   (wall clock, s; z3 timeout, Bitwuzla
                                 #   time-limit-per, Yices a watchdog that
                                 #   stops the search) and the process's
                                 #   memory (GB: RLIMIT_AS, z3
                                 #   memory_max_size, Bitwuzla memory-limit);
                                 #   either one gives result: unknown,
                                 #   status 3
                                 #   --timeout does not bound VC generation
                                 #   (lowering, encoding, the SeaM rewrite);
                                 #   a run whose VC generation may not
                                 #   terminate (e.g. a SeaM rewrite blowup)
                                 #   is bounded by --mem-limit (an
                                 #   allocation failure gives result:
                                 #   unknown, mem-limit: hit) or an external
                                 #   `timeout`; --progress shows the phase
                                 #   it is stuck in
                                 #   --mem-limit is RLIMIT_AS (address
                                 #   space, not resident memory): set it
                                 #   above the expected resident size, with
                                 #   headroom
  sea-vc p.mlir --lower --reach  # REACH: per check site (sea.assume,
                                 #   sea.assert, sea.unreachable = a folded
                                 #   assume of false) after the lowering, is
                                 #   its top reached by an execution on which
                                 #   every earlier assume holds? One query
                                 #   per target, each reachable one replayed
                                 #   (vacuity evidence; not the safety
                                 #   query: no result: line)
  sea-vc p.mlir --lower --reach=blocks  # the same per block
  sea-vc p.mlir --reach --reach-rlimit=300000000  # z3 resource limit per
                                 #   query (deterministic; over it: unknown
                                 #   reason=rlimit); --timeout bounds each
                                 #   query too
  sea-vc p.mlir --reach --reach-witness=<dir>  # each reachable target's
                                 #   inputs to <dir>/reach-<k>.txt (k = the
                                 #   target's position): sea-run <lowered
                                 #   program> --input-file=<file> reaches it
  sea-vc p.mlir --lower --reach --reach-corpus=<dir> --reach-fuzz=1000
                                 # CONCRETE FIRST: run the corpus files (e.g.
                                 #   an earlier --reach-witness dir) and 1000
                                 #   fuzz trials on the program the targets
                                 #   are on; targets a run reaches are
                                 #   reachable via=run, not queried; the
                                 #   solver answers the rest (--reach-seed:
                                 #   the trials' seed)
  sea-vc p.mlir --lower --reach --reach-programs=<dir>  # R_T per target T
                                 #   (every assert dropped, assert(false) at
                                 #   T) to <dir>/reach-<k>.mlir, then exit:
                                 #   sea-vc <dir>/reach-<k>.mlir is sat iff T
                                 #   is reachable (sea-opt
                                 #   --sea-reach-target="id=<id>": one R_T)
  sea-vc p.mlir --reach --reach-incremental=false  # a new solver per target
                                 #   with the guard asserted, a plain check
                                 #   (default: one solver, check-sat-assuming
                                 #   the guard; z3 answers that with its
                                 #   incremental core: cheaper sat answers,
                                 #   an unsat one can be much slower; with
                                 #   --reach-corpus / --reach-fuzz the
                                 #   default is =false, =true still applies)
  sea-vc p.mlir --reach --reach-model-runs=false  # every target queried
                                 #   (default: a model's replay decides the
                                 #   targets it reaches whose query is still
                                 #   to come, via=model-run)
  sea-vc p.mlir --lower --reach --reach-query=false --reach-corpus=<f>
                                 #   runs only: what the corpus reaches,
                                 #   every other target unknown
                                 #   reason=no-query (a witness of another
                                 #   lowering, utils/cover-diff.py)
  sea-vc p.mlir --reach --reach-order=program  # query in program order
                                 #   (default deepest: reverse program order,
                                 #   a deep target's model run passes the
                                 #   targets before it)
  sea-vc p.mlir --vcgen-only     # generate and check the VC, print its
                                 #   statistics and result: none; no solver
                                 #   of the query; --eq-audit runs (its own
                                 #   z3 checks); --vcgen-only skips
                                 #   --seam-check and --x-read-table-check
                                 #   (warning vc.vcgen-only-check-skipped);
                                 #   not with
                                 #   --smt2
  sea-vc p.mlir --print-refs=strong  # keep every printed value alive
                                 #   through the lowering (PRINTS)
  sea-vc p.mlir --progress       # = --log=vc.phase: the start and end
                                 #   (time, memory, sizes) of each phase on
                                 #   stderr; progress inside encode
  sea-vc p.mlir '--log=*:1'      # the whole flow: phases, passes, solver
                                 #   calls (LOGS)
  sea-vc p.mlir --config=seam    # options from a profile (CONFIG below);
                                 #   the command line still overrides them
  sea-vc p.mlir --lower          # lower first (= sea-opt
                                 #   --sea-vc-lower): tracking markers,
                                 #   fold constants (sccp), scalarize
                                 #   structs; if abstract: tracking and
                                 #   fat-pointer metadata to memories, then
                                 #   place; widen (--widen-memory=k); expand
                                 #   symbolic memsets (not with --seam);
                                 #   merge asserts, split wide accesses
                                 #   (kept whole with --seam --seam-wide),
                                 #   expand intrinsics (z3 and bitwuzla
                                 #   keep umulo/smulo: native overflow
                                 #   predicates), split critical edges;
                                 #   cse after sccp, canonicalize + cse
                                 #   before splitting edges (--lower-opt;
                                 #   canonicalize also folds constant
                                 #   assumes / asserts, merges straight
                                 #   blocks, forwards a stored value to a
                                 #   load at the same address)
  sea-vc p.mlir --lower --lower-opt=false  # the lowering without cse /
                                 #   canonicalize: every op stays (A/B of
                                 #   the clean-up)
  sea-vc p.mlir --lower --lower-disable=canonicalize  # leave
                                 #   optimization passes out (sccp, cse,
                                 #   canonicalize; comma-separated): the A
                                 #   side of a pass validation
                                 #   (utils/cover-diff.py). Not
                                 #   sea-drop-fresh-memhavoc: it decides
                                 #   that a fresh memhavoc is no tracked
                                 #   write (a semantics decision)
  sea-vc p.mlir --lower --print-lowered=low.mlir --vcgen-only  # the program
                                 #   the VC encodes, with locations, named
                                 #   as the VC (split selectors, split.*
                                 #   lines) names it; input to sea-vc
                                 #   without --lower (same names)
  sea-vc p.mlir --lower --place-out=p.place  # write the placement (one
                                 #   object per line: name= kind= base=
                                 #   size= align=)
  sea-vc p.mlir --lower --place-in=p.place  # place the objects at the
                                 #   file's bases (another lowering's
                                 #   --place-out), checked as placement
                                 #   does (place.in-*); an object the
                                 #   file does not name is an error
                                 #   (place.in-unplaced), file objects
                                 #   the program lacks are holes. Two
                                 #   lowerings on one file: common objects
                                 #   at the same addresses (cover-diff)
  sea-vc p.mlir --lower --reach --reach-failures  # also each tagged
                                 #   assert's failure (reached with its
                                 #   condition false): op=sea.print
                                 #   of=sea.assert.fail id=<id>.fail
  sea-vc p.mlir --lower --reach --reach-keep-sites  # canonicalize keeps a
                                 #   tagged check of true as a reach-site
                                 #   marker (sea.print, op=sea.print
                                 #   of=<op>), a target, instead of
                                 #   erasing it
  sea-vc p.mlir --lower --widen-memory=8 --widen-memory-report  # a remark
                                 #   per memory: widened, or kept as bytes
                                 #   and the first reason (remark: <loc>:
                                 #   sea-widen-memory: ...)
  sea-vc p.mlir --lower --widen-memory=4  # also widen byte memories whose
                                 #   accesses are all aligned 4-byte words
                                 #   (fewer, wider array cells)
  sea-vc p.mlir --lower --seam   # SeaM: a placed object's base is the
                                 #   constant obj.<name> with a side equality
                                 #   obj.<name> = <address>, not the numeral,
                                 #   and every load / store is rewritten as it
                                 #   is encoded: select over store / ite
                                 #   (hit and skip decided by an object-aware
                                 #   compare), pointer normalization, ite and
                                 #   Boolean rules. Conditionally sound: it
                                 #   assumes accesses stay inside their
                                 #   objects (A-INB), so the store address and
                                 #   read address of a ROW step in different
                                 #   objects differ (never a compare of the
                                 #   program); --seam-inb=0 uses no
                                 #   assumption. An access none of whose
                                 #   alternatives can be in its object (a
                                 #   numeral offset outside it, an object of
                                 #   size 0: sea.ptr.null is the null object,
                                 #   obj.null, size 0) is detected (a sanity
                                 #   check, not a soundness claim): warning
                                 #   vc.seam-inb-violated, and the run starts
                                 #   again without the assumption; an
                                 #   impossible alternative of a mixed
                                 #   pointer is only false in its condition.
                                 #   A ROW pair with one side in a size-0
                                 #   object is vacuous under A-INB
                                 #   (cmp.size0-vacuous).
                                 #   Shadow memories (sea.mem.const ...
                                 #   shadow: sea-lower-tracking) are read
                                 #   by object identity, never by A-INB:
                                 #   their accesses are not counted in
                                 #   seam.inb (seam.rule.shadow.access),
                                 #   two different bases at offset 0 are
                                 #   different cells (shadow.distinct-base,
                                 #   premise LAYOUT: the obj.k = <address>
                                 #   equations; not an assumption)
                                 #   A-INB is assumed and reported
                                 #   (`seam.inb: assumed ...`), never
                                 #   discharged by sea-vc: an unsat holds
                                 #   for programs whose accesses stay in
                                 #   their objects. Discharging it is a
                                 #   separate memory-safety job (bounds
                                 #   checks before every access)
                                 #   Needs --vc-join=ite. Segments must have
                                 #   distinct names and disjoint ranges
                                 #   (errors vc.seam-segment-duplicate /
                                 #   -overlap)
                                 #   memset / memmove / memhavoc of a byte
                                 #   memory are one term each (mset / mcopy /
                                 #   mhavoc, any length, symbolic too: no
                                 #   sea-expand-memset, no n <= K bound, so
                                 #   executions with a long n count: an old
                                 #   unsat may become sat). A read over a
                                 #   term is decided once (counters
                                 #   seam.rule.row.<op>-hit / -skip, or
                                 #   row.<op>: one ite with (j - d) <u n).
                                 #   No term reaches the solver: the boundary
                                 #   translates what is left by the read
                                 #   equations, no assumption
                                 #   (seam.rule.memop.translated)
  sea-vc p.mlir --lower --seam --seam-prune-size0=0   # no addr.prune-size0
                                 #   (default on, uses A-INB): an access
                                 #   address ite(c, null + o, B + x) is
                                 #   kept; with the rule a load / store uses
                                 #   B + x (seam.rule.addr.prune-size0), a
                                 #   range drops the null leaf
                                 #   (addr.prune-size0-range) or, all leaves
                                 #   size 0, is a no-op (A-INB: n = 0;
                                 #   addr.prune-size0-noop). Only the access
                                 #   argument changes, never a compare or a
                                 #   stored pointer; shadow memories exempt
  sea-vc p.mlir --lower --seam --seam-inb=0 --seam-layout=0
                                 # without A-INB, do not decide access
                                 #   compares / memop ranges of two placed
                                 #   objects by their placed numerals
                                 #   (cmp.layout, mem.in-layout; default on)
  sea-vc p.mlir --lower --seam --seam-row-expand=0   # keep a read over a
                                 #   store whose address compare is unknown
                                 #   for the solver, instead of expanding it
                                 #   into ite(i = j, v, select(a, j)): much
                                 #   smaller queries when most compares stay
                                 #   unknown (ARM decides some of them)
  sea-vc p.mlir --lower --seam --seam-resolve-ite-nodes=N  # resolve
                                 #   a loaded pointer through an ite of
                                 #   loaded values with <= N distinct ite
                                 #   nodes (default 0: <= 4 tree leaves;
                                 #   counter ptr.load-resolve-ite-dag)
  sea-vc p.mlir --lower --seam --seam-pull-concat=N  # resolve a split
                                 #   pointer load by pulling ite conditions
                                 #   out of the concat, shared by its bytes
                                 #   first (<= N pulls; counters
                                 #   ptr.load-resolve-concat[.unshared])
  sea-vc p.mlir --lower --seam --ptr-companion=exact  # split pointer
                                 #   loads read a companion memory of the
                                 #   stored pointers, guarded (opaque mode;
                                 #   counters vc.ptr-companion*); =assume:
                                 #   unguarded under A-PTRINT
                                 #   (seam.ptrint), a sat answer rerun exact
  sea-vc p.mlir --lower --seam --seam-wide --seam-wide-ite-open  # W-ITE
                                 #   with undecided leaf pairs too (each a
                                 #   read at its leaf address; default:
                                 #   only when every pair is decided)
  sea-vc p.mlir --lower --seam --seam-wide --seam-offset-leaves=0  # the boundary's
                                 #   byte k of a width term at an ite
                                 #   address as ite + k (default: k in the
                                 #   ite leaves, <= 8 leaves; counter
                                 #   wide.expanded-ite-offset; only with
                                 #   --seam-wide)
  sea-vc p.mlir --lower --seam --seam-leaf-split=0  # keep reads at ite
                                 #   addresses whole (default: split when
                                 #   a write gives every leaf read)
  sea-vc p.mlir --lower --seam --seam-leaf-split-partial  # R-LEAVES with
                                 #   undecided leaves too: they stay one
                                 #   read at the ite of them (untuned)
  sea-vc p.mlir --lower --seam --seam-congruence  # decide addresses
                                 #   apart by divisibility: 8i+1 vs 8j
                                 #   (exact; rule cmp.congruence)
  sea-vc p.mlir --lower --seam --seam-pull-index  # pull a pointer
                                 #   node through any ite in its base or
                                 #   indices (default off: the base ite and
                                 #   stride-1 index ites only; counter
                                 #   seam.rule.ptrindex.pull-ite-index)
  sea-vc p.mlir --lower --seam --seam-arm=0   # no ARM: only the syntactic
                                 #   compares (default: on; the compare of
                                 #   a read-over-write step falls back to
                                 #   disjoint address range maps, counter
                                 #   seam.rule.arm.skip; same-base intervals
                                 #   use no assumption, different bases use
                                 #   A-INB; never a program compare)
  sea-vc p.mlir --lower --seam --seam-store-map=0   # no Store-Map (default
                                 #   on): writes stay a linear chain of
                                 #   stores. With it, writes to one object at
                                 #   numeral offsets are grouped into a
                                 #   store-map (an ordered map per object),
                                 #   writes to objects allocated earlier move
                                 #   below it (A-INB), and a read is a map
                                 #   lookup: same query, much less rewriting
                                 #   time on long write sequences (thesis
                                 #   Table 8.4). Counters seam.rule.smap.* and
                                 #   seam.rule.row.smap-*
  sea-vc p.mlir --lower --seam --seam-simplify=0   # only the rules of the
                                 #   memory terms; default (--seam-simplify=1)
                                 #   also normalizes every term of the VC, the
                                 #   side conditions and the query: byte
                                 #   reassembly (concat / extract), linear
                                 #   equalities, the order of the arguments of
                                 #   bvadd / bvmul / bvand / bvor / bvxor / and /
                                 #   or, Boolean ite. Valid over plain
                                 #   bit-vectors (no assumption); counters
                                 #   seam.rule.bv.*, seam.rule.bool.*
  sea-vc p.mlir --lower --simplify   # the same normalization without SeaM
                                 #   (an error together with --seam, which has it on)
                                 #   (the plain VC); with --seam-stats too
  sea-vc p.mlir --lower --seam --seam-stats   # also print what fired
  sea-vc p.mlir --lower --seam --seam-trail=t.jsonl   # the rule trail:
                                 #   one JSON line per rule firing (rules,
                                 #   premises, op / loc, lhs / rhs node ids)
                                 #   in t.jsonl, the term DAG in
                                 #   t.jsonl.terms; --seam-trail-text adds
                                 #   the printed terms. Query it with
                                 #   utils/seam-trail.py (--agent)
  sea-vc p.mlir --lower --seam --seam-trace=row.store-skip,cmp.*   # print
                                 #   the firings of those rules to stderr
                                 #   (`seam-trace:` lines; `all` = every one)
  sea-vc p.mlir --z3-tactic=smtfd     # z3's default tactic (global
                                 #   tactic.default_tactic, as SeaHorn's
                                 #   --horn-bmc-tactic; `sat` / `...sat)` also
                                 #   set sat.euf); --z3-param=key=value sets
                                 #   any global z3 parameter. Prints
                                 #   solver.z3-tactic: <t>. A sat is still
                                 #   replayed through the interpreter
  sea-vc p.mlir --lower --widen-memory=8 --seam --z3-tactic=sls-smt --timeout=60
                                 #   z3's stochastic local search (sat or
                                 #   unknown, never unsat: an unknown ends at
                                 #   the timeout, exit 3; always give
                                 #   --timeout). Try it when the default
                                 #   solve times out on a query that looks
                                 #   sat, or as a cross-check; on small bugs
                                 #   that CDCL finds in seconds it found
                                 #   none. A seed:
                                 #   --z3-tactic='(using-params sls-smt
                                 #   :random_seed 3)'
  sea-vc p.mlir --lower --mem=lambda   # SeaHorn's lambda memory encoding
                                 #   (docs/design/Lambda.md): memories are
                                 #   unary functions, loads fapp, stores /
                                 #   memset / memmove / memhavoc lambdas
                                 #   with (i - d) <u n (exact symbolic
                                 #   lengths, no sea-expand-memset); z3's
                                 #   simplifier on every value written to a
                                 #   register (--lambda-simplify=z3|none,
                                 #   --lambda-simplify-every=k,
                                 #   --lambda-final-simplify: also the whole
                                 #   query). z3 only (byte and widened memories), ite
                                 #   joins; no assumption. Lines:
                                 #   lambda.writes / lambda.simplify.memory /
                                 #   .scalar / .skipped: <n>,
                                 #   lambda.simplify-time,
                                 #   lambda.final-simplify-time: <s>
  sea-vc p.mlir --lower --seam --eq-audit[=N]   # which undecided
                                 #   equalities were decidable: N (200) of
                                 #   each source (ack: Ackermann constraint
                                 #   pairs; row-store: reads over a store
                                 #   whose compare stayed unknown; row-memop:
                                 #   reads over mset / mcopy / mhavoc left
                                 #   as one ite, the membership (j-d) <u n),
                                 #   each by the lowest level that decides
                                 #   it with z3: L0 nothing (a missing exact
                                 #   rewrite), L1 + layout, L2 + A-INB of the
                                 #   accesses (a missing A-INB rule), else
                                 #   undecided (the solver's search, or a
                                 #   path fact); unknown = timeout
                                 #   (--eq-audit-timeout=<s>, 5), vacuous =
                                 #   the L2 premise is unsat, unstated = no
                                 #   A-INB premise. Lines (after the run):
                                 #   eq-audit: source=<s> items=<n>
                                 #     sampled=<n> L0=.. L1=.. L2=..
                                 #     undecided=.. unknown=.. vacuous=..
                                 #     unstated=..
                                 #   eq-audit-shape: source= level= count=
                                 #     example=<firing id | pair number>
                                 #     shape=<a | b>  (O object, N size-0
                                 #     object, K numeral, R Ackermann read,
                                 #     X other constant, _ cut at depth 3;
                                 #     top --eq-audit-shapes=<k> (8) per
                                 #     level), then eq-audit-example: <the
                                 #     first example's terms>
                                 #   eq-audit-time: <s>; with --brunch-stats
                                 #     BRUNCH_STAT eq-audit.<source>.<level>
  sea-vc p.mlir --lower --seam --seam-check   # check every firing with the
                                 #   solver (--solver): premises => lhs = rhs
                                 #   on its actual terms (store-maps
                                 #   expanded); A-INB firings get the layout
                                 #   and the in-bounds conditions of the
                                 #   accesses the rule compares (this checks
                                 #   the rule against its stated premise; it
                                 #   does not discharge A-INB).
                                 #   --seam-check=N: N firings, evenly
                                 #   spaced; --seam-check-timeout=<s> (10)
  sea-vc p.mlir --ackermannize=on  # reads of read-only array constants
                                 #   (nd.<site> memories, memhavoc arrays:
                                 #   used only under select) become fresh
                                 #   constants ack.<array>.<k> with
                                 #   constraints i_k = i_l -> r_k = r_l
                                 #   (index = S + c, S the non-numeral
                                 #   summands: same S and c: one constant;
                                 #   same S, different c: no constraint).
                                 #   auto (default): on when the query (after
                                 #   the boundary) has no store, not with
                                 #   --solver=bitwuzla (slower measured);
                                 #   off: never.
                                 #   The model's arrays are rebuilt from the
                                 #   reads (0 at unread cells) for replay.
                                 #   docs/design/Ackermann.md
  sea-vc p.mlir --seam --seam-wide --seam-words=all|auto [--words-check]
                                 # initial byte memories (nd.<site>,
                                 #   memhavoc arrays) only read in the query
                                 #   become 8-byte word arrays <A>.w8 at the
                                 #   boundary, exactly (aligned word reads:
                                 #   one read; others: pieces of the words
                                 #   touched, a canonical masked base per
                                 #   symbolic part). A load's `align` >= 8 is
                                 #   trusted for the one-read form (counted;
                                 #   wrong annotations give a sat that replay
                                 #   rejects: warning vc.words-align-trusted).
                                 #   auto: arrays whose aligned word reads
                                 #   cover the other bytes read. --words-check:
                                 #   z3 check of the word query against the
                                 #   byte query (exit 2 if they differ).
                                 #   docs/design/SeaM-initial-words.md
  sea-vc p.mlir --seam --ack-stride=false   # (default on) the stride rule in the Ackermann
                                 #   lowering: reads at sea.ptr.index nodes
                                 #   with a promise (inbounds / nusw / nuw:
                                 #   trusted front-end facts) over one base
                                 #   whose offsets never meet (gcd of the
                                 #   strides does not divide the constant
                                 #   difference) get no constraint; a read
                                 #   also reached through a plain address is
                                 #   not eligible. A dropped constraint only
                                 #   weakens the query: unsat is sound with
                                 #   no premise; a sat model that breaks a
                                 #   promise is detected before the result
                                 #   and the run repeated without the rule
                                 #   (vc.ackermann-stride: fallback). Also
                                 #   turns on the K writers in SeaM: a byte
                                 #   of a wide access at a promised node is
                                 #   that node with K = k (needs A-INB).
                                 #   docs/design/PtrIndex.md sections 0b, 5
  sea-vc p.mlir --ptr-mode=opaque     # the pointer model (default auto:
                                 #   address mode iff the program has an
                                 #   int-to-ptr; opaque refuses one,
                                 #   vc.int-to-ptr). Reported as
                                 #   vc.ptr-mode / vc.int-to-ptr (program
                                 #   ops) / vc.ptr-from-int-opaque (with
                                 #   --seam: addr.from_int encoded as a
                                 #   pointer with no provenance, address
                                 #   mode only) (docs/design/PtrSort.md)
  sea-vc p.mlir --vc-typecheck=false  # (default on) type-check the whole
                                 #   query before the solver: an
                                 #   ill-typed term is vc.ill-typed (a VC
                                 #   bug); prints vc.typecheck-time
  sea-vc p.mlir --lower --check-align      # discharge jobs of trusted
  sea-vc p.mlir --lower --check-ptr-index  #   front-end facts: assert every
                                 #   `align` / every used GEP no-wrap
                                 #   promise, drop the program's asserts;
                                 #   unsat = they hold (one job at a time)
  sea-vc p.mlir --lower --range-fold=on  # fold a sea.icmp / i1 op /
                                 #   select condition the integer ranges
                                 #   settle (InferIntRangeInterface walk,
                                 #   intersected with the simple bounds
                                 #   `icmp x, C` of dominating assumes;
                                 #   no memory reasoning: a load is a
                                 #   leaf). Off by default (untuned).
                                 #   Prints vc.range-fold: mode= bool=
                                 #   checks= depth-cap= empty= facts=
                                 #   facts-skipped= facts-used= icmp=
                                 #   no-facts= queries= select=
                                 #   wrap-flags= (folds whose walk met an
                                 #   op with nsw / nuw: they use the flag
                                 #   semantics, which the VC encoding
                                 #   over-approximates)
  sea-vc p.mlir --lower --range-fold=check  # its paranoid job: assert
                                 #   every fold instead, drop the
                                 #   program's asserts; unsat = every
                                 #   fold valid (docs/design/RangeFold.md)
  sea-vc p.mlir --lower --seam --expand-memmove=K  # a memmove of a
                                 #   constant length <= K bytes as copies:
                                 #   loads of every source byte, then
                                 #   stores in address order (exact). For
                                 #   mcopy reads SeaM cannot decide (symbolic
                                 #   destination): Ackermann residue.
                                 #   Pays for short copies (up to ~16
                                 #   bytes measured), loses for long ones
                                 #   (one load + store per byte).
                                 #   Untuned default: 0 (off). Prints
                                 #   vc.expand-memmove: k= expanded= bytes=
                                 #   kept-symbolic= kept-over-k=
                                 #   kept-word-layout= kept-abstract=
                                 #   kept-tracked= kept-companion-mirror=
                                 #   kept-ptr-companion= (K in index
                                 #   units: bytes in byte memories; every
                                 #   memmove kept with --ptr-companion)
  sea-vc p.mlir --seam --seam-stride  # R-STRIDE: store-map entries
                                 #   skipped for reads at promised GEP
                                 #   nodes whose offsets never reach them
                                 #   (premise.ptr-index; --seam-check proves
                                 #   each firing under P1-P3). Off: on vcc
                                 #   it slows the solver (PtrIndex.md 0c)
  sea-vc p.mlir --ack-dump-pairs=<f>  # with the lowering on: every lowered
                                 #   read (`read <a> <k> <index> loc=<l>`)
                                 #   and every constrained pair (`pair <a>
                                 #   <k> <l>`) to <f>, for a census of
                                 #   undecided pairs; loc= (last field) the
                                 #   loads the read came from, `;`-separated
                                 #   (- for none: a select a later stage
                                 #   made); split the line at its last
                                 #   ` loc=`
  sea-vc p.mlir --lower --seam --vcgen-only --arith-stats  # the products,
                                 #   divisions and remainders of the query
                                 #   the solver gets, by operand kind
                                 #   (numeral: linear; ite-num / ite-sym: an
                                 #   operand is path-merged, ite / concat of
                                 #   ites, leaves numerals or not; nonconst:
                                 #   two non-constant terms) and per source
                                 #   location: a
                                 #   product by a path-merged value (ite-*)
                                 #   is cheap per path and can be a solver
                                 #   cliff as one term (try --split on its
                                 #   condition); see OUTPUT
  sea-vc p.mlir --vc-join=eq     # joins as fresh constants with guarded
                                 #   equalities (default ite: a block
                                 #   argument is ite over the incoming edges,
                                 #   as SeaHorn's --horn-vcgen-use-ite)
  sea-vc p.mlir --vc-join-cases  # experimental: each ite join built as a
                                 #   cases term and expanded at once (same
                                 #   query); prints the cases.* counters
  sea-vc p.mlir --vc-join-cases --vc-cases-reorder [--vc-cases-drop-default]
                                 # experimental, changes the query: merge
                                 #   pairs of a join with equal values (the
                                 #   moved pair's condition conjoins the
                                 #   negations of the conditions it skips);
                                 #   drop pairs equal to the default (their
                                 #   negation conjoined into later pairs)

OUTPUT (stdout, `key: value` lines):
  tool.version: sea-mlir <x.y.z> (<git describe>)
                                  (first line, always; with --smt2 on
                                  stderr; --version: also LLVM and solvers)
  vc.config: <path>               (--config: each config loaded, in order,
                                  first; with --smt2 on stderr)
  program: @<name>
  vc.box-all: dead=<n> empty=<n> expand-max=<K> expanded=<n> guarded=<n>
      lowered=<n> nondets=<n>     (--lower, a program with sea.box.all:
                                  ops erased unused, constant empty boxes
                                  (true), --box-all-expand, boxes expanded
                                  at every index, boxes entered behind a
                                  non-empty test, ops lowered to one index,
                                  index nondets made;
                                  docs/design/BoxAll.md)
  vc.fold: assume-false=<n> assert-false=<n>   (the lowering folded an
                                  assume / assert of constant false:
                                  warnings fold.assume-false (a proved
                                  check or vacuity, not told apart) and
                                  fold.assert-false, also from
                                  sea-merge-asserts for an assert of
                                  constant 0 that is not the only assert
                                  right before the halt, or has a source
                                  location (SeaHorn's unlocated error
                                  marker `assert false; halt` is not
                                  reported); only if one fired). Not
                                  reported: an assert that becomes
                                  constant only in the lowering's
                                  canonicalize (icmp ne x, x); the
                                  verdict still shows it. Counts
                                  of the whole file: every program the
                                  lowering ran on, not only --program's
  vc.statically-safe: no reachable sea.halt ...   (no path reaches the
                                  assertion: it cannot fail; the solver
                                  still answers, unsat)
  vc.ptr-companion: loads=<n> stores=<n> copies=<n> fresh=<n>
                                  (--ptr-companion: split pointer loads read
                                  through the companion, pointer stores /
                                  memmoves it records, fresh companion
                                  arrays)
  vc.ptr-companion-arrays: <n>    (companion arrays the query still reads)
  cases.<counter>: <total> scalar=<n> memory=<n> companion=<n>
                                  (--vc-join-cases, experimental; stderr
                                  with --smt2: joins, arity.<n>, values,
                                  distinct-values, with-dup,
                                  redundant-values, all-equal,
                                  adjacent-equal-pairs, ite-folded-pairs,
                                  last-equal-default, nonadjacent-redundant,
                                  with-nonadjacent, default-not-most-frequent;
                                  with --vc-cases-reorder or
                                  --vc-cases-drop-default also reorder-joins
                                  (joins changed), reorder-merged-pairs,
                                  reorder-merged-adjacent,
                                  reorder-merge-negations,
                                  reorder-dropped-default,
                                  reorder-drop-negations, reorder-pairs
                                  (pairs left); include/sea/VC/JoinCases.h)
  seam.ptrint: assumed loads=<n>  (--ptr-companion=assume: loads resting on
                                  A-PTRINT; checked loads=<n> with =check,
                                  then `seam.ptrint: holds` (unsat) or
                                  `not discharged` (sat: some split load may
                                  read bytes that are not the companion's
                                  pointer, e.g. a cell no pointer was stored
                                  to) after the result)
  object: name=obj.<seg> order=<n> kind=<k> base=<a> size=<s> align=<a>
                                  (--seam: the base registry, allocation order)
  vc-time: <seconds>              (generating the VC: encoding, with --seam
                                  rewriting every load and store, and
                                  building the query; with --smt2 on stderr)
  solve-time: <seconds>           (the solver's check, not with --smt2)
  vc.ackermann: on|off            (with on: vc.ackermann-arrays / -kept /
                                  -reads / -pairs / -numeral-pairs /
                                  -offset-pairs / -residue-pairs /
                                  -leaf-pairs (ites leaf by leaf, layout
                                  values of the objects) / -stride-pairs
                                  (--ack-stride: trusted GEP promises) /
                                  -constraints: arrays lowered and kept,
                                  distinct reads, read pairs, pairs of two
                                  different numerals, pairs S + c1, S + c2
                                  with c1 != c2, and pairs whose symbolic
                                  difference is a multiple of 2^t while
                                  c2 - c1 is not (no constraint: never
                                  equal), constraints added; with vc-time,
                                  stderr with --smt2)
  vc.ackermann-model: defaulted=<n> inconsistent=<n>
                                  (sat, lowering on: reads at indices the
                                  query does not read (cell 0; any value
                                  satisfies the query), and read pairs the
                                  model gives equal indices but different
                                  cells: a lowering bug, error
                                  vc.ackermann-inconsistent, status 2)
  words.<counter>: <n>            (--seam-words: candidates, eligible,
                                  chosen, translated, word-reads,
                                  aligned-proved, aligned-by-annotation,
                                  aligned-partial, masked-reads,
                                  masked-parts; with the SeaM counters)
  words.array: name=<A> chosen=0|1 selects=<n> wselects=<n>
               aligned-words=<n> other-bytes=<n> [reason=<why not>]
  words.check: ok|failed|unknown defs=<n>   (--words-check)
  words.model: decoded=<n> defaulted=<n>    (sat with words: bytes read
                                  back from words; defaulted: a word with
                                  no value, byte 0)
  vc.dag-size: <n>                (--dag-stats: distinct nodes of the query
                                  the solver gets; with --smt2 on stderr)
  vc.dag-op: <op> <n>             (--dag-stats: distinct nodes per operator)
  arith.total: ops=<n> attributed=<n> unattributed=<n> locations=<n>
               shared=<n>          (--arith-stats; with --smt2 on stderr:
                                  the distinct bvmul / bvudiv / bvsdiv /
                                  bvurem / bvsrem / bvsmod / mul-overflow
                                  predicate nodes of the query the solver
                                  gets; unattributed: made or rewritten by a
                                  stage after the encoding (SeaM rules,
                                  the boundary's expansions, e.g. address
                                  arithmetic of ptr.index); shared: nodes
                                  of several locations, counted at each)
  arith.op: op=<op> width=<w> operands=numeral|ite-num|ite-sym|nonconst
            count=<n> unattributed=<n>[ conds=<n> conds-max=<n>]
                                  (per operator, operand width and kind,
                                  numeral operands dropped first (n-ary
                                  products): numeral = at most one operand
                                  left; else ite-num = an operand is
                                  path-merged (its value tree, through ite
                                  branches, concat, extract, extensions,
                                  bvneg, bvnot, holds an ite) with numeral
                                  leaves only; else ite-sym = one is
                                  path-merged; else nonconst. ite kinds:
                                  conds = the distinct ite conditions of the
                                  group, conds-max = the most of one node)
  arith.loc: op=<op> width=<w> operands=<kind> count=<n>[ conds-max=<n>]
             loc=<file:line:col>[ inlined-at=..][ name=..]
                                  (per source location of the sea op that
                                  made the node: mul / div / rem / umulo /
                                  smulo / ptr.index; loc is the last field)
  ite.values: kind=mem|addr|int values= over-guards= sites= listed= dropped=
    leaves-max=                   (--ite-stats, after the run's first
                                  encoding, before any solver; with or without --vcgen-only
                                  and --seam; not with a split: values of
                                  the kind whose term (as encoded) is an ite
                                  tree (addr: or ptr-from-bits of one).
                                  mem: memory values; addr: addresses; int:
                                  integers in the backward slice of an
                                  operand of mul / udiv / sdiv / urem / srem
                                  / umulo / smulo / sdivo, through integer
                                  and i1 ops and block arguments, not
                                  through memory. over-guards: values with
                                  a block guard among the atoms of their
                                  conditions; sites: distinct locations)
  ite.site: kind= values= leaves= conds= guard-atoms= name= loc=
                                  (per location of a kind, most leaves
                                  first, at most --ite-stats-max (40; 0:
                                  all); leaves / conds / guard-atoms: the
                                  most over its values of distinct leaves,
                                  distinct ite conditions, block guards
                                  among the conditions' atoms; name: its
                                  first value, a --split-leaves selector;
                                  a block argument without a position has
                                  its block's first op's)
  ite.loads: loads= dynamic= ite= ite-index= from-bits= symbolic= sites=
    listed= dropped=              (--ite-stats: loads the encoding met;
                                  dynamic: address term with a free
                                  constant (numerals and object bases are
                                  ground), by kind: ite (an ite tree, or
                                  ptr-from-bits of one), from-bits (a
                                  pointer read back from memory), ite-index
                                  (an ite below the top: an index chosen by
                                  a path or a select), symbolic (other))
  ite.load-site: kind= loads= loc=  (per kind and location, most first, at
                                  most --ite-stats-max)
  result: sat|unsat|unknown|none  (none: --vcgen-only, --split-candidates)
  hint: split: open-aliases=<n>[ ackermann-constraints=<m>]; rerun with
                                  --split-candidates ...
                                  (unknown, --seam, not a split, n > 0 open
                                  alias questions in the normalized query,
                                  or n = 0 and m > 0 constraints of the
                                  Ackermann lowering (then shown): where to
                                  look next; jsonl kind hint)
  solve-timeout: true             (unknown because --timeout expired)
  mem-limit: hit                  (unknown because --mem-limit was hit
                                  (RLIMIT_AS: address space, not resident
                                  memory, so it can hit before the resident
                                  size reaches the limit):
                                  z3's memory-out; an allocation that
                                  fails (operator new: the VC, Bitwuzla's
                                  C++; LLVM containers: llvm bad-alloc
                                  handler) prints result: unknown and this line
                                  and exits 3; Bitwuzla: an unknown within
                                  the time under --mem-limit. Not detected:
                                  Yices exits on its own out-of-memory
                                  (status 16, "Out of memory"), and GMP
                                  aborts)
  mem-limit-phase: <phase> [run=<n>]   (after mem-limit: hit: the phase
                                  running, the last vc.phase start; none:
                                  before the first phase; solve: the
                                  solver's own limit)
  mem-limit-progress: time= blocks= blocks-total= ops= ops-total=
      nodes-made= [rss-mb= vm-mb=] peak-rss-mb= [slowest-op=
      slowest-op-time= slowest-op-at=] [biggest-op= biggest-op-nodes=
      biggest-op-at=]             (a hit in encode, after a progress tick:
                                  the counters of the last tick, see
                                  PROGRESS; up to --progress-interval old)
  mem-limit-op: <op> ops=<n> at=<where>   (a hit in encode: the op being
                                  encoded, n-th in encoding order; where:
                                  file:line:col, else the location name,
                                  else -; bytes other than printable
                                  ASCII, " \ and space shown as ?)
  nondet: site=<n> callee=<f> width=<w> value=<unsigned>   (sat: from the model)
  memread: site=<n> addr=<a> width=<w> value=<v> [obj=&<o>[<off>]]
                                  (sat: cells from the model; obj: the
                                  placed object the cell lies in)
  replay: <interpreter status, expected assert-failed>   (with
                                  --ptr-companion=check any status: the
                                  property is A-PTRINT, below)
  replay: assert-at: <where>   (after `replay: assert-failed`; see
                                  LOCATIONS: where the failing condition
                                  is computed. A computed condition: its
                                  defining op, or through a chain of
                                  sea.and the first false conjunct's (the
                                  original assert's condition under
                                  sea-merge-asserts). A constant or
                                  block-argument condition (SeaHorn's one
                                  `assert false`): the last executed op
                                  before the assert with a file location
                                  (a branch, or the condition; source
                                  locations over named ones over .mlir
                                  positions; print ops never), which
                                  names the C check that failed. Only the
                                  program can name the reach site of an
                                  `assert false`: an exporter that leaves
                                  the error branch and the assert
                                  unlocated leaves the last op before
                                  them, maybe inside an inlined callee)
  at: <loc of the op that ended the replay>   (a failing assert: the
                                  original assert under sea-merge-asserts,
                                  its whole MLIR location)
  replay: assert-op: <where>   (see LOCATIONS; the failing sea.assert's
                                  own; the original one under
                                  sea-merge-asserts)
  replay: assert-path: <loc> <- <loc> ...    (up to 5 distinct locations of
                                  executed branches, most recent first;
                                  each <file:line:col>[ inlined-at=..],
                                  see LOCATIONS. In a program with source
                                  locations only those: a sea.cond_br
                                  without one shows its condition's, else
                                  that of the last op before it in its
                                  block; else, like an unlocated sea.br,
                                  it is left out)
  print: ... / print.object: ... / print.note: ... / print.truncated: <n>
                                  (sat: the program's print ops, run on the
                                  model's inputs after the replay status;
                                  see PRINTS)
  validate: checked=<values> mismatches=<n> dontcare=<n>   (dontcare: values
                                  the model leaves unconstrained, its
                                  evaluation a term: not compared; a nondet
                                  or memory cell so left replays as 0,
                                  origin default)
  interp.earlier-version-loads: <n>   (loads of an earlier memory version,
                                  Mem-SSA no-clobber claims: checked by
                                  validate, so counted here instead of
                                  the interp.load-earlier-version warning)
  mismatch: <value> <loc> interp=<v> model=<v|none> [obj=&<o>[<off>]]
                                  (one per mismatch; obj: the placed object
                                  an address value lies in)
  replay: no-clobber-violated: <file:line:col> (...)   (after the
                                  mismatches: a load of an earlier memory
                                  version whose value mismatches, or that
                                  a mismatch depends on: the in-place
                                  interpreter read a cell overwritten since
                                  the version; the program's Mem-SSA
                                  no-clobber claim (Memory.md) fails there,
                                  not the VC; exit 2, vc.replay-mismatch
                                  names the claim)
  seam.ptrint-violated: <loc> addr=<q> [obj=&<o>[<off>]] bits=<b> stored=<p>
  seam.ptrint-unstored: <loc> addr=<q> [obj=&<o>[<off>]] bits=<b>
                                  (--ptr-companion=check, sat: a split
                                  pointer load of the replayed run whose
                                  A-PTRINT guard fails: the pointer last
                                  stored at q (its bits p) is not the
                                  loaded bits b (violated), or no pointer
                                  was stored at q (unstored: A-PTRINT names
                                  none); loc the sea.addr.from_bits.
                                  The replay keeps the stored pointers as
                                  the VC's companion does)
  seam.ptrint-replay: loads=<n> holds=<n> violated=<n> unstored=<n>
                                  (--ptr-companion=check, sat: the split
                                  pointer loads the run executed; the sat
                                  is confirmed (exit 0) iff one fails, else
                                  vc.replay-no-failure, exit 2)
  replay.original: status=<s> at=<file:line:col|-> match=yes|no|undetermined
      nondets=<n> cells=<n> defaulted=<n> unmapped=<n> [box-all-false=(<k>..)]
      [earlier-version-loads=<n>]   (box-all-false: the first false index
                                  of the input's first false sea.box.all;
                                  the lowered program's nondet
                                  box.all.<tag>.<d> is the trip number t
                                  of dimension d, index = lo + step*t)
                                  (--replay-original, sat confirmed by the
                                  replay above: the input program as it
                                  was before any pass, run on the model's
                                  inputs (nondets by site; a cell
                                  <object>+<offset> at the object's placed
                                  base; widened words split little-endian),
                                  deferring memory-safety violations.
                                  match=yes: it fails the same assertion
                                  (status assert-failed, or memory-error
                                  that reached it): the same id, which
                                  each sea.assert gets before the clone
                                  (attribute sea.assert_id, kept by
                                  sea-merge-asserts on its branches); an
                                  assert without one by its location, if
                                  no other
                                  assert has it. defaulted: inputs the
                                  model leaves unconstrained, unmapped:
                                  sites (kind, number, callee / name) or
                                  objects the lowered program has not;
                                  both read as 0. undetermined: the
                                  interpreter cannot run it (ptr.to_int
                                  before placement, ...) or hits the step
                                  limit (a large sea.box.all), it loads an
                                  earlier memory version, it read an
                                  unmapped input, or the assert cannot be
                                  told apart: warning
                                  vc.replay-original-undetermined, exit
                                  unchanged. no: exit 6, or 4 after a
                                  memory-safety violation. Not a match:
                                  `replay.original: expected: <loc>
                                  [assert=<id>]`, `got:`, `message:` lines
                                  follow)
  replay.original: skipped (<reason>)   (no lowering ran, a check job, or
                                  --ptr-companion=check)
  assume.memhavoc-null-noop: vc sites=<n>   (n > 0: --lower of an abstract
                                  program guarded n memhavocs whose pointer
                                  may be null (sea-guard-null-memhavoc): the
                                  VC runs them only when it is not null, as
                                  the interpreter; --memhavoc-null=error:
                                  no guard, the VC havocs from address 0)
  assume.memhavoc-null-noop: uses=<n>   (after replay.original or
                                  seam.inb-violation, n > 0: memhavocs of
                                  null its run executed as no-ops, an open
                                  semantics decision; BRUNCH_STAT
                                  assume.memhavoc-null-noop-uses;
                                  --memhavoc-null=error: a memory-safety
                                  error)
  assume.addr0-is-null: uses=<n>   (after replay.original, n > 0: steps
                                  of its run that rested on the named
                                  assumption addr0-is-null, address 0 is
                                  null: zero bytes loaded as a pointer,
                                  null's bytes as an integer, ptr.to_int
                                  of null; --addr0-is-null=false: they
                                  stop the run, undetermined)
  assume.addr0-is-null: vc null-uses=<n>   (--lower: placement puts the null
                                  object at address 0, so the VC
                                  identifies null with address 0; n: the
                                  uses of its segment. A fact about the
                                  target, discharged by no check; the
                                  replays' own uses: `uses=<n>` lines)
  assume.a-inb: drop-fresh-memhavoc=<n>   (n > 0: --lower of an abstract
                                  program dropped n memhavocs of fresh
                                  objects whose no-op rests on A-INB (an
                                  update of another object, or the
                                  memhavoc's range, could reach the object
                                  after placement); also without --seam and
                                  with --seam-inb=0 (`seam.inb: off
                                  (lowering: ...)`); a separate
                                  memory-safety check discharges it)
  assume.memset-bound: count=<n> bound=<K>   (n > 0: --lower without --seam
                                  expanded n symbolic-length memsets /
                                  memmoves with assume(len <= K), the
                                  sea.assume ops marked sea.memset_bound; an
                                  unsat holds only for lengths <= K; K the
                                  largest bound; with --smt2 on stderr)
  seam.inb: assumed accesses=<n> conditions=<m>
  seam.inb: off [(fallback: an access leaves its object)]
      [(lowering: drop-fresh-memhavoc=<n> rests on A-INB)]   (the VC runs
                                  without A-INB, the lowering's dropped
                                  memhavocs still rest on it; after a
                                  fallback, warning
                                  vc.seam-inb-fallback-conditional)
  vc.seam-inb-violated: at <file:line:col> <op>: <reason>
                                  (--seam, before the run restarts without
                                  A-INB; the first 8 definite violations:
                                  op load / store / memset / memmove /
                                  companion, reason the check that failed:
                                  numeral offset outside, range outside,
                                  object of size 0 or smaller than the
                                  access, ARM range outside; with --smt2 on
                                  stderr)
  vc.seam-inb-violated-count: <n> (all definite violations)
  seam.alias.open.<kind>: <n>     (--seam-stats: the alias questions the
                                  rewriter left to the solver, IS_OVERLAP
                                  terms of the query; kind all,
                                  same-object, diff-object, one-unknown,
                                  both-unknown: what the views of the two
                                  pointers say, per leaf pair of an ite
                                  pointer, the least decided pair (in that
                                  order, diff-object first); many-leaves:
                                  more pairs than the decisions look at;
                                  same-object: also the offset compares
                                  one-object questions reduce to)
  seam.alias.open-pairs.<kind>: <n>  (--seam-stats: the leaf pairs of the
                                  IS_OVERLAP questions, by kind)
  seam.alias.open-memop.<kind>: <n>  (--seam-stats: the open in-tests
                                  (idx - d) <u n of reads over memset /
                                  memmove / memhavoc terms, one per leaf
                                  pair left open; kinds as above, of the
                                  read and destination pointers)
  seam.alias.same-object-reduced: <n>  (--seam-stats: one-object alias
                                  questions asked, decided or not)
  seam.ptr.from-bits-left.<kind>: <n>  (--seam-stats: loaded pointers no
                                  rule resolved, by what they hold)
                                  (--seam: the assumption the answer rests
                                  on; conditions = accesses whose in-bounds
                                  condition is not constant. Assumed, not checked: an
                                  unsat holds if every access stays in its
                                  object)
  seam.rewrite-time: <seconds>     (--seam-stats: the part of vc-time spent in
                                  the rewriter: the thesis' pre-processing)
  seam.simplify-time: <seconds>    (--seam-stats: the final pass of the
                                  normalization over the side conditions and
                                  the query, also part of vc-time)
  vc.<phase>-time: <seconds>       (--seam-stats: the parts of vc-time, in
                                  order: setup (factory, names, SeaOpSem),
                                  encode (genVc; of it vc.exec-time the ops
                                  and vc.join-time the block guards, edges
                                  and block arguments), simplify-sides,
                                  mkquery, simplify-query, solver-term (the
                                  boundary: store-maps expanded, memory-
                                  operation terms translated), unique-names,
                                  ackermannize)
  seam-trace: id=<n> rules=<r,...> premises=<A-INB,LAYOUT,ALIGN,PTR-INDEX|none> op=<kind>
      [loc=<file:line:col>] lhs=<node id> rhs=<node id>   (--seam-trace,
                                  stderr; then seam-trace-lhs: / -rhs: with
                                  the printed terms, cut at 400 characters)
  seam-check: firings=<n> checked=<n> valid=<n> invalid=<n> unknown=<n>
      unexpressible=<n>           (--seam-check, after the verdict; checked =
                                  valid + invalid + unknown; unexpressible: a
                                  firing whose premise cannot be stated, e.g.
                                  an A-INB firing while an address is
                                  normalized, or one the boundary cannot
                                  translate: warning seam.check-unexpressible,
                                  never counted valid. A memory-valued firing
                                  over a memory-operation term is compared
                                  at a fresh index seam-check.cell)
  seam-check-invalid: id=<n> rules=<r,...> premises=<p> op=<kind> loc=<loc>
  seam-check-lhs: <term> / seam-check-rhs: <term>
  seam-check-smt: <line>          (the SMT-LIB of the failed check: sat means
                                  the rule's result differs from its input)
  seam-check-unknown: id=<n> rules=<r,...>    (timeout or solver unknown)
  seam.<counter>: <n>              (--seam-stats, before `result:`; with
                                  --smt2 on stderr): rule-calls, rules-fired,
                                  cache-hits, ..., and seam.rule.<rule>: the
                                  times each rule fired (row.store-hit,
                                  row.store-skip, cmp.diff-base [A-INB],
                                  cmp.same-base, add.normalize, ...); every
                                  rule behind a decision is counted, not only
                                  the first; seam.rule.premise.a-inb: the
                                  rewrites that depend on A-INB (0: the answer
                                  does not use it), seam.rule.arm.skip-diff-base:
                                  the ARM skips across two bases)
Names in the query are the names `sea-opt --mlir-use-nameloc-as-prefix`
prints (%x, ^bb1); nondet constants are nd.<site>; with --seam an
object base is obj.<segment name>: obj.<kind>.<site or name>,
obj.nondet.<site> for a ptr.nondet, and obj.null (size 0, address 0) for
sea.ptr.null (docs/design/SeaM-null.md).

MEMORY: array encoding (single-cell load and store, address ops; memset,
memmove and memhavoc with a constant length become store chains; a
symbolic length is an error: --lower bounds memset / memmove lengths by
sea-expand-memset, assume(n <= 16), warning memset.symbolic-bound, line
assume.memset-bound). With
--seam, byte memories get memory-operation terms instead (exact, any
length; docs/design/SeaM-memops.md); word memories (--widen-memory) keep
the constant-length store chains. Error vc.memop-term-at-boundary: a term
in a position that is not read (by design; not produced by sea-vc's VCs).
Addresses are pointers, ptr(w), in every VC but --mem=lambda
(docs/design/PtrSort.md): sea.ptr.index and addr.add are PTR_INDEX nodes,
casts, loads and stores of addresses are explicit markers, eq / ne is
PTR_EQ; all of it is erased to bit-vectors at the solver boundary (error
vc.ptr-sort-at-boundary: a pointer-sorted term reached the solver, a
bug). Accesses spanning several cells and
abstract pointers need lowering first (--lower). Replay runs memory in place, so updates must be `inplace` (else
status 1 after the verdict).

CONFIG (--config=<file|name>, repeatable; docs/Profiles.md): LLVM config
files (as clang --config): options separated by whitespace / newlines, `#`
comments, `@file` includes (nested; relative paths against the including
file's directory; <CFGDIR> is that directory). The options of the configs
are inserted, in order, before the command line's: later configs, then the
command line, win (the last value of an option counts; lists accumulate).
A bare name (no '/', no extension) is <name>.cfg, looked up in order in:
the directories of $SEA_MLIR_PROFILE_PATH (colon-separated), then
<prefix>/share/sea-mlir/profiles (../share/sea-mlir/profiles from the
binary), then the source tree's profiles/ (build trees). Anything else is
a path. Not found: error vc.config-not-found (the searched directories);
unreadable / a bad include: error vc.config-read; status 1. Inside a config,
include another with @file (a --config there is LLVM's own: a path or a
file name in the same directories, not printed). Shipped examples (not
recommended defaults): seam (--lower --seam), seam-wide (@seam.cfg
--seam-wide).

PRINTS (docs/design/Print.md): sea.print / sea.print.mem /
sea.print.scope / sea.print.end show values in the counterexample; the VC
ignores them (the query is byte-identical without them). Weak prints name
what they show (`"x"`: the value with loc("x"), or `nd.<site>`): they
keep nothing alive, and a value the lowering removed prints
`<dangling: x>` (never a guess). Strong prints have SSA operands: kept
alive through the lowering. --print-refs=weak|strong converts the
program's prints first (sea-print-weaken / sea-print-strengthen; default
asis).
Op syntax (weak: names in strings; strong: SSA operands):
  sea.print "t"                             a tag line
  sea.print "t" = "x" : i64 {fmt = "s"}     the value named x (loc("x"),
                                            or nd.<site>: a nondet's result)
  sea.print "t" %x : i64                    strong
  sea.print "t" %s : !sea.struct<i32, i64> {names = ["a", "b"]}
  sea.print.mem "t" = "nd.<site>"["<ptr>"] : i32 {count = 4, offset = 8}
                                            memory: nd.<site> of a
                                            sea.mem.nondet / sea.memhavoc
                                            (the region) or a named memory
                                            value; <ptr>: a named pointer or
                                            address
  sea.print.mem "t" %m[%p], %n : !sea.mem<!sea.ptr<64>, i8>, i64 -> i8
                                            strong (%n: the count)
  sea.print.scope "t" ... sea.print.end "t" nests tags as t.<tag>
print.mem attributes: count (elements, default 1; count_ref = "<name>":
the value of a named integer, capped at 65536), offset (bytes past the
address, signed), fmt. Element types: whole-byte integers
(little-endian), pointers, addresses. Formats (fmt): auto (default:
unsigned decimal below 2^32, else hex; an i1 as a bool), u, s, x, b
(bool), c (char), bits; hexdump (print.mem of i8 only).
Lines (--print=lines, default; tree: indented by scope; off):
  print: <scope>.<tag> = 5 : ui32 | -1 : si32 | 0xff : i8 | true
  print: <tag> = &heap.3[8]       (object + byte offset; `end` one past,
                                  `oob` outside, `freed`; null, &null[8],
                                  &func.f; an address without provenance:
                                  0x1000 or 0x1000 ~heap.3[8], a hint)
  print: <tag> = heap.3[0..20]    (a hex dump: then `<tag> +00: xx .. |ascii|`
                                  rows, `*` for repeated rows; `..` a byte
                                  the model leaves unconstrained, `??` one
                                  no source gives)
  print: <tag>[i] = ...           (print.mem count > 1); `(default)` after
                                  a value read from unconstrained bytes
  print.object: heap.3 kind=heap size=24 [label=<l>] [at=<file:line:col>]
                [inlined-at=..] [at_name=<name>] [base=0x..]   (once, before the first
                                  line using it; at: the allocation)
--print-limit=N (default 1000, 0 none) then print.truncated: <dropped>;
--print-locs appends at=<file:line:col> [inlined-at=..] [at_name=<name>]
of each print op.
Names live in locations: keep them (sea-opt --mlir-print-debuginfo)
between tools.

LOCATIONS (assert-at, assert-op, assert-path entries; print at=): <where> is
  <file>:<line>:<col> [inlined-at=<file:line:col>[,<file:line:col>...]]
  [name=<name>]
The position comes first: a source location if the op has one, else its
position in the input .mlir file; `-` if it has none. An op inlined from
a callee (loc(callsite(<callee> at <caller>)), nested for deeper chains)
shows its innermost position (in the callee), then ` inlined-at=` the
call sites, innermost first, outermost (the entry function's) last:
callsite("f.c":3:5 at callsite("g.c":9:2 at "main.c":20:7)) gives
`f.c:3:5 inlined-at=g.c:9:2,main.c:20:7`. ` name=<name>` follows when
the location has a name: loc("claim.x") gives `p.mlir:154:3
name=claim.x` (the op's line in the input), and
loc("claim.x"("a.py":61:4)) gives `a.py:61:4 name=claim.x`. The name is
the last field and runs to the end of the line. In print and print.object
lines the same is at=<pos> [inlined-at=..] [at_name=<name>]. A fused
location's metadata (the SeaHorn exporter's link
fused<"llvm @f %bb #i">[...]) is neither a position nor a name: only
`at:` (the whole MLIR location) shows it; a fused location without a
file position gets its .mlir position. jsonl: the position in the field
(at, op), the call sites in <field>_inlined (at_inlined, op_inlined: an
array of file:line:col strings, outermost last), the name in
<field>_name (at_name, op_name), each only when present; assert-path:
path (positions) and, when an entry was inlined, path_inlined (an array
parallel to path of call-site arrays, [] for an entry not inlined).

PROGRESS (--progress = --log=vc.phase, --progress=false = vc.phase:0; on
stderr, or the log file; each line flushed as it is written):
`log: vc.phase start phase=<name>` at the start of each phase and
`log: vc.phase end phase=<name> time=<s> <memory> <sizes>` at its end;
phases: lower, encode, simplify (with the rewriter's normalization),
solver-term, ackermann, solve, replay (sat only). A killed run's last
start line names the phase it was in. A run that restarts (the
vc.seam-inb-violated fallback, the --ack-stride retry) repeats them from
encode. <memory>: rss-mb vm-mb (now; Linux only, else omitted)
peak-rss-mb (so far), in MB; vm-mb is what --mem-limit bounds.
Inside encode, every --progress-interval=<s> seconds (default 10,
untuned; 0: after every block; checked before each op and after each
block, so never inside one op):
`log: vc.phase progress phase=encode run=<n> time=<s> blocks=<d>
blocks-total=<n> ops=<d> ops-total=<n> nodes-made=<n> <memory>
[slowest-op=<op> slowest-op-time=<s> slowest-op-at=<where>]
[biggest-op=<op> biggest-op-nodes=<n> biggest-op-at=<where>]`: blocks
encoded of those reachable; ops whose encoding started (no terminators);
the op that took longest and the one that made the most Expr nodes so far
(SeaM's load / store rewriting is part of its op; block joins are in no
op). One op can dominate the phase: these fields name it once it ends.
Under --mem-limit the counters are kept without the log, for the
mem-limit-* lines of a hit (OUTPUT FORMATS).

DIAGNOSTICS (stderr): "Warning: ... [kind]" (yellow) for incomplete or
approximate handling, once per kind; repeats are counted and summarized at
exit (--log=warn.<kind>:2 prints each). "Error: ... [kind]" (red) when
the tool cannot continue.

LOGS (stderr, or --log-file / SEA_LOG_FILE; off by default): SEA_LOG, then
--log=<pattern>[:<level>],... (pattern: a tag, <prefix>.* or *; level 1
flow, 2 object, 3 detail; 0 off); --log=help lists the tags. One line per
event: `log: <tag> [<msg>] k=v ... [at <loc>]`; under --format=jsonl
{"log":"<tag>","level":L,...,"text":"<the text line>"}.
Level-1 (flow) tags, `--log=*:1` for all:
  vc.phase    start phase=<p> / end phase=<p> time=<s> rss-mb vm-mb
              peak-rss-mb + sizes (progress: PROGRESS): lower
              ops-before ops-after; encode nodes-made sides guarantees
              (its start: run=<n>, 2+ after a fallback or retry);
              simplify, solver-term nodes-made; ackermann on arrays pairs
              nodes-made; solve (time only); replay status checked
              mismatches validated. nodes-made = Expr nodes made so far in
              this run (freed ones included; not the DAG size).
  lower.pass  pass=<arg> on=<op> program=@<name> ops-before ops-after
              time, per pass run
              (failed: msg `failed`)
  vc.solve    backend result time nodes-made, per solver call of the
              verdict
MLIR's pass-manager options apply to the lowering: --mlir-print-ir-after-all,
--mlir-print-ir-after-change, --mlir-timing, --mlir-pass-statistics;
--mlir-print-ir-module-scope needs --mlir-disable-threading.

REACH (--reach[=checks|blocks]): the VC without the exit requirement and
without the ban on sea.unreachable blocks; sea-split-at-checks gives each
target a block right before it (no check site in it but its first op), and
the query of a target is check-sat-assuming that block's guard (an assume
before the target holds, one after it does not count). Not with --smt2,
--vcgen-only, --sink-assumes, --ptr-companion=assume|check. A model that
breaks a promise of the stride rule (--ack-stride) reruns every query
without it, as for a counterexample. The records are printed after the
last query (--log=vc.reach:2 shows each as it is answered). Ids: file:line:col of the op (a block: of its first
located op), call sites appended with @ (inlined-at), a position in the
.mlir file by its basename, `unlocated#<k>` without a position, `#<n>` on a
repeat; stable for one input and options. With --check-align /
--check-ptr-index the merged assert of the promises is a target too.
Under --seam each model also runs on the input program with its accesses
checked (as for a counterexample): a run that leaves an object before the
target is replay=memory-error (status 4); inb=undetermined: that run read
a nondet memory cell and decided nothing. Witness headers say which
program they replay on (lowered or input).
Block ids: --reach also tags every block of the input (sea.block_ids =
["<n>"] on its terminator; the passes keep them: merged blocks hold both
lists, a split tail is <last>.s<k>, a new block <last>.<role><k>, an edge
block <a>><b>); R_T keeps them, and --smt2 on a program with them ends the
query with `; sea.block-ids <guard> <id>,...` lines (query-diff's Skolem
hints). A reachable block without ids: warning vc.block-ids-missing.
Targets are those of the lowered program: a site the lowering removed (an
assume of true, the code after an assume of false) is not listed; the
folded assume of false is the sea.unreachable at its location.
Reach sites: --reach tags every sea.assume / sea.assert of the input
before any pass (sea.reach_site = its position in program order; an input
that has the attribute keeps its tags; a tag two checks share is error
vc.reach-site). A target whose op carries one reports site=<k>: the
input check it is (kept), or was (the sea.unreachable of a folded assume;
the sea.assume sea-merge-asserts makes of an assert, reached once the
assert holds). Checks the lowering made have none. Two lowerings of one
input match targets by site (utils/cover-diff.py). With --reach-failures
the failure of a tagged assert is a target too: a reach-site marker (op=sea.print
of=sea.assert.fail site=<k>) in a block of its own, reached iff an
execution reaches the assert with its condition false
(sea-split-assert-failures; for a merged assert, sea-merge-asserts'
landing block). A check of the
constant true (canonicalize erases it) also has cond=true [next=<k>]: k
is the site of the next target in its block, reached iff it is.
  reach.config: kind=checks|blocks solver=incremental|separate rlimit=<n>
                splits=<n> model-runs=0|1 order=program|deepest
                [query=0] [corpus=<n> fuzz=<n>]
  reach: <id> reachable|unreachable|unknown op=<sea.assume|sea.assert|
         sea.unreachable|block> [reason=cfg|rlimit|timeout|guard-free|
         no-query|...]
         [replay=reached|mismatch|memory-error|not-reached|error]
         [inb=undetermined]
         [mismatches=<n>] [witness=<file>] time=<s> at=<loc>
         [site=<k> [cond=true [next=<k>]]]
                                  (one per target, in program order;
                                  reason=cfg: no path from the entry;
                                  replay=reached: the model's run reaches
                                  the target, its values agree up to it)
  reach: <id> reachable op=<op> via=model-run by=<id> [inb=undetermined]
         [witness=<file>] time=0.000 at=<loc> [site=<k>]
                                  (no query: the replay of the model of
                                  target by= reached it; witness: that
                                  model's run)
  reach.summary: targets=<n> reachable=<n> unreachable=<n> unknown=<n>
                 queries=<n> time=<s> [model-runs=<n>]
MODEL RUNS (--reach-model-runs, default on): a sat model's replay is an
execution; every target it reaches (about to execute the op) whose query
is still to come is reachable via=model-run, under --seam only if the
input program's run on the model reaches it inside every object (as for
the model's own target). The cross-check stays: a clean run that reaches
a target the solver answers unreachable is vc.reach-inconsistent.
--reach-order=deepest (default) queries in reverse program order, so a
deep target's model passes the targets before it; =program: in order.
CONCRETE FIRST (--reach-corpus=<dir|file,...>, --reach-fuzz=<N>): before any
query the corpus files run, in order, then N fuzz trials (sea-run --fuzz
mutations), on the program the targets are on (after the lowering and the
splits), with the reach replays' interpreter options; the queries of the
rest use a fresh solver each (--reach-incremental=false) unless
--reach-incremental is given. A run that is about
to execute a target's op reached it (its prefix is an execution with every
earlier assume holding; what it does later does not matter):
  reach: <id> reachable op=<op> via=run by=seed:<file>|trial:<n>
         [inb=undetermined] [witness=<file>] time=0.000 at=<loc> [site=<k>]
                                  (no query; by: the first run that reached
                                  it; witness: that corpus file, or with
                                  --reach-witness the trial's inputs)
  reach.runs: decided=<n> model-run=<n> solver=<n> cfg=<n> unknown=<n>
              trials=<n> seeds=<n> inb-violated=<n> time=<s>
                                  (after reach.summary; inb-violated: under
                                  --seam, reached only by runs that leave an
                                  object first, or leave one and reach the
                                  target by a location other ops share:
                                  left to the solver)
  The deciding runs' assumption uses are in the assume.* uses= lines.
R_T (--reach-programs=<dir>; then exit, no query): per target T, the program
the targets are on with every sea.assert dropped, T's block split before T
and branching to `sea.assert false {sea.reach_target = "<id>"}; sea.halt`,
every other sea.halt a sea.unreachable, unreachable blocks erased: BMC
form when the input is (a target no path reaches: no assert, no halt left,
statically safe). sea-vc on R_T: sat iff T reachable; T's witness fails
R_T's assert in sea-run. Ids on R_T: its assert has T's id (the
sea.reach_target attribute); R_T's other targets are numbered on R_T (#n,
unlocated#k, and the sea.unreachable of each replaced halt is a target).
R_T of a --lower --seam run keeps symbolic-length
memset / memmove: run sea-vc --seam on it.
  reach.program: <id> op=<op> file=<dir>/reach-<k>.mlir at=<loc> [site=<k>]
  reach.programs: <n> dir=<dir>
  The seam.inb / assume.* lines are the assumptions the answers rest on, as
  for a verdict; the reach runs use the safety replays' interpreter options
  (--addr0-is-null, --memhavoc-null), and after reach.summary
  assume.addr0-is-null: uses=<n> / assume.memhavoc-null-noop: uses=<n>
  count their steps that rested on them. An unreachable check site is vacuity or dead
  code; a reachable one has satisfiable preconditions (on A-INB under
  --seam). Errors: vc.reach-replay-mismatch, vc.reach-replay-not-reached,
  vc.reach-inconsistent (a replay that reached its own target, agrees
  with the model and stays in its objects reaches a target the VC answers
  unreachable): bugs, status 2; warning vc.reach-replay-memory-error.
  Status: 0 every target decided and every reachable one replayed to it;
  3 some unknown; 4 a replay stops at a memory-safety error before its
  target (the answer rests on A-INB); 2 a bug as above; 1 error. Not a
  safety verdict.

SPLIT (--split=<sel,...>, --split-leaves=<sel,...>, --split-auto,
--split-candidates; docs/design/Split.md): Shannon cofactoring of the VC,
as experiment and debugging infrastructure (which case of a timeout is
hard). A target is
fixed when the encoding defines it, before SeaM rewrites its uses; each part
is VC[t := value] /\ condition, answered by the usual run (lowering once;
encoding, SeaM, Ackermann, solver, replay per part). --split: an i1 value
(2 parts: 1, 0) or a block guard (true, false). --split-leaves: an address
or an integer (iN, N > 1) whose term is an ite tree (an address: or
ptr-from-bits of one): one part per distinct leaf, condition = the
disjunction of its path conditions (a path-merged column index: one part
per column). Selectors: the VC
name (%x, nd.<site>, ^bb3; names of the lowered program as
--print-lowered=<file> writes it; sea-opt numbers values differently) or
@<file>:<line> (the one op result or block argument of the kind with a file
position, its own or an inlined call site, on that line; an ambiguous one lists every match:
name, op, type, col=; one that finds nothing lists the results of other
types on the line and the nearest lines with results of the kind). Several targets nest (one level per target, in
encoding order, under the cases before it; enumerated depth-first, one run
per part; a part whose levels differ from its plan is vc.split-unstable).
--split-auto (--seam): a solverless pick run ranks the ite-valued addresses
by the open alias questions (IS_OVERLAP of the normalized query) whose
pointers contain the address's term or one of its leaves without an object
(ties: more leaves first; a heuristic: one unknown leaf behind several ites
counts for each), prints the top 5 as split.candidate lines, splits on the
leaves of the first with >= 2 leaves (none: warning vc.split-auto-none,
one unsplit part). --split-check (default on): per level, z3 (rlimit
--split-check-rlimit) proves the case conditions exhaustive and the chosen
case exact (condition => term = value, on the solver's terms: pointers by
address (pointer identity holds by construction and is checked
structurally)); failed: exit 5, vc.split-check-failed; unknown: warning
vc.split-check-unknown. --split-max-parts (64, untuned): more parts is an
error (vc.split-max-parts), no verdict; raise it for nested levels or wide
leaf sets (each part is one encoding and one solver run). --split-max-paths (1024): paths of an ite
tree. --split-stop-on-sat. --split-smt2=<prefix>: <prefix>.part<N>.smt2
per part (with --vcgen-only: nothing solved). Not with --smt2, --reach,
--mem=lambda. Under --seam a split turns on --seam-stats (the per-part
counters). A level with one case (warning vc.split-trivial), a target the
encoding never meets (vc.split-unmet).
Lines: every part prints its usual output between
  split.part-start: <N>
  split.check: level=<i> target=<name> cases=<n> exhaustive=ok|failed|unknown exact=...
  split.smt2: <path>
  split.part-result: sat|unsat|unknown|none   (a part's answer; no result: line)
  split.part: <N> choice=<name>=<case>#<k>/<n>[,...] result=... vc-time=
    solve-time= dag= alias-open= alias-open-pairs= alias-open-memop= rows=
    ack-reads= ack-pairs= ack-constraints= check=ok|unknown|off
  (<case>: 1|0, true|false, leaf:<obj> or leaf:unknown(<op>) (address),
  leaf:<numeral, unsigned> or leaf:term(<op>) (integer), with /paths=<n>; dag: nodes of the solver's query; rows: the row.* counters'
  sum)
then
  split.verdict: <v> parts= unsat= sat= unknown= none= hardest-part=
    hardest-solve-time= check-unknown= check=on|off leaf-structural=
    [stopped=first-sat]
    (check-unknown: levels whose exhaustive or exact check was unknown or
    unexpressible, summed over the parts: > 0 means the construction is not
    fully checked; check=off: --split-check=false; leaf-structural: --split-leaves
    levels whose chosen value was checked structurally)
  split.unknown-part: <N>:<choice>   (each unknown part)
  result: <v>     sat if a part is sat (replayed), unsat if every part is,
                  none under --vcgen-only, else unknown
--split-auto adds split.auto: open-aliases= candidates= vc-time=,
split.candidate: name= leaves= open-aliases= loc=, split.auto-pick: <name>.
--split-candidates (--seam; not with a split, --smt2, --reach, --mem=lambda;
--split-max-paths applies): the pick run of --split-auto, listed; no split,
no solver; then result: none. Start here when the names are unknown (a
--seam run that ends unknown with open alias questions prints
`hint: split: open-aliases=<n>; ...` pointing here, also with
open-aliases=0 ackermann-constraints=<m> when Ackermann constraints are
left). An ite tree with more
paths than --split-max-paths is listed with leaves=0 (warning
vc.split-max-paths).
  split.candidates: open-aliases= leaves-candidates= bool-candidates=
    bool-listed= bool-dropped= bool-values= reads= same-object-open=
    memop-open= vc-time=
    (bool-values: i1 values and guards recorded; bool-candidates: those
    behind an open question or in a read index, one per term;
    bool-dropped: cut by --split-candidates-max; reads: array reads (select,
    index not a numeral) of the query at the solver boundary, the reads the
    Ackermann lowering pairs; open-aliases: IS_OVERLAP questions only;
    same-object-open: one-object questions reduced to an offset compare and
    left open, memop-open: open in-tests of memset / memmove / memhavoc
    reads; neither is attributed. --seam-stats' seam.alias.open.all =
    open-aliases + same-object-open; seam.alias.open-memop.all = memop-open)
  split.leaves-candidate: name= leaves= unknown-leaves= open-aliases=
    direct= own= reads= loc=     every ite-valued address, in
    --split-auto's order (with no open question at all: by reads, then
    leaves); name is a --split-leaves selector. reads: array reads whose
    index holds its term. unknown-leaves: leaves
    without an object; open-aliases as split.candidate; direct: questions
    holding the term itself, not only an unknown leaf of it; own: questions
    no other candidate is behind (own < open-aliases: the count is shared,
    e.g. one unknown leaf behind several ites)
  split.bool-candidate: name= target=value|guard open-aliases= addrs=
    reads= cond-reads= covers= same= loc=
    an i1 value or block guard (name: a --split selector) in an ite
    condition of a question's pointers, of the term of an address candidate
    behind it, or of the index of an array read; open-aliases: those
    questions; addrs: address candidates with it in an ite condition;
    reads: array reads with it in an ite condition of the index (anywhere
    in the condition); cond-reads: of those, reads with an ite whose
    condition is it; covers: other candidates in its Boolean structure (an
    and covers its compares); same: other values with the same term, listed
    with the one with the most uses that reach a select or branch
    condition or an address, through and / or / xor / i1 selects / i1
    block arguments (a split replaces that op's value only: uses of a
    same= value keep the term).
    Ranked by open-aliases; ties: addrs, then encoding order; among
    open-aliases=0: reads, cond-reads, covers, addrs, encoding order. Only
    those behind a question or in a read index; at most
    --split-candidates-max (20; 0: all). A guard's loc is its first op's.
  split.arith-candidates: arith-nodes= int-values= candidates= listed=
    dropped=
    (arith-nodes: bvmul / bvudiv / bvsdiv / bvurem / bvsrem / bvsmod and
    the multiplication overflow predicates of the query at the solver
    boundary; int-values: integer values (iN, N > 1) whose term is an ite
    tree; candidates: those and the i1 values / guards controlling at least
    one node; dropped: cut by --split-candidates-max)
  split.arith-candidate: name= target=int|value|guard arith= mul= div=
    direct= feeds= cases= same= loc=
    a value controlling arithmetic: an int (a --split-leaves selector)
    counts a node with, in an operand, its term or an ite whose condition's
    atoms are all atoms of its tree's conditions (the normalization folds
    a compare of the term into its conditions); an i1 value or guard (a
    --split selector) counts a node with it in an ite condition of an
    operand. mul / div: of arith, products (and overflow predicates) /
    quotients and remainders; direct: (int) nodes with the term itself in
    an operand; feeds: (int) other int values whose term holds this one's;
    cases: leaves (int) or 2; same: as split.bool-candidate. Ranked by
    arith, then ints first, then feeds, then direct, then cases, then
    encoding order (untuned); at most --split-candidates-max. Values that
    share the paths of a merge share the count (over-attribution).
  All rankings are heuristics (cheap counts, not a measure of hardness);
  reads is for queries whose residue is Ackermann pairs, not open alias
  questions; arith for queries whose cost is products or quotients by
  path-merged values.

OUTPUT FORMATS (--format=text|jsonl; default text, the lines above):
jsonl: every stdout line is one JSON object
  {"kind":"<kind>", <fields>..., "text":"<the text line(s)>"}
`text` is the text format's rendering, so `jq -r .text` gives the text
output (timings aside). Integers are JSON numbers when below 2^53, else
decimal strings; field order is stable. Diagnostics stay on stderr as text
(no diag records). --agent prints this guide as text whatever --format.
Objects: one id in every record, the print notation (stack.1, heap.3,
global.g): object.id, print.object.id, and obj in print / print.mem /
memread / mismatch; join on it. Addresses (base, addr) are numbers.
Kinds and fields:
  stat          key, value (a number when the text is one), plus one field
                per k=v of the line: every `key: value [k=v ...]` line
                that is not below (vc-time, seam.*, vc.ackermann-*, xrt.*,
                seam.inb: value=assumed accesses= conditions=, seam-check:
                firings= ..., vc.ptr-companion: loads= ...); a field named
                kind/text/value twice gets a `_` suffix. BRUNCH_STAT lines
                are stat records with "brunch":true
  line          text only: a line not of the form `key: ...`
  program       name
  object        id (<seg>), symbol (the VC constant obj.<seg> of the text's
                name=), order, objkind, base, size, align
  result        value: sat|unsat|unknown|none
  split-part-start part
  split-part    part, choice, result, vc_time, solve_time, dag, alias_open,
                alias_open_pairs, alias_open_memop, rows, ack_reads,
                ack_pairs, ack_constraints, check
  split-verdict result, parts, unsat, sat, unknown, none, hardest_part,
                hardest_solve_time, check_unknown, check (on|off),
                leaf_structural, [stopped]
  split-candidate name, loc, leaves, open_aliases
  arith-total   ops, attributed, unattributed, locations, shared
  arith-op      op, width, operands, count, unattributed, [conds, conds_max]
  arith-loc     op, width, operands, count, [conds_max], loc [loc_inlined]
                [loc_name] (--arith-stats)
  split-candidates open_aliases, leaves_candidates, bool_candidates,
                bool_listed, bool_dropped, bool_values, reads,
                same_object_open, memop_open, vc_time
  split-leaves-candidate name, loc, leaves, unknown_leaves, open_aliases,
                direct, own, reads
  split-bool-candidate name, loc, target (value|guard), open_aliases, addrs,
                reads, cond_reads, covers, same (names, comma-separated;
                - for none)
  split-arith-candidates arith_nodes, int_values, candidates, listed,
                dropped
  split-arith-candidate name, loc, target (int|value|guard), arith, mul,
                div, direct, feeds, cases, same
  ite-values    value_kind (the text's kind=), values, over_guards, sites,
                listed, dropped, leaves_max
  ite-site      value_kind, values, leaves, conds, guard_atoms, name, loc
  ite-loads     loads, dynamic, ite, ite_index, from_bits, symbolic, sites,
                listed, dropped
  ite-load-site addr_kind (the text's kind=), loads, loc
  hint          topic (split), open_aliases, ackermann_constraints (only
                with open_aliases 0), option (--split-candidates)
  nondet        site, callee, width, value
  memread       site, addr, width, value, [obj + off] (the object id and
                the cell's offset in it; text obj=&<obj>[<off>])
  replay        status, [at] [at_inlined] [at_name] (assert-at), [loc]
                (at:), [op] [op_inlined] [op_name] (assert-op), [path]
                (array, most recent first) [path_inlined], [message];
                one record
                for the `replay:` / `at:` / `message:` lines (text has
                several lines)
  validate      checked, mismatches, dontcare
  reach         id, status, op, [via, by], [reason], [replay], [inb],
                [mismatches], [witness], time, [at] [at_inlined] [at_name],
                [site] (--reach; reach.config, reach.summary and reach.runs are
                stat records)
  reach.program id, op, file, [at] [at_inlined] [at_name], [site]
                (--reach-programs; reach.programs is a stat record)
  mismatch      def (`<op> <loc>`), type, interp, model (null: none),
                [obj + off] (an address: the object it points into)
  no-clobber-violated  loc (the load's file:line:col)
  replay-original  (--replay-original) status, at, match, nondets, cells,
                defaulted, unmapped, [box_all_false],
                [earlier_version_loads], [expected,
                got, message] (not a match); skipped: status `skipped`,
                reason; one record for the `replay.original:` lines
  smt2          (--smt2) the query in text (and its `; sea.block-ids`
                lines); the statistics follow as records on stdout (text
                mode: stderr)
  print         path (scope.tag[.leaf|[i]]), [scope], [at], [at_inlined],
                [at_name], [depth] (tree),
                and the value: type (iN|ptr|addr), integers fmt + value
                (signed for fmt s), pointers obj + off [+ state
                end|oob|freed], addresses addr [+ obj/off with provenance,
                else near/near_off for a `~obj[off]` hint], repr (the
                value's text: `100 : si64`, `&heap.3[0]`); error
                (dangling|stale|pointer-bytes|not-a-pointer) + ref,
                unknown (`?`), default (model default) when they apply
  print.mem     (hexdump header) path, obj + lo + hi (or addr_lo/addr_hi
                without provenance), bytes (hex, 00 where unknown),
                unknown / default: [[lo,hi),...] byte ranges, repr
  print.mem.row path, row (one hexdump row as text)
  print.scope   (--print=tree) path, tag, depth
  print.object  id, objkind, size, [label], [at], [at_inlined],
                [at_name], [base], [freed]
  print.note    message
  print.truncated count
jq one-liners:
  sea-vc p.mlir --format=jsonl | jq -r 'select(.kind=="result").value'
  ... | jq -c 'select(.kind=="replay") | {status, at, path}'
  ... | jq -r 'select(.kind=="print") | [.path, .repr] | @tsv'
  ... | jq 'select(.kind=="stat" and (.key|startswith("seam.rule."))) | {(.key): .value}'
  ... | jq -s 'map(select(.kind=="stat" and .brunch|not)) | map({(.key): .value}) | add'

BRUNCH STATS (--brunch-stats): also prints `BRUNCH_STAT <key> <value>` lines
(one token values; brunch collects them into a CSV, utils/bench.py too): result,
vc-time, solve-time, status (the exit status; not for status 1 before the
verdict), seam.inb (assumed|off) with seam.inb-accesses / seam.inb-conditions,
the assume.* lines split the same way (assume.a-inb-drop-fresh-memhavoc,
assume.memset-bound-count / -bound, assume.addr0-is-null vc with
assume.addr0-is-null-null-uses),
and with --seam-stats every seam.* counter and time (also vc.*-time). With
--smt2 they go to stderr with the other statistics. Existing output unchanged.

STATUS CODES: 5 (--seam-check) a rule firing is invalid: a rewriting bug,
the verdict is not to be trusted (takes precedence over 0, 2, 3, 4; not
over 1). 0 unsat, or sat confirmed by replay (the replayed run fails an
assertion; with --ptr-companion=check, whose property is A-PTRINT and not
the assertions, a split pointer load fails its guard: seam.ptrint-violated
or seam.ptrint-unstored, whatever the run's status); 1 error (bad input, form,
unsupported op, replay impossible); 2 sat but replay disagrees with the
model (a value differs, or the run shows no failure of the property:
vc.replay-no-failure): a bug in the VC or the interpreter, or a load of an earlier memory
version whose cell was clobbered (the in-place interpreter sees the new
value, the VC the old one); 3 unknown; 4 (--seam) sat that does not replay
because the program leaves an object with the model's inputs: A-INB does not
hold, the answer is inconclusive, the program has a memory-safety error
(`seam.inb-violation: <message>`, `seam.inb-violation-at: <loc>`; the
abstract program is run with the model's nondet values, and only a run that
reads no nondet memory cell is the model's execution: otherwise
`seam.inb-violation: undetermined (...)` and the replay's own status);
with --replay-original also: the original program's run went past a
memory-safety violation and did not fail the same assertion
(vc.replay-original-memory-error). 6 (--replay-original) sat confirmed on
the lowered program, but the original program on the same inputs does not
fail the same assertion (vc.replay-original-mismatch): the lowering changed
what the program does, a bug in a pass, or a program whose behavior depends
on placement (a pointer compare across objects).
Split (--split*): the status of the verdict (0 sat or unsat, 3 unknown); a
part's status 1, 2, 4, 5 or 6 stops the split with that status; 5 also for
a failed --split-check.
)";

/// --version: the solvers linked in (their library versions at run time)
static void printSolverVersions(raw_ostream &os) {
  unsigned ma, mi, bu, re;
  Z3_get_version(&ma, &mi, &bu, &re);
  os << "  z3 " << ma << "." << mi << "." << bu << "." << re << "\n";
#ifdef WITH_YICES2
  os << "  yices " << yices_version << "\n";
#else
  os << "  yices: not built (--solver=yices disabled)\n";
#endif
#ifdef WITH_BITWUZLA
  os << "  bitwuzla " << bitwuzla::version() << "\n";
#else
  os << "  bitwuzla: not built (--solver=bitwuzla disabled)\n";
#endif
}

static int fail(const Twine &msg) {
  errs() << "sea-vc: " << msg << "\n";
  return 1;
}

static void printValue(raw_ostream &os, Value v) {
  if (Operation *op = v.getDefiningOp())
    os << op->getName() << " " << op->getLoc();
  else
    os << "block-arg " << v.getLoc();
}

/// MLIR's timing (--mlir-timing): the root scope of one report over every
/// PassManager; set in main when timing is on
static mlir::TimingScope *gTiming = nullptr;

/// MLIR's pass-manager options (--mlir-print-ir-*, --mlir-pass-statistics,
/// crash reproducers) and --mlir-timing, on a PassManager sea-vc built
static LogicalResult configurePM(PassManager &pm) {
  if (failed(mlir::applyPassManagerCLOptions(pm)))
    return failure();
  sea::addPassLog(pm);
  if (gTiming)
    pm.enableTiming(*gTiming);
  return success();
}

/// --reach-programs: R_T (sea-reach-target) of each target of `program`,
/// on a copy of `module`, to <dir>/reach-<k>.mlir; a `reach.program` record
/// per target, then `reach.programs`. The status of the run.
static int writeReachPrograms(ModuleOp module, sea::ProgramOp program,
                              ArrayRef<sea::ReachTarget> targets) {
  if (std::error_code ec = sys::fs::create_directories(reachPrograms))
    return fail("cannot create '" + reachPrograms + "': " + ec.message());
  for (auto [k, t] : llvm::enumerate(targets)) {
    OwningOpRef<ModuleOp> copy = module.clone();
    sea::ProgramOp p;
    for (auto q : copy->getOps<sea::ProgramOp>())
      if (q.getSymName() == program.getSymName())
        p = q;
    // the same list on the copy: the k-th target is T
    std::vector<sea::ReachTarget> ts = sea::collectReachTargets(p, reachKind);
    if (ts.size() != targets.size() || ts[k].id != t.id)
      return fail("--reach-programs: the targets of a copy of the program "
                  "differ (a bug)");
    if (failed(sea::buildReachTargetProgram(p, ts[k].op, t.id)))
      return 1;
    SmallString<128> path(reachPrograms);
    sys::path::append(path, "reach-" + Twine(k) + ".mlir");
    std::error_code ec;
    raw_fd_ostream os(path, ec, sys::fs::OF_Text);
    if (ec)
      return fail("cannot write '" + path + "': " + ec.message());
    // with the locations; R_T's own target has its id in sea.reach_target
    copy->print(os, OpPrintingFlags().enableDebugInfo(true, false));
    std::string at = sea::formatLoc(t.loc);
    sea::out::Record rec("reach.program");
    rec.str("id", t.id).str("op", t.opName).str("file", path.str());
    rec.text = "reach.program: " + t.id + " op=" + t.opName +
               " file=" + path.str().str() + " at=" + (at.empty() ? "-" : at);
    sea::addLocFields(rec, "at", t.loc);
    if (t.site >= 0) {
      rec.num("site", uint64_t(t.site));
      rec.text += " site=" + std::to_string(t.site);
    }
    if (!t.of.empty()) {
      rec.str("of", t.of);
      rec.text += " of=" + t.of;
    }
    gOut->emit(rec);
  }
  vout() << "reach.programs: " << targets.size() << " dir=" << reachPrograms
         << "\n";
  gOut->flush();
  return 0;
}

// --ite-stats: the census of one encoding (sea::IteCensus), by kind and
// source location
static void printIteStats(const sea::IteCensus &c, sea::ProgramOp program) {
  sea::ValueNames vn(program);
  const size_t cap = iteStatsMax ? size_t(iteStatsMax) : SIZE_MAX;
  auto locOf = [](Value v) {
    std::string l = sea::formatLoc(v.getLoc());
    // a join's block argument without a position: its block's first op's
    if (l.empty())
      if (auto a = dyn_cast<BlockArgument>(v))
        if (!a.getOwner()->empty())
          l = sea::formatLoc(a.getOwner()->front().getLoc());
    return l.empty() ? std::string("-") : l;
  };
  struct Site {
    std::string loc, name;
    unsigned values = 0, leaves = 0, conds = 0, guardAtoms = 0;
  };
  for (const char *kind : {"mem", "addr", "int"}) {
    std::vector<Site> sites;
    std::map<std::string, size_t> at;
    unsigned n = 0, overGuards = 0, leavesMax = 0;
    for (const sea::IteCensus::Value &v : c.values) {
      if (StringRef(v.kind) != kind)
        continue;
      ++n;
      overGuards += v.guardAtoms > 0;
      leavesMax = std::max(leavesMax, v.leaves);
      std::string l = locOf(v.value);
      auto [it, fresh] = at.try_emplace(l, sites.size());
      if (fresh) {
        Site st;
        st.loc = l;
        st.name = vn.name(v.value).str();
        sites.push_back(st);
      }
      Site &st = sites[it->second];
      ++st.values;
      st.leaves = std::max(st.leaves, v.leaves);
      st.conds = std::max(st.conds, v.conds);
      st.guardAtoms = std::max(st.guardAtoms, v.guardAtoms);
    }
    std::stable_sort(sites.begin(), sites.end(),
                     [](const Site &a, const Site &b) {
                       return a.leaves != b.leaves ? a.leaves > b.leaves
                                                   : a.values > b.values;
                     });
    const size_t listed = std::min(sites.size(), cap);
    {
      sea::out::Record rec("ite-values");
      rec.str("value_kind", kind)
          .num("values", n)
          .num("over_guards", overGuards)
          .num("sites", sites.size())
          .num("listed", listed)
          .num("dropped", sites.size() - listed)
          .num("leaves_max", leavesMax);
      rec.text = std::string("ite.values: kind=") + kind +
                 " values=" + std::to_string(n) +
                 " over-guards=" + std::to_string(overGuards) +
                 " sites=" + std::to_string(sites.size()) +
                 " listed=" + std::to_string(listed) +
                 " dropped=" + std::to_string(sites.size() - listed) +
                 " leaves-max=" + std::to_string(leavesMax);
      gOut->emit(rec);
    }
    for (size_t i = 0; i < listed; ++i) {
      const Site &st = sites[i];
      sea::out::Record rec("ite-site");
      rec.str("value_kind", kind)
          .num("values", st.values)
          .num("leaves", st.leaves)
          .num("conds", st.conds)
          .num("guard_atoms", st.guardAtoms)
          .str("name", st.name)
          .str("loc", st.loc);
      rec.text = std::string("ite.site: kind=") + kind +
                 " values=" + std::to_string(st.values) +
                 " leaves=" + std::to_string(st.leaves) +
                 " conds=" + std::to_string(st.conds) +
                 " guard-atoms=" + std::to_string(st.guardAtoms) +
                 " name=" + st.name + " loc=" + st.loc;
      gOut->emit(rec);
    }
  }
  // the loads at non-ground addresses, by kind and location
  std::map<std::string, uint64_t> byKind;
  std::vector<std::tuple<std::string, std::string, uint64_t>> sites;
  std::map<std::pair<std::string, std::string>, size_t> at;
  for (const sea::IteCensus::Load &l : c.loads) {
    ++byKind[l.kind];
    std::string loc = sea::formatLoc(l.op->getLoc());
    if (loc.empty())
      loc = "-";
    auto [it, fresh] = at.try_emplace({l.kind, loc}, sites.size());
    if (fresh)
      sites.emplace_back(l.kind, loc, 0);
    ++std::get<2>(sites[it->second]);
  }
  std::stable_sort(sites.begin(), sites.end(),
                   [](const auto &a, const auto &b) {
                     return std::get<2>(a) > std::get<2>(b);
                   });
  const size_t listed = std::min(sites.size(), cap);
  {
    sea::out::Record rec("ite-loads");
    rec.num("loads", c.allLoads)
        .num("dynamic", c.loads.size())
        .num("ite", byKind["ite"])
        .num("ite_index", byKind["ite-index"])
        .num("from_bits", byKind["from-bits"])
        .num("symbolic", byKind["symbolic"])
        .num("sites", sites.size())
        .num("listed", listed)
        .num("dropped", sites.size() - listed);
    rec.text = "ite.loads: loads=" + std::to_string(c.allLoads) +
               " dynamic=" + std::to_string(c.loads.size()) +
               " ite=" + std::to_string(byKind["ite"]) +
               " ite-index=" + std::to_string(byKind["ite-index"]) +
               " from-bits=" + std::to_string(byKind["from-bits"]) +
               " symbolic=" + std::to_string(byKind["symbolic"]) +
               " sites=" + std::to_string(sites.size()) +
               " listed=" + std::to_string(listed) +
               " dropped=" + std::to_string(sites.size() - listed);
    gOut->emit(rec);
  }
  for (size_t i = 0; i < listed; ++i) {
    const auto &[kind, loc, k] = sites[i];
    sea::out::Record rec("ite-load-site");
    rec.str("addr_kind", kind).num("loads", k).str("loc", loc);
    rec.text = "ite.load-site: kind=" + kind + " loads=" + std::to_string(k) +
               " loc=" + loc;
    gOut->emit(rec);
  }
}

int main(int argc, char **argv) {
  InitLLVM y(argc, argv);
  // SEA_LOG / SEA_LOG_FILE, before --log entries override them
  sea::log::init();
  mlir::registerPassManagerCLOptions();
  mlir::registerDefaultTimingManagerCLOptions();
  mlir::registerAsmPrinterCLOptions();
  // --mlir-disable-threading (and the diagnostic options): read by the
  // MLIRContext constructor
  mlir::registerMLIRContextCLOptions();
  // Repeated warnings are summarized at exit, whatever the exit path.
  struct Summary {
    ~Summary() { sea::diag::printSummary(); }
  } summary;
  // --config files: their options first, then the command line's
  BumpPtrAllocator configAlloc;
  StringSaver configSaver(configAlloc);
  SmallVector<const char *> args;
  std::vector<std::string> configsLoaded;
  if (!expandConfigs(argc, argv, configSaver, configAlloc, args, configsLoaded))
    return 1;
  sea::setVersionPrinter("sea-vc", printSolverVersions);
  cl::ParseCommandLineOptions(args.size(), args.data(),
                              "Verify a sea program with a VC\n");
  // z3's global parameters (SeaHorn's BmcEngine: Bmc.cc:38-43), before any
  // z3 solver exists
  if (z3Tactic != "default") {
    Z3_global_param_set("tactic.default_tactic", z3Tactic.c_str());
    if (z3Tactic == "sat" || StringRef(z3Tactic).ends_with("sat)"))
      Z3_global_param_set("sat.euf", "true");
  }
  for (const std::string &kv :
       llvm::concat<std::string>(yicesParams, yicesConfig)) {
    auto [k, v] = StringRef(kv).split('=');
    if (k.empty() || v.empty()) {
      errs() << "sea-vc: --yices-param / --yices-config wants key=value, "
                "got '"
             << kv << "'\n";
      return 1;
    }
  }
  for (const std::string &kv : z3Params) {
    auto [k, v] = StringRef(kv).split('=');
    if (k.empty() || v.empty()) {
      errs() << "sea-vc: --z3-param wants key=value, got '" << kv << "'\n";
      return 1;
    }
    Z3_global_param_set(k.str().c_str(), v.str().c_str());
  }
  if (agent) {
    outs() << agentGuide;
    return 0;
  }
  sea::out::Emitter emitter(outs(), outFormat);
  gOut = &emitter;
  sea::log::setFormat(outFormat);
  // --mlir-timing: reported on stderr when main returns
  mlir::DefaultTimingManager timing;
  mlir::applyDefaultTimingManagerCLOptions(timing);
  mlir::TimingScope timingRoot = timing.getRootScope();
  if (timing.isEnabled())
    gTiming = &timingRoot;
  // statistics: stderr under --smt2 (stdout is the query), unless jsonl
  // (the query is a record then)
  auto statOut = [&]() -> raw_ostream & {
    return smt2 && !emitter.jsonl() ? errs() : vout();
  };
  // the version first, so that every output (and bug report) carries it
  statOut() << "tool.version: " << sea::versionString() << "\n";
  for (const std::string &p : configsLoaded)
    statOut() << "vc.config: " << p << "\n";
#ifndef WITH_YICES2
  // Yices is optional at build time (SEA_ENABLE_YICES); release binaries
  // omit it (GPLv3, docs/Release.md)
  if (backend == Backend::Yices || !yicesParams.empty() ||
      !yicesConfig.empty() || !yicesLogic.empty())
    return fail("this sea-vc was built without Yices: --solver=yices and "
                "--yices-* are unavailable (configure with "
                "-DSEA_ENABLE_YICES=ON -DYICES2_ROOT=<install>)");
#endif
  if (vcgenOnly && smt2)
    return fail("--vcgen-only runs no solver; --smt2 prints the solver's "
                "query: use one");
  const bool reach = reachKind != sea::ReachKind::Off;
  if (!reach &&
      (reachRlimit || !reachWitness.empty() ||
       reachIncremental.getNumOccurrences() || !reachCorpus.empty() ||
       reachModelRuns.getNumOccurrences() || reachOrder.getNumOccurrences() ||
       reachFuzz || reachSeed.getNumOccurrences() || !reachPrograms.empty() ||
       reachQuery.getNumOccurrences() || reachKeepSites || reachFailures))
    return fail("--reach-rlimit, --reach-witness, --reach-incremental, "
                "--reach-model-runs, --reach-order, --reach-corpus, "
                "--reach-fuzz, --reach-seed, --reach-programs, "
                "--reach-query, --reach-keep-sites and --reach-failures "
                "need --reach");
  if (!reachQuery && reachCorpus.empty() && !reachFuzz)
    return fail("--reach-query=false decides by runs only: give "
                "--reach-corpus or --reach-fuzz");
  unsigned lowerDisableMask = 0;
  for (const std::string &p : lowerDisable) {
    unsigned bit = llvm::StringSwitch<unsigned>(p)
                       .Case("sccp", sea::LowerDisableSccp)
                       .Case("cse", sea::LowerDisableCse)
                       .Case("canonicalize", sea::LowerDisableCanonicalize)
                       .Default(0);
    if (p == "sea-drop-fresh-memhavoc")
      return fail("--lower-disable: sea-drop-fresh-memhavoc is a semantics "
                  "decision, not an optimization: it decides that a "
                  "memhavoc of a fresh object is no modification (tracking), "
                  "so the lowering without it means something else");
    if (!bit)
      return fail("--lower-disable: '" + p +
                  "' is not one of sccp, cse, canonicalize");
    lowerDisableMask |= bit;
  }
  if (!lowerDisable.empty() && !lower)
    return fail("--lower-disable needs --lower");
  if (rangeFoldOpt != sea::RangeFoldMode::Off && !lower)
    return fail("--range-fold needs --lower");
  if (expandMemMoveOpt && !lower)
    return fail("--expand-memmove needs --lower");
  if (boxAllExpandOpt.getNumOccurrences() && !lower)
    return fail("--box-all-expand needs --lower");
  if (boxAllExpandOpt > sea::kMaxBoxAllExpand)
    return fail("--box-all-expand=" + Twine(boxAllExpandOpt) +
                " is above the limit (" + Twine(sea::kMaxBoxAllExpand) + ")");
  if (expandMemMoveOpt > sea::kMaxExpandMemMoveBytes)
    return fail("--expand-memmove=" + Twine(expandMemMoveOpt) +
                " is above the limit (" + Twine(sea::kMaxExpandMemMoveBytes) +
                ")");
  if ((!placeIn.empty() || !placeOut.empty()) && !lower)
    return fail("--place-in and --place-out need --lower");
  if (!placeIn.empty() && !placeOut.empty())
    return fail("--place-in and --place-out exclude each other");
  if (printLowered == "-")
    return fail("--print-lowered needs a file (stdout is the report)");
  if (reachSeed.getNumOccurrences() && !reachFuzz)
    return fail("--reach-seed is the seed of --reach-fuzz");
  if (!reachPrograms.empty() &&
      (!reachCorpus.empty() || reachFuzz || !reachWitness.empty() ||
       reachRlimit || reachIncremental.getNumOccurrences() ||
       reachQuery.getNumOccurrences()))
    return fail("--reach-programs writes the R_T programs and exits: not "
                "with --reach-corpus, --reach-fuzz, --reach-witness, "
                "--reach-rlimit, --reach-incremental or --reach-query");
  // the corpus, read before any work
  std::vector<sea::CorpusEntryText> reachCorpusEntries;
  for (const std::string &c : reachCorpus) {
    std::string error;
    if (failed(sea::readCorpus(c, reachCorpusEntries, error)))
      return fail("--reach-corpus: " + error);
  }
  if (reach && (smt2 || vcgenOnly))
    return fail("--reach runs its own queries: not with --smt2 or "
                "--vcgen-only");
  if (reach && sinkAssumes)
    return fail("--reach: --sink-assumes moves every assume before the "
                "assertion, which changes what reaches a target");
  if (reach && (ptrCompanion == CompanionMode::Assume ||
                ptrCompanion == CompanionMode::Check))
    return fail("--reach: --ptr-companion=assume|check is not supported "
                "(use exact or off)");
  if (reach && reachRlimit && backend != Backend::Z3)
    return fail("--reach-rlimit is a z3 limit (--solver=z3)");
  if (memLimitPlant && !memLimit)
    return fail("--mem-limit-plant needs --mem-limit");
  if (memLimit) {
    memPhaseNote.set("mem-limit-phase: none");
    rlim_t bytes = rlim_t(memLimit) << 30;
    struct rlimit rl;
    if (getrlimit(RLIMIT_AS, &rl) == 0 &&
        (rl.rlim_max == RLIM_INFINITY || bytes <= rl.rlim_max)) {
      rl.rlim_cur = bytes;
      if (setrlimit(RLIMIT_AS, &rl) != 0)
        return fail("--mem-limit: setrlimit(RLIMIT_AS) failed");
    } else {
      return fail("--mem-limit: above the hard limit of RLIMIT_AS");
    }
    // z3's own accounting (MB), before any z3 context exists; an explicit
    // --z3-param=memory_max_size=<MB> wins
    if (llvm::none_of(z3Params, [](const std::string &kv) {
          return StringRef(kv).starts_with("memory_max_size=");
        }))
      Z3_global_param_set("memory_max_size",
                          std::to_string(uint64_t(memLimit) * 1024).c_str());
    std::set_new_handler(memLimitHit);
    llvm::install_bad_alloc_error_handler(memLimitHitLlvm);
  }

  MLIRContext context;
  // remarks (sea-widen-memory's report) are printed only when asked for; the
  // other diagnostics go to the default handler
  if (widenMemoryReport)
    context.getDiagEngine().registerHandler([](Diagnostic &d) {
      if (d.getSeverity() != DiagnosticSeverity::Remark)
        return failure();
      errs() << "remark: " << d.getLocation() << ": " << d.str() << "\n";
      return success();
    });
  context.getOrLoadDialect<sea::SeaDialect>();
  OwningOpRef<ModuleOp> module = sea::parseInputFile(inputFilename, &context);
  if (!module)
    return 1;

  sea::ProgramOp program;
  for (auto p : module->getOps<sea::ProgramOp>()) {
    if (!programName.empty() && p.getSymName() != programName)
      continue;
    if (program)
      return fail("several programs; pick one with --program");
    program = p;
  }
  if ((!seamInb || !seamRowExpand || !seamRowGroup || seamCheckInbPremise ||
       !seamLayout || !seamArm || !seamPruneSize0 || seamWide ||
       seamWideIteOpen || !seamLeafSplit || seamLeafSplitPartial ||
       seamPullIndex || seamOffsetLeaves.getNumOccurrences() ||
       seamResolveIteNodes || seamPullConcat || seamCongruence ||
       ptrCompanion != CompanionMode::Off || !seamStoreMap || !seamSimplify) &&
      !seam)
    return fail("--seam-inb, --seam-row-expand, --seam-row-group, "
                "--seam-check-inb-premise, --seam-layout, --seam-prune-size0, "
                "--seam-wide, --seam-wide-ite-open, --seam-leaf-split, "
                "--seam-leaf-split-partial, --seam-offset-leaves, "
                "--seam-arm, --seam-pull-index, --seam-resolve-ite-nodes, "
                "--seam-pull-concat, --seam-congruence, --ptr-companion, "
                "--seam-store-map and "
                "--seam-simplify need "
                "--seam");
  if (seamWords != WordsMode::Off && !seam)
    return fail("--seam-words needs --seam");
  if (wordsCheck && seamWords == WordsMode::Off)
    return fail("--words-check needs --seam-words");
  if (seamWords != WordsMode::Off && !xReadTable.empty())
    return fail("--seam-words and --x-read-table exclude each other");
  if (vcJoinCases && joinEncoding != sea::JoinEncoding::Ite)
    return fail("--vc-join-cases builds the ite joins: needs --vc-join=ite");
  if ((vcCasesReorder || vcCasesDropDefault) && !vcJoinCases)
    return fail("--vc-cases-reorder and --vc-cases-drop-default rearrange "
                "cases joins: need --vc-join-cases");
  if (seamStats && !seam && !simplifyOpt)
    return fail("--seam-stats needs --seam or --simplify");
  // the rewriter normalizes under --simplify, and under --seam unless
  // --seam-simplify=0
  // (--simplify with --seam would be ignored: refuse it instead)
  if (seam && simplifyOpt.getNumOccurrences() > 0)
    return fail("--simplify is the normalization without SeaM; --seam has it "
                "on by default (--seam-simplify=0 turns it off)");
  const bool simplify = seam ? seamSimplify : simplifyOpt;
  const bool lambdaMem = memEncoding == MemEncoding::Lambda;
  if (lambdaMem && (seam || simplifyOpt))
    return fail("--mem=lambda is an alternative to --seam / --simplify "
                "(docs/design/Lambda.md): not with them");
  if (lambdaMem && joinEncoding != sea::JoinEncoding::Ite)
    return fail("vc.lambda-join: --mem=lambda needs --vc-join=ite (an "
                "equality of memories is not specified for lambdas)");
  if (lambdaMem && backend != Backend::Z3)
    return fail("vc.lambda-unsupported: --mem=lambda needs --solver=z3 "
                "(the Yices and Bitwuzla marshalers refuse lambdas)");
  // case splitting (sea/VC/Split.h)
  const bool splitOn = !splitBool.empty() || !splitLeaves.empty() || splitAuto;
  if (splitCandidatesOpt) {
    if (splitOn)
      return fail("--split-candidates lists split targets without solving: "
                  "not with --split, --split-leaves or --split-auto");
    if (smt2 || reach || lambdaMem)
      return fail("--split-candidates: not with --smt2, --reach or "
                  "--mem=lambda");
    if (!seam)
      return fail("--split-candidates ranks SeaM's open alias questions: "
                  "needs --seam");
    if (splitMaxParts.getNumOccurrences() || splitCheck.getNumOccurrences() ||
        splitCheckRlimit.getNumOccurrences() ||
        splitStopOnSat.getNumOccurrences() || !splitSmt2.empty())
      return fail("--split-candidates splits nothing: --split-max-parts, "
                  "--split-check, --split-check-rlimit, --split-stop-on-sat "
                  "and --split-smt2 need --split, --split-leaves or "
                  "--split-auto");
    // they run after a solve, which --split-candidates never reaches
    if (seamCheck.getNumOccurrences() || eqAudit.getNumOccurrences() ||
        xReadTableCheck.getNumOccurrences())
      return fail("--split-candidates solves nothing: not with --seam-check, "
                  "--eq-audit or --x-read-table-check");
  } else if (splitCandidatesMax.getNumOccurrences()) {
    return fail("--split-candidates-max needs --split-candidates");
  }
  if (!splitOn && !splitCandidatesOpt &&
      (splitMaxParts.getNumOccurrences() || splitMaxPaths.getNumOccurrences() ||
       splitCheck.getNumOccurrences() || splitCheckRlimit.getNumOccurrences() ||
       splitStopOnSat.getNumOccurrences() || !splitSmt2.empty()))
    return fail("--split-max-parts, --split-max-paths, --split-check, "
                "--split-check-rlimit, --split-stop-on-sat and --split-smt2 "
                "need --split, --split-leaves or --split-auto "
                "(--split-max-paths: or --split-candidates)");
  if (iteStats && splitOn)
    return fail("--ite-stats reports one encoding: not with --split, "
                "--split-leaves or --split-auto (each part is an encoding)");
  if (!iteStats && iteStatsMax.getNumOccurrences())
    return fail("--ite-stats-max needs --ite-stats");
  if (splitOn && (smt2 || reach || lambdaMem))
    return fail("--split, --split-leaves and --split-auto: not with --smt2 "
                "(use --split-smt2), --reach or --mem=lambda");
  if (!splitPlant.empty() && splitPlant != "drop-case" &&
      splitPlant != "wrong-value")
    return fail("--x-split-plant: drop-case or wrong-value");
  if (splitAuto && !seam)
    return fail("--split-auto ranks SeaM's open alias questions: needs --seam");
  if (splitOn && !splitMaxParts)
    return fail("--split-max-parts must be at least 1");
  // the per-part counters (alias.open.*, row.*) are part of a split's report
  if ((splitOn || splitCandidatesOpt) && seam)
    seamStats = true;
  const bool checkOn = seamCheck.getNumOccurrences() > 0;
  const bool auditOn = eqAudit.getNumOccurrences() > 0;
  unsigned auditSample = 200;
  if (auditOn && !eqAudit.empty() &&
      StringRef(eqAudit).getAsInteger(10, auditSample))
    return fail("--eq-audit=N needs a number");
  if (auditOn && !seam)
    return fail("--eq-audit needs --seam");
  const bool trailOn =
      !seamTrail.empty() || !seamTrace.empty() || checkOn || auditOn;
  if (trailOn && !seam && !simplifyOpt)
    return fail("--seam-trail, --seam-trace and --seam-check need --seam or "
                "--simplify");
  if (seamTrailText && seamTrail.empty())
    return fail("--seam-trail-text needs --seam-trail");
  unsigned checkSample = 0; // 0: every firing
  if (checkOn && !seamCheck.empty() &&
      StringRef(seamCheck).getAsInteger(10, checkSample))
    return fail("--seam-check=N needs a number");
  expr::SeaMOptions::PlantBug plantBug = expr::SeaMOptions::PlantBug::None;
  if (seamPlantBug == "sub.const")
    plantBug = expr::SeaMOptions::PlantBug::SubConst;
  else if (seamPlantBug == "row.store-hit")
    plantBug = expr::SeaMOptions::PlantBug::RowStoreHit;
  else if (!seamPlantBug.empty())
    return fail("--seam-plant-bug: sub.const or row.store-hit");
  // the trail outlives the runs (a run without A-INB may follow the first)
  expr::RewriteTrail trail;
  std::unique_ptr<raw_fd_ostream> trailOs, termsOs;
  if (!seamTrail.empty()) {
    std::error_code ec;
    trailOs = std::make_unique<raw_fd_ostream>(seamTrail, ec);
    if (!ec)
      termsOs = std::make_unique<raw_fd_ostream>(seamTrail + ".terms", ec);
    if (ec)
      return fail("cannot write " + seamTrail + ": " + ec.message());
    trail.setOutput(trailOs.get(), termsOs.get());
    trail.setPrintText(seamTrailText);
  }
  if (!seamTrace.empty())
    trail.setTrace(std::vector<std::string>(seamTrace.begin(), seamTrace.end()),
                   &errs());
  trail.setKeep(checkOn || auditOn);
  // the report of --seam-check (printed after the verdict) and its verdict
  std::string checkReport;
  std::string auditReport;
  uint64_t checkInvalid = 0;
  if (!program)
    return fail(programName.empty() ? "no sea.program in input"
                                    : "no program named '" + programName + "'");
  bool abstract = sea::isAbstract(program);
  // z3 and Bitwuzla have mul overflow predicates (Bitwuzla's bvumulo /
  // bvsmulo, checked against z3's by check-semantics.py --bitwuzla); Yices
  // gets the expansion (as do add / sub overflow for every solver: solvers
  // expand those eagerly anyway).
  bool nativeMulOverflow = backend != Backend::Yices;
  // With --seam the answer rests on A-INB. A sat whose replay fails may be
  // an artifact of a program that leaves its objects; the abstract program
  // (every access checked against its object) tells, so keep a copy.
  // --reach: every check of the input gets its reach site, before any
  // pass and any copy: the targets of the lowered program are found in the
  // A-INB copy below by it (not by their location: inlined code repeats
  // one), and two lowerings of the input match their targets by it
  // (`site=`, utils/cover-diff.py)
  if (reach && failed(sea::tagReachSites(*module)))
    return 1;
  // --reach: every block gets its stable id too (kBlockIdsAttr; the passes
  // keep them): R_T keeps them, and its --smt2 query names the ids of each
  // block guard, from which utils/query-diff.py builds Skolem hints
  if (reach)
    sea::tagBlockIds(*module);
  // --reach-keep-sites: read by the canonicalizer (kKeepReachSitesAttr)
  for (auto p : module->getOps<sea::ProgramOp>()) {
    if (reachKeepSites)
      p->setAttr(sea::kKeepReachSitesAttr, UnitAttr::get(&context));
    if (reachFailures)
      p->setAttr(sea::kReachFailuresAttr, UnitAttr::get(&context));
  }
  OwningOpRef<ModuleOp> abstractModule;
  if (seam && seamInb && abstract && lower)
    abstractModule = module->clone();
  // The pointer mode (PtrSort S0), classified on
  // the input program, before the lowering: sea-split-wide-access makes
  // addr.from_int of its own for reassembled pointer loads (to become
  // addr.from_bits).
  unsigned intToPtr = 0;
  Operation *firstIntToPtr = nullptr;
  program.walk([&](Operation *op) {
    if (isa<sea::PtrFromIntOp, sea::AddrFromIntOp>(op)) {
      ++intToPtr;
      if (!firstIntToPtr)
        firstIntToPtr = op;
    }
  });
  if (ptrMode == PtrMode::Opaque && intToPtr) {
    sea::diag::error("vc.int-to-ptr",
                     "--ptr-mode=opaque: the program converts an integer to a "
                     "pointer (opaque pointers have no int-to-ptr; use "
                     "--ptr-mode=address)",
                     firstIntToPtr->getLoc());
    return 1;
  }
  const bool addressMode =
      ptrMode == PtrMode::Address || (ptrMode == PtrMode::Auto && intToPtr);
  if (!addr0IsNull)
    sea::diag::warn("vc.addr0-is-null-off",
                    "--addr0-is-null=false changes the replays only: the VC "
                    "still identifies null with address 0 (placement puts "
                    "the null object there; assume.addr0-is-null: vc)");
  if (rangeFoldOpt == sea::RangeFoldMode::Check &&
      (checkAlign || checkPtrIndex)) {
    sea::diag::error("vc.check-jobs",
                     "--range-fold=check is a job of its own (each check "
                     "job drops the program's asserts): run one at a time");
    return 1;
  }
  if (checkAlign && checkPtrIndex) {
    sea::diag::error("vc.check-jobs",
                     "--check-align and --check-ptr-index are separate jobs "
                     "(each drops the program's asserts): run one at a time");
    return 1;
  }
  // --replay-original: the input before any pass of sea-vc
  // (each assert tagged with its id first, in both copies: tagAsserts)
  OwningOpRef<ModuleOp> originalModule;
  if (replayOriginalOpt) {
    for (auto p : module->getOps<sea::ProgramOp>())
      sea::tagAsserts(p);
    originalModule = module->clone();
  }
  if (printRefs != sea::PrintRefs::AsIs) {
    PassManager pm(&context);
    sea::addPrintRefsPass(pm, printRefs);
    if (failed(configurePM(pm)) || failed(pm.run(*module)))
      return 1;
  }
  if (lower) {
    Phase lowerPhase("lower");
    const uint64_t opsBefore =
        logPhase.on(sea::log::Flow) ? countOps(*module) : 0;
    PassManager pm(&context);
    // with --seam, memset / memmove / memhavoc are terms (SeaM-memops.md):
    // no sea-expand-memset, symbolic lengths are exact
    sea::buildVCLoweringPipeline(
        pm, abstract, nativeMulOverflow,
        /*fold=*/true, widenMemory,
        /*expandMemSet=*/!seam && !lambdaMem, widenMemoryReport,
        /*keepByteWide=*/seam && seamWide, checkAlign,
        checkPtrIndex ? (checkPtrIndexAssumeAsserts ? 2u : 1u) : 0u, lowerOpt,
        memhavocNull == MemhavocNull::Noop, lowerDisableMask,
        sea::PlaceFiles{placeIn, placeOut}, rangeFoldOpt, expandMemMoveOpt,
        ptrCompanion != CompanionMode::Off, boxAllExpandOpt);
    if (failed(configurePM(pm)) || failed(pm.run(*module)))
      return 1;
    SEA_PHASE_END(lowerPhase)
        .kv("ops-before", opsBefore)
        .kv("ops-after", countOps(*module));
  } else if (abstract) {
    sea::diag::error("vc.abstract-program",
                     "the program uses abstract pointers; place it first "
                     "(sea-opt --sea-place-static, or sea-vc --lower)",
                     program.getLoc());
    return 1;
  }
  if (failed(sea::verifyBmcForm(program)))
    return 1;
  // block ids (--reach, or an R_T that carries them): a reachable block
  // without them is a pass that lost them; its guard gets no Skolem hint
  {
    Operation *first = nullptr;
    if (unsigned n = sea::countBlocksWithoutIds(program, &first))
      sea::diag::warn("vc.block-ids-missing",
                      Twine(n) + " reachable block(s) without sea.block_ids "
                                 "(a pass lost them): their guards get no "
                                 "Skolem hint",
                      first->getLoc());
  }
  // folding (canonicalize) may leave no reachable assertion: reported; the
  // solver still answers (unsat)
  const bool staticallySafe = sea::isStaticallySafe(program);
  // the folds that decide a block's rest (sea canonicalizers; this run,
  // every program of the file)
  const unsigned foldAssumeFalse = sea::diag::count("fold.assume-false");
  const unsigned foldAssertFalse = sea::diag::count("fold.assert-false");
  auto foldLines = [&](raw_ostream &os) {
    if (foldAssumeFalse || foldAssertFalse)
      os << "vc.fold: assume-false=" << foldAssumeFalse
         << " assert-false=" << foldAssertFalse << "\n";
    if (staticallySafe)
      os << "vc.statically-safe: no reachable sea.halt (every path is "
            "blocked; the assertion cannot fail)\n";
  };
  // the bounded expansions of symbolic memset / memmove lengths
  // (sea-expand-memset, without --seam): assume(n <= K), an
  // under-approximation the result rests on (assume.memset-bound)
  // the uses of the null object's address 0 placement made
  uint64_t nullUses = 0;
  program.walk([&](sea::SegmentOp seg) {
    if (seg.getKind() == "null")
      nullUses += std::distance(seg->use_begin(), seg->use_end());
  });
  // the memhavocs sea-guard-null-memhavoc guarded (memhavoc of null is a
  // no-op in the VC too)
  uint64_t nullGuards = 0;
  if (auto n =
          program->getAttrOfType<IntegerAttr>(sea::kMemhavocNullGuardsAttr))
    nullGuards = n.getInt();
  // the memhavocs sea-drop-fresh-memhavoc dropped under A-INB
  // the counters of the lowering passes: sea-range-fold (--range-fold)
  std::string lowerStatLines;
  if (auto d =
          program->getAttrOfType<DictionaryAttr>(sea::kRangeFoldStatsAttr)) {
    raw_string_ostream os(lowerStatLines);
    os << "vc.range-fold: mode="
       << (rangeFoldOpt == sea::RangeFoldMode::Check ? "check" : "on");
    for (NamedAttribute a : d)
      if (auto i = dyn_cast<IntegerAttr>(a.getValue()))
        os << " " << a.getName().getValue() << "=" << i.getInt();
    os << "\n";
  }
  // sea-expand-small-memmove's counters (--expand-memmove)
  if (auto d = program->getAttrOfType<DictionaryAttr>(
          sea::kExpandMemMoveStatsAttr)) {
    raw_string_ostream os(lowerStatLines);
    os << "vc.expand-memmove:";
    for (StringRef key :
         {"k", "expanded", "bytes", "kept-symbolic", "kept-over-k",
          "kept-word-layout", "kept-abstract", "kept-tracked",
          "kept-companion-mirror", "kept-ptr-companion"})
      if (auto i = d.getAs<IntegerAttr>(key))
        os << " " << key << "=" << i.getInt();
    os << "\n";
  }
  // sea-lower-box-all's counters (a program with sea.box.all ops)
  if (auto d = program->getAttrOfType<DictionaryAttr>(sea::kBoxAllStatsAttr)) {
    raw_string_ostream os(lowerStatLines);
    os << "vc.box-all:";
    for (NamedAttribute a : d)
      if (auto i = dyn_cast<IntegerAttr>(a.getValue()))
        os << " " << a.getName().getValue() << "=" << i.getInt();
    os << "\n";
  }
  uint64_t dropFreshInb = 0;
  if (auto n = program->getAttrOfType<IntegerAttr>(sea::kDropFreshInbAttr))
    dropFreshInb = n.getInt();
  uint64_t memsetBounds = 0, memsetBoundK = 0;
  program.walk([&](sea::AssumeOp a) {
    if (auto k = a->getAttrOfType<IntegerAttr>(sea::kMemSetBoundAttr)) {
      ++memsetBounds;
      memsetBoundK = std::max<uint64_t>(memsetBoundK, k.getInt());
    }
  });
  // --sink-assumes: after the census of the assumes above (the pass erases
  // them; the result rests on them all the same)
  unsigned sunkAssumes = 0;
  if (sinkAssumes) {
    // the chosen program only (a file may hold several)
    PassManager pm(&context, sea::ProgramOp::getOperationName());
    pm.addPass(sea::createSinkAssumesPass(&sunkAssumes));
    if (failed(configurePM(pm)) || failed(pm.run(program)))
      return 1;
  }

  // --reach: the targets of the program the VC encodes (after the lowering),
  // then a block right before each (sea-split-at-checks): its guard is the
  // target's reachability
  std::vector<sea::ReachTarget> reachTargets;
  unsigned reachSplits = 0;
  if (reach) {
    // --reach-failures: the failure of each tagged assert, a target of
    // its own (a block with its reach-site marker; the program means the
    // same)
    if (reachFailures) {
      PassManager pm(&context, sea::ProgramOp::getOperationName());
      pm.addPass(sea::createSplitAssertFailuresPass());
      if (failed(configurePM(pm)) || failed(pm.run(program)))
        return 1;
    }
    reachTargets = sea::collectReachTargets(program, reachKind);
    if (!reachPrograms.empty())
      return writeReachPrograms(*module, program, reachTargets);
    PassManager pm(&context, sea::ProgramOp::getOperationName());
    pm.addPass(sea::createSplitAtChecksPass(reachKind == sea::ReachKind::Blocks,
                                            &reachSplits));
    if (failed(configurePM(pm)) || failed(pm.run(program)))
      return 1;
  }
  // --reach-corpus / --reach-fuzz: concrete runs on the program the queries
  // are about, before any query (lib/VC/Reach.cpp)
  std::optional<sea::ReachRuns> reachRuns;
  if (reach && (!reachCorpusEntries.empty() || reachFuzz)) {
    sea::ReachRunOptions rro;
    rro.corpus = std::move(reachCorpusEntries);
    rro.fuzzTrials = reachFuzz;
    rro.seed = reachSeed;
    sea::ReachOptions ropts;
    ropts.witnessDir = reachWitness;
    ropts.programPath = inputFilename;
    ropts.lowered = lower;
    ropts.addr0IsNull = addr0IsNull;
    ropts.memhavocNullNoop = memhavocNull == MemhavocNull::Noop;
    if (seam && seamInb && abstractModule)
      for (auto p : abstractModule->getOps<sea::ProgramOp>())
        if (p.getSymName() == program.getSymName())
          ropts.abstractProgram = p;
    Phase runsPhase("reach-runs");
    std::string error;
    FailureOr<sea::ReachRuns> rr =
        sea::runReachRuns(program, reachTargets, rro, ropts, error);
    if (failed(rr)) {
      sea::diag::error("vc.reach-runs", error);
      return 1;
    }
    SEA_PHASE_END(runsPhase)
        .kv("decided", rr->decided)
        .kv("trials", rr->trials);
    reachRuns = std::move(*rr);
  }

  // --print-lowered: the program the VC encodes, named as sea::ValueNames
  // names it (AsmState of the module, NameLocs as name prefixes)
  if (!printLowered.empty()) {
    std::error_code ec;
    raw_fd_ostream os(printLowered, ec, sys::fs::OF_Text);
    if (ec)
      return fail("cannot write '" + printLowered + "': " + ec.message());
    os << "// sea-vc --print-lowered: the program the VC encodes, named as "
          "the VC names it\n// (split selectors, split.* lines); a nondet "
          "result is nd.<site> in the VC.\n";
    // locations inline (no #loc aliases), so a grep for a name shows its
    // position
    module->print(os, OpPrintingFlags()
                          .enableDebugInfo(true, false)
                          .useLocalScope()
                          .printNameLocAsPrefix());
    os << "\n";
  }
  // case splitting (sea/VC/Split.h): the targets, on the program the VC
  // encodes; the choices of the run's first levels; what a run reports
  std::vector<sea::SplitTarget> splitTargets;
  if (splitOn) {
    std::string error;
    if (failed(sea::resolveSplitTargets(program, splitBool,
                                        sea::SplitTarget::Kind::Bool,
                                        splitTargets, error)) ||
        failed(sea::resolveSplitTargets(program, splitLeaves,
                                        sea::SplitTarget::Kind::Leaves,
                                        splitTargets, error))) {
      sea::diag::error("vc.split-target", error);
      return 1;
    }
  }
  std::vector<unsigned> splitPrefix;
  unsigned splitPartId = 0;
  struct SplitPartInfo {
    // per level: case count, chosen case
    std::vector<std::pair<unsigned, unsigned>> levels;
    std::string choice = "-", result = "error";
    double vcSeconds = 0, solveSeconds = 0;
    uint64_t dag = 0, aliasOpen = 0, aliasOpenPairs = 0, memopOpen = 0,
             rows = 0, ackReads = 0, ackPairs = 0, ackConstraints = 0;
    bool ackOn = false;
    std::string check = "off";
    // levels whose (E) or (X) check was unknown or unexpressible
    unsigned checkUnknown = 0;
    // Leaves levels whose chosen value was checked structurally
    unsigned leafStructural = 0;
  } splitPart;
  // --split-auto: the pick run (no solver) and its ranking
  bool splitPickRun = false;
  struct SplitCandidate {
    Value value;
    std::string name, loc;
    unsigned leaves = 0;
    uint64_t open = 0;
    // --split-candidates: questions attributed to no other candidate;
    // leaves without an object
    uint64_t own = 0, direct = 0;
    unsigned unknownLeaves = 0;
    // --split-candidates: array reads whose index holds its term
    uint64_t reads = 0;
  };
  std::vector<SplitCandidate> splitCandidates;
  // --split-candidates: the Boolean candidates behind an open question,
  // ranked; how many were recorded; the pick run's time
  struct SplitBoolCandidate {
    std::string name, loc;
    bool guard = false;
    uint64_t open = 0;
    unsigned addrs = 0;
    uint64_t reads = 0, condReads = 0;
    unsigned covers = 0;
    // the later candidates with the same term
    std::vector<std::string> same;
  };
  std::vector<SplitBoolCandidate> splitBoolCandidates;
  size_t splitBoolRecorded = 0;
  uint64_t splitOpenAliases = 0;
  // --split-candidates: the array reads of the query at the solver
  // boundary; the open questions the attribution does not take (the
  // same-object offset compares and the memory-operation in-tests that
  // seam.alias.open.* / seam.alias.open-memop.* count)
  uint64_t splitReads = 0, splitSameObjectOpen = 0, splitMemopOpen = 0;
  double splitPickSeconds = 0;
  // --split-candidates: the integer and Boolean candidates by the
  // arithmetic nodes they control (SplitCensus::attributeArith), ranked;
  // the arithmetic nodes of the query; the integer ite values recorded
  struct SplitArithCandidate {
    std::string name, loc, target;
    uint64_t arith = 0, mul = 0, div = 0, direct = 0;
    unsigned cases = 0, feeds = 0;
    std::vector<std::string> same;
  };
  std::vector<SplitArithCandidate> splitArithCandidates;
  // --ite-stats: printed for the first encoding only (a retry encodes again)
  bool iteStatsDone = false;
  uint64_t splitArithNodes = 0;
  size_t splitIntRecorded = 0;
  // One run of the VC and the solver. Returns kRetryWithoutInb if A-INB
  // (assumed by the rewriting) is found not to hold.
  constexpr int kRetryWithoutInb = -1;
  // a sat model that breaks a GEP promise the stride rule relied on: rerun
  // without the rule (PtrIndex.md section 5)
  constexpr int kRetryWithoutStride = -2;
  // a sat answer under A-PTRINT: rerun with the exact companion (the
  // counterexample may rest on the premise)
  constexpr int kRetryExactCompanion = -3;
  // --replay-original: the confirmed counterexample of the lowered program,
  // run on the input program as it was before any pass (sea::replayOriginal).
  // It must fail the same assertion (its own location, stopLocOf): else the
  // lowering changed what the program does on these inputs (exit 6). A run
  // that went past a memory-safety violation (deferred) and fails elsewhere
  // has an error placement defines and the abstract program does not: exit 4.
  auto replayOnOriginal = [&](sea::solver::Model &replayModel,
                              sea::SeaOpSem &sem, sea::OpSemContext &ctx,
                              const sea::RunResult &lowRun,
                              bool ptrIntCheck) -> int {
    const char *skip = nullptr;
    if (!lower && !sinkAssumes)
      skip = "no lowering: the same program";
    else if (checkAlign || checkPtrIndex)
      skip = "a check job: its assertions are not the program's";
    else if (ptrIntCheck)
      skip = "--ptr-companion=check: the property is A-PTRINT";
    if (skip) {
      sea::out::Record rec("replay-original");
      rec.str("status", "skipped").str("reason", skip);
      rec.text = std::string("replay.original: skipped (") + skip + ")";
      gOut->emit(rec);
      gOut->flush();
      return 0;
    }
    sea::ProgramOp orig;
    for (auto p : originalModule->getOps<sea::ProgramOp>())
      if (p.getSymName() == program.getSymName())
        orig = p;
    if (!orig) {
      sea::diag::error("vc.replay-original-impossible",
                       "no program named '" + program.getSymName().str() +
                           "' in the input");
      return 1;
    }
    Phase phase("replay-original");
    sea::OriginalReplay o =
        sea::replayOriginal(orig, program, replayModel, sem, ctx, addr0IsNull,
                            memhavocNull == MemhavocNull::Noop);
    std::optional<Location> origLoc =
        o.run.status == sea::RunStatus::MemoryError &&
                o.run.reachedStatus == sea::RunStatus::AssertFailed
            ? sea::reachedLocOf(o.run)
        : o.run.status == sea::RunStatus::AssertFailed ? sea::stopLocOf(o.run)
                                                       : std::nullopt;
    std::optional<Location> lowLoc = sea::stopLocOf(lowRun);
    auto locText = [](std::optional<Location> l) {
      std::string t = l ? sea::formatLoc(*l) : "";
      return t.empty() ? std::string("-") : t;
    };
    auto posText = [](std::optional<Location> l) {
      std::optional<FileLineColLoc> f = l ? sea::fileLocOf(*l) : std::nullopt;
      return f ? sea::fileLocText(*f) : std::string("-");
    };
    // The same assert: by the id tagAsserts gave it (before the clone).
    // Without ids on both sides (a pass rebuilt the branch that carried
    // it), by
    // its location text only when no other assert of the original shares
    // it and it is not empty: else the assert cannot be told apart.
    std::optional<uint64_t> origId =
        origLoc ? sea::assertIdOf(o.run) : std::nullopt;
    std::optional<uint64_t> lowId =
        lowLoc ? sea::assertIdOf(lowRun) : std::nullopt;
    bool match = false, ambiguous = false;
    if (origId && lowId) {
      match = *origId == *lowId;
    } else if (origLoc && lowLoc) {
      std::string text = locText(lowLoc);
      unsigned same = 0;
      orig.walk([&](sea::AssertOp a) { same += locText(a.getLoc()) == text; });
      ambiguous = text == "-" || same != 1;
      match = !ambiguous && locText(origLoc) == text;
    }
    // Not a verdict on the lowering: the interpreter cannot run the original
    // program on these inputs (ptr.to_int before placement, a pointer load
    // of nondet cells, a limit), or the run read an earlier memory version
    // (the in-place interpreter is right only if the program's Mem-SSA
    // no-clobber claim holds; the lowered program's replay validates it,
    // this one has no model of the original to compare with).
    const char *undetermined = nullptr;
    if (ambiguous)
      undetermined = "the failing assert has no id and its location is empty "
                     "or shared with another assert";
    else if (!match && o.unmapped)
      undetermined = "inputs the lowered program has no site or object for "
                     "read as 0 (unmapped)";
    else if (!match && o.run.status == sea::RunStatus::Error &&
             !sea::isMemorySafetyKind(o.run.errorKind))
      undetermined = "the original program cannot be run on these inputs";
    else if (!match && o.run.status == sea::RunStatus::StepLimit)
      undetermined = "the original program's run reached the step limit "
                     "(e.g. a sea.box.all with a large box: the "
                     "interpreter runs every index)";
    else if (!match && !o.run.earlierVersionLoads.empty())
      undetermined = "the original program loads an earlier memory version "
                     "(a Mem-SSA no-clobber claim the in-place interpreter "
                     "cannot check)";
    const char *matchText = match          ? "yes"
                            : undetermined ? "undetermined"
                                           : "no";
    SEA_PHASE_END(phase)
        .kv("status", sea::stringifyRunStatus(o.run.status))
        .kv("match", matchText);
    {
      sea::out::Record rec("replay-original");
      rec.str("status", sea::stringifyRunStatus(o.run.status))
          .str("at", posText(origLoc))
          .str("match", matchText)
          .num("nondets", o.nondets)
          .num("cells", o.cells)
          .num("defaulted", o.defaulted)
          .num("unmapped", o.unmapped);
      raw_string_ostream os(rec.text);
      os << "replay.original: status=" << sea::stringifyRunStatus(o.run.status)
         << " at=" << posText(origLoc) << " match=" << matchText
         << " nondets=" << o.nondets << " cells=" << o.cells
         << " defaulted=" << o.defaulted << " unmapped=" << o.unmapped;
      // the first false index of a sea.box.all of the original (its
      // claim over the whole box; docs/design/BoxAll.md)
      if (o.run.boxAllFalse) {
        std::string idx = sea::boxAllFalseText(o.run);
        os << " box-all-false=" << idx;
        rec.str("box_all_false", idx);
      }
      if (!o.run.earlierVersionLoads.empty()) {
        os << " earlier-version-loads=" << o.run.earlierVersionLoads.size();
        rec.num("earlier_version_loads", o.run.earlierVersionLoads.size());
      }
      if (!match) {
        auto withId = [&](std::optional<Location> l,
                          std::optional<uint64_t> id) {
          return locText(l) + (id ? " assert=" + std::to_string(*id) : "");
        };
        os << "\nreplay.original: expected: " << withId(lowLoc, lowId)
           << "\nreplay.original: got: " << withId(origLoc, origId);
        rec.str("expected", withId(lowLoc, lowId))
            .str("got", withId(origLoc, origId));
        if (!o.run.message.empty()) {
          os << "\nreplay.original: message: " << o.run.message;
          rec.str("message", o.run.message);
        }
      }
      gOut->emit(rec);
    }
    if (o.run.addr0IsNullUses) {
      gOut->emit(sea::out::parseLine("assume.addr0-is-null: uses=" +
                                     std::to_string(o.run.addr0IsNullUses)));
      brunchStat(vout(), "assume.addr0-is-null-uses",
                 Twine(o.run.addr0IsNullUses));
    }
    if (o.run.memhavocNullNoopUses) {
      gOut->emit(
          sea::out::parseLine("assume.memhavoc-null-noop: uses=" +
                              std::to_string(o.run.memhavocNullNoopUses)));
      brunchStat(vout(), "assume.memhavoc-null-noop-uses",
                 Twine(o.run.memhavocNullNoopUses));
    }
    gOut->flush();
    if (match)
      return 0;
    if (undetermined) {
      sea::diag::warn("vc.replay-original-undetermined",
                      Twine("sat, but the replay on the original program "
                            "decides nothing: ") +
                          undetermined +
                          (o.run.message.empty() ? "" : ": " + o.run.message));
      return 0;
    }
    if (o.run.deferredCount) {
      sea::diag::error(
          "vc.replay-original-memory-error",
          "sat, but the original program makes a memory-safety violation on "
          "the model's inputs (" +
              o.run.deferredMessage +
              ") and then does not fail the same assertion: placement "
              "defines what the program does not; the answer is "
              "inconclusive and the program has a memory-safety error",
          o.run.deferredAt ? std::optional<Location>(o.run.deferredAt->getLoc())
                           : std::nullopt);
      return 4;
    }
    sea::diag::error(
        "vc.replay-original-mismatch",
        "sat, and the lowered program's replay confirms it, but the original "
        "program on the same inputs ends " +
            sea::stringifyRunStatus(o.run.status) + " at " + locText(origLoc) +
            ", not at the failing assertion " + locText(lowLoc) +
            ": the lowering changed what the program does (a bug in a pass, "
            "or a program whose behavior depends on where placement puts "
            "its objects, such as a pointer compare across objects)");
    return 6;
  };
  CompanionMode companionMode = ptrCompanion;
  bool strideOn = ackStride;
  unsigned vcRuns = 0; // vc.phase run=
  auto runVc = [&](bool inb, bool mayRetry, const char *note) -> int {
    ++vcRuns;
    auto vcStart = std::chrono::steady_clock::now();
    // the phases of vc-time (--seam-stats: vc.<phase>-time)
    using Clock = std::chrono::steady_clock;
    auto lapStart = vcStart;
    std::vector<std::pair<const char *, double>> phases;
    auto lap = [&](const char *name) {
      auto now = Clock::now();
      phases.emplace_back(
          name, std::chrono::duration<double>(now - lapStart).count());
      lapStart = now;
    };
    expr::ExprFactory efac;
    sea::ValueNames names(program);
    expr::SeaMOptions seamOpts;
    seamOpts.assumeInBounds = inb;
    seamOpts.expandUnknownRow = seamRowExpand;
    seamOpts.groupSameValueRow = seamRowGroup;
    seamOpts.checkInbPremise = seamCheckInbPremise;
    seamOpts.layoutDecide = seamLayout;
    seamOpts.pruneSize0 = seamPruneSize0;
    seamOpts.wideAccess = seamWide;
    seamOpts.strideRule = seamStride;
    seamOpts.arm = seamArm;
    seamOpts.pullIteIndex = seamPullIndex;
    seamOpts.resolveIteNodes = seamResolveIteNodes;
    seamOpts.pullConcatIte = seamPullConcat;
    seamOpts.congruence = seamCongruence;
    seamOpts.wideIteOpenLeaves = seamWideIteOpen;
    seamOpts.leafSplit = seamLeafSplit;
    seamOpts.leafSplitPartial = seamLeafSplitPartial;
    seamOpts.maxOffsetLeaves = seamOffsetLeaves;
    seamOpts.storeMap = seamStoreMap;
    seamOpts.simplify = simplify;
    seamOpts.plantBug = plantBug;
    seamOpts.maxRuleCalls = seamRuleLimit;
    // the sea op of each arithmetic node and select (--arith-stats,
    // --ack-dump-pairs; sea/VC/Origins.h), followed through the stages
    // below; declared before sem, which holds it
    sea::ExprOrigins origins;
    const bool originsOn = arithStats || !ackDumpPairs.empty();
    sea::SeaOpSem sem(efac, names, nativeMulOverflow, seam, &seamOpts);
    if (originsOn)
      sem.setOrigins(&origins);
    sem.setAddressMode(addressMode);
    sem.setPtrCompanion(companionMode != CompanionMode::Off,
                        companionMode == CompanionMode::Assume,
                        companionMode == CompanionMode::Check);
    sem.setLambdaMemory(lambdaMem);
    // --mem=lambda: SeaHorn's per-write simplification (docs/design/Lambda.md):
    // one z3 context and simplifier cache for the VC, `:rewriter.flat false`;
    // every value written to a register, memories coerced back to functions
    struct LambdaStats {
      uint64_t writes = 0, memory = 0, scalar = 0, skipped = 0;
      double seconds = 0, finalSeconds = 0;
    } lstats;
    std::unique_ptr<sea::EZ3> lz3;
    std::unique_ptr<sea::ZSimplifier<sea::EZ3>> lsimp;
    if (lambdaMem && lambdaSimplify == LambdaSimplify::Z3) {
      lz3 = std::make_unique<sea::EZ3>(efac);
      lsimp = std::make_unique<sea::ZSimplifier<sea::EZ3>>(*lz3);
      lsimp->params().set("ctrl_c", true);
      lsimp->params().set(":rewriter.flat", false);
    }
    // the trail of this run; its terms are dropped before efac
    struct TrailRun {
      expr::RewriteTrail *t;
      sea::SeaOpSem &sem;
      ~TrailRun() {
        if (!t)
          return;
        const expr::RewriteStats *st = sem.seamStats();
        t->endRun(st ? st->ruleCalls : 0, st ? st->cacheHits : 0);
      }
    } trailRun{trailOn ? &trail : nullptr, sem};
    if (trailOn) {
      trail.beginRun(inb);
      sem.setTrail(&trail);
    }
    sea::SymStore store(efac);
    expr::ExprVector side;
    sea::OpSemContext ctx(store, side);
    if (lambdaMem)
      ctx.writeHook = [&](expr::Expr reg, expr::Expr val) -> expr::Expr {
        ++lstats.writes;
        const bool mem = sea::SeaOpSem::isMemoryReg(reg);
        // a joined memory is one lambda before it is simplified (SeaHorn:
        // bind::lite at the phi, then write simplifies)
        if (mem && isOpX<expr::ITE>(val))
          if (expr::Expr f = sem.coerceMemory(val, reg))
            val = f;
        if (lsimp) {
          unsigned k = std::max(1u, (unsigned)lambdaSimplifyEvery);
          if ((lstats.writes - 1) % k == 0) {
            auto t0 = std::chrono::steady_clock::now();
            val = lsimp->simplify(val);
            lstats.seconds += std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            ++(mem ? lstats.memory : lstats.scalar);
          } else {
            ++lstats.skipped;
          }
        }
        // well-scoped terms (Lambda.md): no free bound variable in a
        // written value
        if (lambdaCheckScopes && sea::SeaOpSem::hasFreeBoundVar(val)) {
          std::ostringstream os;
          os << *reg;
          sea::diag::error("vc.lambda-free-bvar",
                           "a value written to " + os.str() +
                               " has a free bound variable (write " +
                               std::to_string(lstats.writes) + ")");
        }
        if (mem) {
          expr::Expr f = sem.coerceMemory(val, reg);
          if (!f) {
            sea::diag::error("vc.lambda-unexpected",
                             "a memory value has no functional form");
            return val;
          }
          val = f;
        }
        return val;
      };
    // case splitting: the run's levels (--split / --split-leaves), or the
    // candidates of --split-auto's pick run (ite-valued addresses)
    std::unique_ptr<sea::SplitRun> splitRun;
    std::unique_ptr<sea::SplitCensus> census;
    if (splitPickRun) {
      // the Boolean candidates only for --split-candidates (--split-auto's
      // pick run installs no guard hook)
      census = std::make_unique<sea::SplitCensus>(splitCandidatesOpt);
      census->install(sem, ctx);
    } else if (!splitTargets.empty()) {
      splitRun = std::make_unique<sea::SplitRun>(splitTargets, splitPrefix,
                                                 splitMaxPaths);
      splitRun->install(sem, ctx);
      if (splitPlant == "drop-case")
        splitRun->setPlant(sea::SplitRun::Plant::DropCase);
      else if (splitPlant == "wrong-value")
        splitRun->setPlant(sea::SplitRun::Plant::WrongValue);
    }
    // --ite-stats: after the hooks above (it chains to them)
    std::unique_ptr<sea::IteCensus> iteCensus;
    if (iteStats && !iteStatsDone) {
      iteCensus = std::make_unique<sea::IteCensus>(program);
      iteCensus->install(sem, ctx);
    }
    sea::VCGen vcgen(sem, joinEncoding);
    vcgen.setReach(reach);
    vcgen.setJoinCases(vcJoinCases, {vcCasesReorder, vcCasesDropDefault});
    lap("setup");
    Phase encodePhase("encode", vcRuns);
    std::optional<EncodeMeter> meter;
    if (logPhase.on(sea::log::Flow) || memLimit) {
      meter.emplace(efac, vcRuns);
      vcgen.setProgress(
          [&meter](const sea::EncodeProgress &p) { (*meter)(p); });
    }
    if (failed(vcgen.genVc(program, ctx)))
      return 1;
    lap("encode");
    if (splitRun) {
      if (!splitRun->failure().empty()) {
        sea::diag::error(splitRun->unstable() ? "vc.split-unstable"
                                              : "vc.split-target",
                         "cannot split: " + splitRun->failure());
        return 1;
      }
      splitPart.levels.clear();
      for (const sea::SplitLevel &l : splitRun->levels()) {
        splitPart.levels.emplace_back(l.cases.size(), l.chosen);
        if (l.cases.size() < 2)
          sea::diag::warn("vc.split-trivial",
                          splitTargets[l.target].name +
                              ": one case only (its ite tree has a single "
                              "distinct leaf); no split at this level");
      }
      splitPart.choice = splitRun->choiceText();
      splitPart.leafStructural = splitRun->leafStructural();
      if (splitRun->levels().empty())
        sea::diag::warn("vc.split-unmet",
                        "no split target was met by the encoding (each is "
                        "in a block no path reaches): the part is the "
                        "whole VC");
    }
    if (iteCensus) {
      printIteStats(*iteCensus, program);
      iteStatsDone = true;
    }
    SEA_PHASE_END(encodePhase)
        .kv("nodes-made", efac.nodesMade())
        .kv("sides", side.size())
        .kv("guarantees", ctx.guarantees().size());
    // the final normalization pass: every side condition
    // and guarantee, then the query, with the cache of the load and store
    // rewriting
    std::optional<Phase> simplifyPhase;
    if (sem.simplifies())
      simplifyPhase.emplace("simplify");
    if (sem.simplifies()) {
      sem.setTrailContext("side");
      for (expr::Expr &e : side)
        e = sem.simplify(e);
      for (expr::Expr &e : ctx.guarantees())
        e = sem.simplify(e);
    }
    lap("simplify-sides");
    // the memory values may hold store-maps, which no solver knows: the
    // query goes through the boundary (StoreMap.hh)
    expr::Expr query = reach ? sea::mkReachQuery(ctx) : sea::mkQuery(ctx);
    // the part's condition (case splitting): VC[t := value] /\ condition
    if (splitRun)
      if (expr::Expr c = splitRun->partCondition(efac))
        query = expr::op::boolop::land(query, c);
    if (reach && reachPlant == "skip-assumes") {
      const auto &ap = sem.assumeSidePositions();
      std::set<size_t> skip(ap.begin(), ap.end());
      expr::ExprVector kept;
      for (size_t x = 0; x < side.size(); ++x)
        if (!skip.count(x))
          kept.push_back(side[x]);
      query = expr::op::boolop::land(kept, efac);
    }
    if (reach && reachPlant == "block-last" && !reachTargets.empty())
      if (Block *b = reachTargets.back().op->getBlock()->getSinglePredecessor())
        if (expr::Expr g = vcgen.guard(b))
          query = expr::mk<expr::AND>(query, expr::op::boolop::lneg(g));
    if (lambdaMem && lambdaFinalSimplify) {
      auto t0 = std::chrono::steady_clock::now();
      sea::EZ3 fz3(efac);
      query = sea::z3_simplify(fz3, query);
      lstats.finalSeconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
              .count();
    }
    // names are checked on the query as encoded too: a constant the
    // normalization removes (x - x, x ^ x) still has its value evaluated on
    // replay, so a clash among those must not go unseen
    // also the boundary's name-clash guard (PtrSort): erasure maps a ptr(w)
    // constant to a bv(w) constant of the same name, so two distinct constants
    // with one name before erasure could become one term after it
    std::string clash;
    if (failed(sea::verifyUniqueNames(query, clash))) {
      sea::diag::error("vc.ptr-erase-name-clash",
                       "two distinct VC constants are both named '" + clash +
                           "' (after the pointer sort is erased they would "
                           "be one constant)");
      return 1;
    }
    lap("mkquery");
    sem.setTrailContext("query");
    query = sem.simplify(query);
    lap("simplify-query");
    // the origins of the simplified terms: the rewriter caches every term
    // with its normal form (simplify is on)
    if (originsOn && sem.simplifies() && sem.seamRewriter()) {
      const expr::RewriteCache &cache = sem.seamRewriter()->cache();
      origins = origins.through([&](const expr::Expr &n) {
        expr::Expr r = n;
        for (unsigned hop = 0; hop < 16; ++hop) {
          const expr::Expr *f = cache.find(r, 0);
          if (!f || f->get() == r.get())
            break;
          r = *f;
        }
        return r;
      });
    }
    // the unknown hint counts its open alias questions (only then: a DAG
    // walk after the solver); a reference, the nodes are hash-consed
    expr::Expr hintQuery;
    if (!splitOn && !splitPickRun && !reach && sem.seamRewriter())
      hintQuery = query;
    if (simplifyPhase)
      SEA_PHASE_END(*simplifyPhase).kv("nodes-made", efac.nodesMade());
    // census (PtrSort S4): the loaded pointers no rule resolved, by what the
    // PTR_FROM_BITS holds (select: an undecided read; concat: a split load;
    // wselect / ite / other), before the boundary
    std::map<std::string, uint64_t> fromBitsLeft, aliasOpen;
    // the same-object alias questions overlap() reduced to offset
    // compares (Rules::aliasReduced), in their normal forms: the census
    // looks for those (a reduced term is normalized further after it is
    // recorded; one that normalizes to a constant was decided). The
    // rewriter's counters and trail are left as they were.
    std::unordered_set<expr::ENode *> sameObjectOpen;
    std::vector<expr::Expr> sameObjectKeep;
    uint64_t sameObjectReduced = 0;
    // the kind of an alias question by its leaf pairs (Rules::eqPairs: an
    // ite pointer is its leaves, not one pointer without a view): every pair
    // by what the views of its two pointers say; the question is the least
    // decided kind among its pairs (diff-object first: A-INB decides such a
    // pair at an access, so one left open is the census's alarm), and
    // `many-leaves` when it has more pairs than the decisions look at
    std::map<std::string, uint64_t> aliasOpenPairs, memOpOpen;
    auto aliasKind = [&](const expr::Expr &p, const expr::Expr &q,
                         std::map<std::string, uint64_t> &pairs) {
      const auto &R = sem.seamRewriter()->rules();
      llvm::SmallVector<std::pair<expr::Expr, expr::Expr>, 8> lp;
      if (!R.eqPairs(p, q, lp, R.options().maxDecideLeaves))
        return "many-leaves";
      static const char *order[] = {"diff-object", "one-unknown",
                                    "both-unknown", "same-object"};
      unsigned worst = 3;
      for (const auto &[a, b] : lp) {
        auto va = R.view(a), vb = R.view(b);
        unsigned k = va && vb   ? (va->base == vb->base ? 3 : 0)
                     : va || vb ? 1
                                : 2;
        ++pairs[order[k]];
        ++pairs["all"];
        worst = std::min(worst, k);
      }
      return order[worst];
    };
    // the open in-tests of memory-operation reads (Rules::memOpOpen: one per
    // leaf pair, or one over whole ites), by their normal forms, each with
    // the kind of its read / destination pair
    std::unordered_map<expr::ENode *, expr::Expr> memOpOpenAt;
    std::vector<expr::Expr> memOpKeep;
    if (seamStats && sem.seamRewriter()) {
      auto &rw = *sem.seamRewriter();
      const expr::ExprSet red = rw.rules().aliasReduced();
      sameObjectReduced = red.size();
      expr::RewriteStats saved = rw.stats();
      rw.stats().trail = nullptr;
      for (const expr::Expr &t : red) {
        expr::Expr f = rw.simplify(t);
        if (!expr::isOpX<expr::TRUE>(f) && !expr::isOpX<expr::FALSE>(f)) {
          sameObjectKeep.push_back(f);
          sameObjectOpen.insert(f.get());
        }
      }
      for (const expr::Expr &t : rw.rules().memOpOpen()) {
        expr::Expr f = rw.simplify(t);
        if (!expr::isOpX<expr::TRUE>(f) && !expr::isOpX<expr::FALSE>(f)) {
          memOpKeep.push_back(f);
          memOpOpenAt.emplace(f.get(), t);
        }
      }
      rw.stats() = saved;
    }
    // the companion arrays the query still reads (S6: a guard that did not
    // fold), counted vc.ptr-companion-arrays
    uint64_t companionArrays = 0;
    if (sem.ptrCompanion()) {
      std::unordered_set<expr::ENode *> seen;
      std::vector<expr::ENode *> stack{query.get()};
      while (!stack.empty()) {
        expr::ENode *n = stack.back();
        stack.pop_back();
        if (!seen.insert(n).second)
          continue;
        if (expr::bind::IsConst()(expr::Expr(n))) {
          std::ostringstream os;
          os << *expr::bind::fname(expr::bind::fname(expr::Expr(n)));
          if (llvm::StringRef(os.str()).starts_with("ptrc."))
            ++companionArrays;
          continue;
        }
        for (size_t i = 0; i < n->arity(); ++i)
          stack.push_back(n->arg(i));
      }
    }
    auto companionLine = [&](llvm::raw_ostream &os) {
      if (sem.ptrCompanion())
        os << "vc.ptr-companion: loads=" << sem.companionLoads()
           << " stores=" << sem.companionStores()
           << " copies=" << sem.companionCopies()
           << " fresh=" << sem.companionFresh() << "\n"
           << "vc.ptr-companion-arrays: " << companionArrays << "\n";
      if (sem.companionAssumed())
        os << "seam.ptrint: assumed loads=" << sem.companionAssumed() << "\n";
      if (sem.ptrIntChecking())
        os << "seam.ptrint: checked loads=" << sem.companionChecked() << "\n";
    };
    // --split-auto's pick run: the open alias questions
    std::vector<expr::ENode *> openOverlaps;
    if (seamStats && sem.seam()) {
      std::unordered_set<expr::ENode *> seen;
      std::vector<expr::ENode *> stack{query.get()};
      while (!stack.empty()) {
        expr::ENode *n = stack.back();
        stack.pop_back();
        if (!seen.insert(n).second)
          continue;
        // an alias question the rewriter left open (IS_OVERLAP, D13), by
        // what is known of its two pointers (Rules::view)
        if (sameObjectOpen.count(n)) {
          ++aliasOpen["same-object"];
          ++aliasOpen["all"];
          if (splitPickRun)
            ++splitSameObjectOpen;
        }
        if (expr::isOpX<expr::IS_OVERLAP>(n) && sem.seamRewriter()) {
          ++aliasOpen[aliasKind(expr::Expr(n->arg(0)), expr::Expr(n->arg(2)),
                                aliasOpenPairs)];
          ++aliasOpen["all"];
          if (splitPickRun)
            openOverlaps.push_back(n);
        }
        if (auto it = memOpOpenAt.find(n); it != memOpOpenAt.end()) {
          // the recorded (idx - d) <u n: its pointer difference
          const expr::Expr diff = it->second->arg(0);
          if (!expr::isOpX<expr::PTR_DIFF>(diff)) {
            // an exact numeral difference has one base
            ++memOpOpen["same-object"];
          } else {
            std::map<std::string, uint64_t> ignored;
            ++memOpOpen[aliasKind(diff->arg(0), diff->arg(1), ignored)];
          }
          ++memOpOpen["all"];
        }
        if (expr::isOpX<expr::PTR_FROM_BITS>(n)) {
          auto kind = [](expr::ENode *a) -> const char * {
            return expr::isOpX<expr::SELECT>(a)           ? "select"
                   : expr::isOpX<expr::BCONCAT>(a)        ? "concat"
                   : expr::isOpX<expr::W_SELECT>(a)       ? "wselect"
                   : expr::isOpX<expr::ITE>(a)            ? "ite"
                   : expr::isOpX<expr::PTR_TO_BITS>(a)    ? "to-bits"
                   : expr::op::bv::isBvNum(expr::Expr(a)) ? "numeral"
                                                          : "other";
          };
          expr::ENode *a = n->arg(0);
          ++fromBitsLeft[kind(a)];
          // an ite: its leaves, by kind
          if (expr::isOpX<expr::ITE>(a)) {
            std::vector<expr::ENode *> lv{a};
            while (!lv.empty()) {
              expr::ENode *l = lv.back();
              lv.pop_back();
              if (expr::isOpX<expr::ITE>(l)) {
                lv.push_back(l->arg(1));
                lv.push_back(l->arg(2));
              } else {
                ++fromBitsLeft[std::string("ite-leaf.") + kind(l)];
              }
            }
          }
        }
        for (size_t i = 0; i < n->arity(); ++i)
          stack.push_back(n->arg(i));
      }
    }
    // --split-auto's pick run: each candidate (an ite-valued address) by
    // the open alias questions whose pointers contain its term; no solver
    // (sea/VC/Split.h, SplitCensus)
    if (splitPickRun) {
      census->attribute(sem, openOverlaps, splitMaxPaths);
      // --split-candidates: the array reads of the query at the solver
      // boundary (Ackermann pairs their indices), by the candidates in
      // their indices
      if (splitCandidatesOpt) {
        expr::Expr solverQuery = sem.solverTerm(query);
        if (!solverQuery)
          return 1; // vc.memop-term-at-boundary
        splitReads = census->attributeReads(sem, solverQuery);
        splitArithNodes =
            census->attributeArith(sem, solverQuery, splitMaxPaths);
        splitMemopOpen = memOpOpen["all"];
      }
      sea::ValueNames vn(program);
      for (const sea::SplitAddrCandidate &a : census->addrs) {
        SplitCandidate c;
        c.value = a.value;
        c.name = vn.name(c.value).str();
        c.loc = sea::formatLoc(c.value.getLoc());
        c.open = a.open;
        c.leaves = a.cases.size();
        c.own = a.own;
        c.direct = a.direct;
        c.unknownLeaves = a.unknownLeaves;
        c.reads = a.reads;
        splitCandidates.push_back(std::move(c));
      }
      std::stable_sort(splitCandidates.begin(), splitCandidates.end(),
                       [](const SplitCandidate &a, const SplitCandidate &b) {
                         // ties (one unknown leaf behind several ites):
                         // the most leaves
                         return a.open != b.open ? a.open > b.open
                                                 : a.leaves > b.leaves;
                       });
      splitBoolRecorded = census->boolCands.size();
      // a candidate with the term of another is listed with it (same=), not
      // on its own (sea::SplitBoolCandidate::sameAs)
      std::vector<int> listedAt(census->boolCands.size(), -1);
      auto boolName = [&](const sea::SplitBoolCandidate &b) {
        return b.block ? vn.name(b.block).str() : vn.name(b.value).str();
      };
      for (size_t i = 0; i < census->boolCands.size(); ++i) {
        const sea::SplitBoolCandidate &b = census->boolCands[i];
        if (!b.open && !b.reads)
          continue;
        if (b.sameAs >= 0)
          continue;
        SplitBoolCandidate c;
        c.guard = b.block != nullptr;
        c.name = boolName(b);
        if (b.block) {
          // a block has no location: its first op's
          if (!b.block->empty())
            c.loc = sea::formatLoc(b.block->front().getLoc());
        } else {
          c.loc = sea::formatLoc(b.value.getLoc());
        }
        c.open = b.open;
        c.addrs = b.addrs;
        c.reads = b.reads;
        c.condReads = b.condReads;
        c.covers = b.covers;
        listedAt[i] = splitBoolCandidates.size();
        splitBoolCandidates.push_back(std::move(c));
      }
      for (const sea::SplitBoolCandidate &b : census->boolCands)
        if (b.sameAs >= 0 && listedAt[b.sameAs] >= 0)
          splitBoolCandidates[listedAt[b.sameAs]].same.push_back(boolName(b));
      // by the open questions; ties: in more address terms, then encoding
      // order. Without open questions (a heuristic for an Ackermann
      // residue): by the read indices, then those where it is a whole ite
      // condition, then the other candidates in its term (an and before its
      // conjuncts), then the address terms, then encoding order.
      std::stable_sort(
          splitBoolCandidates.begin(), splitBoolCandidates.end(),
          [](const SplitBoolCandidate &a, const SplitBoolCandidate &b) {
            if (a.open != b.open)
              return a.open > b.open;
            if (a.open == 0) {
              if (a.reads != b.reads)
                return a.reads > b.reads;
              if (a.condReads != b.condReads)
                return a.condReads > b.condReads;
              if (a.covers != b.covers)
                return a.covers > b.covers;
            }
            return a.addrs > b.addrs;
          });
      // the arithmetic they control: integer candidates (one part per
      // leaf), then the Boolean ones (one line per term, as above); by the
      // arithmetic nodes, then integers before Booleans, then (integers)
      // the other integer candidates computed from it, then the nodes with
      // its term itself in an operand, then more cases, then encoding order
      // (untuned)
      splitIntRecorded = census->ints.size();
      for (const sea::SplitIntCandidate &n : census->ints) {
        if (!n.arith)
          continue;
        SplitArithCandidate c;
        c.name = vn.name(n.value).str();
        c.loc = sea::formatLoc(n.value.getLoc());
        c.target = "int";
        c.arith = n.arith;
        c.mul = n.arithMul;
        c.div = n.arithDiv;
        c.direct = n.arithDirect;
        c.cases = n.leaves;
        c.feeds = n.feeds;
        splitArithCandidates.push_back(std::move(c));
      }
      {
        std::vector<int> arithAt(census->boolCands.size(), -1);
        for (size_t i = 0; i < census->boolCands.size(); ++i) {
          const sea::SplitBoolCandidate &b = census->boolCands[i];
          if (!b.arith || b.sameAs >= 0)
            continue;
          SplitArithCandidate c;
          c.name = boolName(b);
          if (b.block) {
            if (!b.block->empty())
              c.loc = sea::formatLoc(b.block->front().getLoc());
          } else {
            c.loc = sea::formatLoc(b.value.getLoc());
          }
          c.target = b.block ? "guard" : "value";
          c.arith = b.arith;
          c.mul = b.arithMul;
          c.div = b.arithDiv;
          c.cases = 2;
          arithAt[i] = splitArithCandidates.size();
          splitArithCandidates.push_back(std::move(c));
        }
        for (const sea::SplitBoolCandidate &b : census->boolCands)
          if (b.sameAs >= 0 && arithAt[b.sameAs] >= 0)
            splitArithCandidates[arithAt[b.sameAs]].same.push_back(boolName(b));
      }
      std::stable_sort(
          splitArithCandidates.begin(), splitArithCandidates.end(),
          [](const SplitArithCandidate &a, const SplitArithCandidate &b) {
            if (a.arith != b.arith)
              return a.arith > b.arith;
            const bool ai = a.target == "int", bi = b.target == "int";
            if (ai != bi)
              return ai;
            if (a.feeds != b.feeds)
              return a.feeds > b.feeds;
            if (a.direct != b.direct)
              return a.direct > b.direct;
            return a.cases > b.cases;
          });
      splitOpenAliases = openOverlaps.size();
      splitPickSeconds = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - vcStart)
                             .count();
      if (!splitCandidatesOpt)
        vout() << "split.auto: open-aliases=" << openOverlaps.size()
               << " candidates=" << splitCandidates.size()
               << " vc-time=" << llvm::format("%.3f", splitPickSeconds) << "\n";
      return 0;
    }
    // the layout (obj.k = <numeral>: unconditional side conditions of the
    // query, OpSem.cpp sea.segment), for pairs that are never equal and for
    // alignment proofs of initial words
    std::unordered_map<expr::ENode *, expr::mpz_class> layoutValues;
    for (const expr::ObjectBase &o : vcgen.baseRegistry().objects()) {
      expr::mpz_class v = expr::op::bv::toMpz(sem.numeral(o.base));
      layoutValues.emplace(o.constant.get(), v);
      // the readers (InitialWords, Ackermann) see the erased query: the bv
      // twin of the ptr constant (PtrSort S2e; the eraser pins it)
      if (sem.seamRewriter())
        layoutValues.emplace(
            sem.seamRewriter()->rules().erase(o.constant).get(), v);
    }
    std::unique_ptr<expr::InitialWords> words;
    // the stride rule's record (--ack-stride; the plain boundary only: the
    // words and xrt paths expand the nodes first and record nothing)
    expr::AccessOrigins accessOrigins;
    if (seamWords != WordsMode::Off)
      words = std::make_unique<expr::InitialWords>(
          8,
          seamWords == WordsMode::All ? expr::InitialWords::Mode::All
                                      : expr::InitialWords::Mode::Auto,
          &layoutValues);
    // EXPERIMENT: the guarded read table in place of the boundary
    std::unique_ptr<expr::xrt::Translator> xtr;
    std::unique_ptr<expr::xrt::Lowering> xlw;
    const bool xrtOn = !xReadTable.empty();
    uint64_t xrtDagStd = 0, xrtDagTranslated = 0;
    // the origins follow the boundary along the structure it keeps
    expr::Expr preBoundary = originsOn ? query : expr::Expr();
    Phase solverTermPhase("solver-term");
    if (xrtOn) {
      if (!sem.seam() || !sem.seamRewriter())
        return fail("--x-read-table needs --seam");
      expr::SeaMRewriter *rw = sem.seamRewriter();
      query =
          rw->expandPtrIndex(rw->expandStoreMaps(rw->expandPtrIndex(query)));
      if (dagStats) {
        std::string why;
        expr::Expr std = sem.solverTerm(query, why);
        xrtDagStd = std ? expr::dagSize(std) : 0;
      }
      lap("solver-term");
      xtr = std::make_unique<expr::xrt::Translator>(efac, sem.baseRegistry(),
                                                    rw->rules(), xReadTableCap,
                                                    inb && xReadTableInb);
      std::string why;
      query = xtr->translate(query, why);
      if (!query)
        return fail("--x-read-table: " + why);
      if (dagStats)
        xrtDagTranslated = expr::dagSize(query);
      lap("xrt-translate");
    } else if (seamWords != WordsMode::Off) {
      if (!sem.seamRewriter())
        return fail("--seam-words needs --seam");
      std::string why;
      expr::Expr before = query;
      words->recordReads(wordsCheck);
      query = sem.seamRewriter()->solverTermWords(query, *words, why);
      if (!query)
        return fail("--seam-words: " + why);
      lap("solver-term");
      if (wordsCheck) {
        // the byte meaning (solverTerm) against the word query, given the
        // defining equations of the words read: unsat iff they agree on
        // every model of the bytes (no alignment premise: A_w is the packing
        // at every index read; SeaM-initial-words.md rev 3, point 6)
        expr::Expr bytesQ = sem.solverTerm(before);
        if (!bytesQ)
          return 1;
        expr::ExprVector defs;
        for (const expr::InitialWords::Census &c : words->census()) {
          if (!c.chosen)
            continue;
          for (const expr::Expr &idx : words->wordIndices(c.array.get())) {
            unsigned n = expr::op::bv::width(expr::sort::arrayIndexTy(
                expr::op::bind::declaredSort(c.array)));
            expr::Expr cat;
            for (unsigned k = 0; k < words->wordBytes(); ++k) {
              expr::Expr at =
                  k ? expr::mk<expr::BADD>(
                          idx, expr::op::bv::bvnum(
                                   expr::mpz_class((unsigned long)k), n, efac))
                    : idx;
              expr::Expr b = expr::op::array::select(c.array, at);
              cat = cat ? expr::op::bv::concat(b, cat) : b;
            }
            defs.push_back(
                expr::mk<expr::EQ>(expr::op::array::select(c.word, idx), cat));
          }
        }
        defs.push_back(expr::mk<expr::XOR>(bytesQ, query));
        auto z = std::make_unique<sea::solver::z3_solver_impl>(efac);
        if (wordsCheckTimeout) {
          sea::ZParams<sea::EZ3> params(z->get_context());
          params.set("timeout", wordsCheckTimeout * 1000);
          z->get_solver().set(params);
        }
        bool added = true;
        for (const expr::Expr &d : defs)
          added = added && z->add(d);
        auto r = added ? z->check() : sea::solver::SolverResult::UNKNOWN;
        const char *verdict = r == sea::solver::SolverResult::UNSAT ? "ok"
                              : r == sea::solver::SolverResult::SAT ? "failed"
                                                                    : "unknown";
        vout() << "words.check: " << verdict << " defs=" << defs.size() - 1
               << "\n";
        if (r == sea::solver::SolverResult::SAT) {
          // localize: the first read whose decoding differs from its bytes
          // (the original read's index translated too: equal under defs)
          for (const auto &[orig, dec] : words->reads()) {
            expr::Expr bytesR = orig;
            if (expr::isOpX<expr::W_SELECT>(orig)) {
              expr::WideExpander wx;
              bytesR = wx.expand(orig);
            }
            auto z1 = std::make_unique<sea::solver::z3_solver_impl>(efac);
            bool ok1 = true;
            for (size_t d = 0; d + 1 < defs.size(); ++d)
              ok1 = ok1 && z1->add(defs[d]);
            ok1 = ok1 &&
                  z1->add(expr::mk<expr::NEG>(expr::mk<expr::EQ>(bytesR, dec)));
            if (ok1 && z1->check() == sea::solver::SolverResult::SAT) {
              vout() << "words.check-read: " << *orig << "\n";
              vout() << "words.check-decoded: " << *dec << "\n";
              break;
            }
          }
          sea::diag::error("vc.words-check-failed",
                           "the word query differs from the byte query: a "
                           "bug in the initial-words encoding");
          return 2;
        }
        if (r != sea::solver::SolverResult::UNSAT)
          sea::diag::warn("vc.words-check-unknown",
                          "--words-check: the solver gave no answer");
        lap("words-check");
      }
    } else {
      // the stride rule's record: the address each read had before the
      // nodes were expanded (the query only)
      if (strideOn && sem.seamRewriter())
        sem.seamRewriter()->recordAccesses(&accessOrigins);
      query = sem.solverTerm(query);
      if (sem.seamRewriter())
        sem.seamRewriter()->recordAccesses(nullptr);
      if (!query)
        return 1; // vc.memop-term-at-boundary
      lap("solver-term");
    }
    SEA_PHASE_END(solverTermPhase).kv("nodes-made", efac.nodesMade());
    if (originsOn) {
      origins = origins.along(preBoundary, query, [&](const expr::Expr &e) {
        return sem.erasedImage(e);
      });
      preBoundary = expr::Expr();
    }
    if (failed(sea::verifyUniqueNames(query, clash)))
      return fail("two distinct VC constants are both named '" + clash + "'");
    lap("unique-names");
    // the pointer sort (PtrSort S2e, S2g) ends at the boundary: a ptr(w) term,
    // node, marker or PTR_EQ left in the query is a bug in a boundary path.
    // Before Ackermann, which keeps an array whose index sort is not bv(w)
    // without a word.
    auto ptrSortLeft = [&](const expr::Expr &q) {
      if (!sem.hasPtrTerms(q))
        return false;
      sea::diag::error("vc.ptr-sort-at-boundary",
                       "internal error: a pointer-sorted term (ptr(w), a "
                       "PTR_INDEX node, a marker, PTR_EQ) reached the "
                       "solver (docs/design/PtrSort.md)");
      return true;
    };
    if (ptrSortLeft(query))
      return 1;
    // cmp.diff-base decisions whose A-INB premise names no recorded access
    // (seam::Rules::checkInbPremise)
    if (sem.seamRewriter()) {
      const auto &rules = sem.seamRewriter()->rules();
      auto it =
          sem.seamRewriter()->stats().rules.find("premise.inb-unrecorded");
      if (it != sem.seamRewriter()->stats().rules.end() && it->second) {
        std::string ex;
        for (const std::string &e : rules.inbUnrecorded())
          ex += "\n  " + e;
        sea::diag::warn("vc.inb-premise-unrecorded",
                        Twine(it->second) +
                            " address(es) of cmp.diff-base decisions lie in no "
                            "access whose A-INB condition the run recorded "
                            "(the rule's premise is not stated); first:" +
                            ex);
      }
    }
    // Ackermann lowering of read-only arrays (an encoding step of the query,
    // docs/design/Ackermann.md); its table rebuilds the arrays on a model
    Phase ackPhase("ackermann");
    expr::Ackermannizer ack;
    ack.setValues(layoutValues);
    if (strideOn && !accessOrigins.byIndex.empty()) {
      ack.setAccessOrigins(&accessOrigins);
      std::unordered_set<expr::ENode *> size0;
      for (const expr::ObjectBase &o : vcgen.baseRegistry().objects())
        if (o.size.isZero())
          size0.insert(o.constant.get());
      ack.setSize0Bases(std::move(size0));
      ack.setAssumeInBounds(inb);
    }
    ack.analyze(query);
    // auto: not for Bitwuzla, whose own array handling (lemmas on demand)
    // beat the eager constraints on every measured query (Ackermann.md)
    const bool ackOn =
        !xrtOn && (ackermannize == AckMode::On ||
                   (ackermannize == AckMode::Auto && !ack.hasStore() &&
                    backend != Backend::Bitwuzla));
    // --eq-audit: the constrained pairs (of the lowering itself, or of a
    // lowering made only for the audit when this run does not lower)
    std::unique_ptr<expr::Ackermannizer> auditAck;
    if (auditOn) {
      if (ackOn) {
        ack.recordPairs(true);
      } else if (!ack.hasStore()) {
        auditAck = std::make_unique<expr::Ackermannizer>();
        auditAck->setValues(layoutValues);
        auditAck->recordPairs(true);
        (void)auditAck->lower(query);
      }
    }
    if (ackOn && !ackDumpPairs.empty())
      ack.recordPairs(true);
    if (ackOn && originsOn)
      ack.keepImages(true);
    // the origins of the lowering's input (the reads' selects)
    sea::ExprOrigins preAckOrigins;
    if (ackOn)
      query = ack.lower(query);
    if (ackOn && originsOn) {
      preAckOrigins = origins;
      origins =
          origins.through([&](const expr::Expr &n) { return ack.image(n); });
    }
    if (ackOn && !ackDumpPairs.empty()) {
      std::error_code ec;
      llvm::raw_fd_ostream os(ackDumpPairs, ec);
      if (ec)
        return fail("--ack-dump-pairs: " + ec.message());
      const auto &arrs = ack.arrays();
      for (size_t a = 0; a < arrs.size(); ++a) {
        os << "array " << a << " "
           << *expr::op::bind::fname(arrs[a].array->first()) << "\n";
        for (size_t k = 0; k < arrs[a].reads.size(); ++k) {
          // the index, then the loads the read came from (the distinct
          // locations of the selects merged into it; - for none)
          os << "read " << a << " " << k << " " << *arrs[a].reads[k].index
             << " loc=";
          std::vector<std::string> ls;
          for (const expr::Expr &sel : arrs[a].reads[k].selects)
            for (mlir::Location l : preAckOrigins.of(sel.get())) {
              std::string t = sea::formatLoc(l);
              if (t.empty())
                t = "-";
              if (!llvm::is_contained(ls, t))
                ls.push_back(t);
            }
          if (ls.empty())
            os << "-";
          llvm::interleave(ls, os, ";");
          os << "\n";
        }
      }
      for (const auto &p : ack.constrainedPairs())
        os << "pair " << p.array << " " << p.k << " " << p.l << "\n";
    }
    lap("ackermannize");
    if (xrtOn) {
      expr::xrt::Mode mode;
      if (xReadTable == "plain")
        mode = expr::xrt::Mode::Plain;
      else if (xReadTable == "flat")
        mode = expr::xrt::Mode::Flat;
      else if (xReadTable == "arrays")
        mode = expr::xrt::Mode::Arrays;
      else if (xReadTable == "objack")
        mode = expr::xrt::Mode::ObjAck;
      else if (xReadTable == "mux")
        mode = expr::xrt::Mode::Mux;
      else
        return fail("--x-read-table: plain|flat|arrays|objack|mux");
      xlw = std::make_unique<expr::xrt::Lowering>(efac, sem.baseRegistry(),
                                                  *xtr, mode, xReadTableMux);
      expr::Expr preLower = query;
      query = xlw->lower(query);
      if (originsOn)
        origins = origins.along(preLower, query);
      lap("xrt-lower");
    }
    SEA_PHASE_END(ackPhase)
        .kv("on", ackOn)
        .kv("arrays", ack.stats().lowered)
        .kv("pairs", ack.stats().pairs)
        .kv("nodes-made", efac.nodesMade());
    // the size of what the solver gets: distinct nodes (shared subterms
    // once), per operator
    if (dagStats) {
      raw_ostream &os = statOut();
      os << "vc.dag-size: " << expr::dagSize(query) << "\n";
      std::map<std::string, size_t> perOp;
      llvm::DenseSet<const expr::ENode *> seen;
      llvm::SmallVector<const expr::ENode *> stack{query.get()};
      while (!stack.empty()) {
        const expr::ENode *n = stack.pop_back_val();
        if (!seen.insert(n).second)
          continue;
        ++perOp[n->op().name()];
        for (auto it = n->args_begin(), end = n->args_end(); it != end; ++it)
          stack.push_back(*it);
      }
      for (const auto &[op, n] : perOp)
        os << "vc.dag-op: " << op << " " << n << "\n";
    }
    // the multiplications, divisions and remainders of what the solver
    // gets, by operand shape and source location (tools/sea-vc/ArithStats.h)
    if (arithStats) {
      sea::ArithStats census(query, origins);
      census.emit([&](const sea::out::Record &rec) {
        // with --smt2 (text) the statistics go to stderr
        if (smt2 && !gOut->jsonl())
          errs() << rec.text << "\n";
        else
          gOut->emit(rec);
      });
    }
    // the VC is generated: encoding, with --seam the rewriting of every load
    // and store, and the query
    double vcSeconds = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - vcStart)
                           .count();

    // Backend::Bitwuzla without the library is an error (null solver)
    // \p limits: --timeout / --mem-limit (the solver of the verdict only)
    auto makeSolver = [&](bool limits =
                              false) -> std::unique_ptr<sea::solver::Solver> {
      const unsigned ms = limits ? solveTimeout * 1000 : 0;
      if (backend == Backend::Z3) {
        auto z = std::make_unique<sea::solver::z3_solver_impl>(efac);
        if (ms) {
          sea::ZParams<sea::EZ3> params(z->get_context());
          params.set("timeout", ms);
          z->get_solver().set(params);
        }
        return z;
      }
      if (backend == Backend::Bitwuzla) {
#ifdef WITH_BITWUZLA
        sea::solver::bitwuzla_solver_impl::solver_options o;
        if (ms)
          o["time-limit-per"] = std::to_string(ms);
        if (limits && memLimit)
          o["memory-limit"] = std::to_string(uint64_t(memLimit) * 1024);
        return std::make_unique<sea::solver::bitwuzla_solver_impl>(efac,
                                                                   nullptr, o);
#else
        sea::diag::error("vc.solver-unavailable",
                         "this sea-vc was built without Bitwuzla (configure "
                         "with -DBITWUZLA_ROOT=<install>)");
        return nullptr;
#endif
      }
#ifndef WITH_YICES2
      return nullptr; // refused after option parsing
#else
      sea::solver::yices_solver_impl::solver_options yo;
      for (const std::string &kv : yicesConfig) {
        auto [k, v] = StringRef(kv).split('=');
        yo[k.str()] = v.str();
      }
      auto y = std::make_unique<sea::solver::yices_solver_impl>(
          efac, yicesLogic.empty() ? nullptr : yicesLogic.c_str(), yo);
      if (!y->config_error().empty()) {
        sea::diag::error("vc.yices-config",
                         "Yices rejects --yices-logic / --yices-config " +
                             y->config_error());
        return nullptr;
      }
      for (const std::string &kv : yicesParams) {
        auto [k, v] = StringRef(kv).split('=');
        std::string err;
        if (!y->set_param(k.str(), v.str(), err)) {
          sea::diag::error("vc.yices-param",
                           "Yices rejects --yices-param " + kv + ": " + err);
          return nullptr;
        }
      }
      y->set_timeout(ms);
      return y;
#endif
    };
    // A-INB is assumed and reported, not checked. Definite violations (a
    // numeral offset outside the object) make the run start again without
    // it: a sanity check.
    if (inb && sem.seam() && sem.inbStaticViolationCount()) {
      sea::diag::warn("vc.seam-inb-violated",
                      "an access leaves its object (" +
                          Twine(sem.inbStaticViolationCount()) +
                          " access(es) with no alternative inside its "
                          "object: a numeral offset or an offset range "
                          "wholly outside it, or an object of size 0 such "
                          "as null): --seam's A-INB "
                          "does not hold; running without it");
      // name the accesses (the first few): where, and which check failed
      raw_ostream &os = statOut();
      for (const std::string &n : sem.inbViolationNotes())
        os << "vc.seam-inb-violated: at " << n << "\n";
      os << "vc.seam-inb-violated-count: " << sem.inbStaticViolationCount()
         << "\n";
      os.flush();
      if (mayRetry)
        return kRetryWithoutInb;
    }
    // --split-check: the construction of each level, by z3 (rlimit), on the
    // solver's terms (the boundary of the query): its case conditions are
    // exhaustive, and the chosen case's condition implies term = value
    if (splitRun && splitCheck) {
      using SR = sea::solver::SolverResult;
      auto unsatOf = [&](expr::Expr f, std::string &why) -> SR {
        expr::Expr g = sem.solverTerm(f, why);
        if (!g)
          return SR::UNKNOWN;
        auto z = std::make_unique<sea::solver::z3_solver_impl>(efac);
        if (splitCheckRlimit) {
          sea::ZParams<sea::EZ3> params(z->get_context());
          params.set("rlimit", unsigned(splitCheckRlimit));
          z->get_solver().set(params);
        }
        return z->add(g) ? z->check() : SR::UNKNOWN;
      };
      auto word = [](SR r) {
        return r == SR::UNSAT ? "ok" : r == SR::SAT ? "failed" : "unknown";
      };
      bool failedAny = false, unknownAny = false;
      unsigned nUnknownChecks = 0;
      const auto &lv = splitRun->levels();
      for (size_t i = 0; i < lv.size(); ++i) {
        const sea::SplitLevel &l = lv[i];
        expr::ExprVector conds;
        for (const sea::SplitCase &c : l.cases)
          conds.push_back(c.cond);
        std::string why1, why2;
        SR ex = unsatOf(
            expr::op::boolop::lneg(expr::op::boolop::lor(conds, efac)), why1);
        const sea::SplitCase &c = l.cases[l.chosen];
        expr::Expr eq =
            l.ptr ? expr::Expr(expr::mk<expr::PTR_EQ>(l.term, c.value))
                  : expr::Expr(expr::mk<expr::EQ>(l.term, c.value));
        SR exact = unsatOf(
            expr::op::boolop::land(c.cond, expr::op::boolop::lneg(eq)), why2);
        vout() << "split.check: level=" << i
               << " target=" << splitTargets[l.target].name
               << " cases=" << l.cases.size() << " exhaustive=" << word(ex)
               << " exact=" << word(exact) << "\n";
        if (!why1.empty() || !why2.empty())
          sea::diag::warn("vc.split-check-unexpressible",
                          "--split-check: a check has no solver form: " + why1 +
                              why2);
        failedAny |= ex == SR::SAT || exact == SR::SAT;
        unknownAny |= ex == SR::UNKNOWN || exact == SR::UNKNOWN;
        if (ex == SR::UNKNOWN || exact == SR::UNKNOWN)
          ++nUnknownChecks;
      }
      splitPart.checkUnknown = nUnknownChecks;
      splitPart.check = failedAny ? "failed" : unknownAny ? "unknown" : "ok";
      if (failedAny) {
        sea::diag::error("vc.split-check-failed",
                         "a split level is not exhaustive or not exact (a "
                         "bug in the case split construction)");
        return 5;
      }
      if (unknownAny)
        sea::diag::warn("vc.split-check-unknown",
                        "--split-check: z3 gave no answer within "
                        "--split-check-rlimit for a split level: its "
                        "construction is not checked");
    }
    // --vcgen-only: no solver at all
    std::unique_ptr<sea::solver::Solver> solver;
    if (!vcgenOnly) {
      solver = makeSolver(/*limits=*/true);
      if (!solver)
        return 1;
    }
    if (vcTypecheckPlant)
      query = expr::mk<expr::AND>(
          query, expr::mk<expr::EQ>(
                     expr::op::bv::bvnum(expr::mpz_class(1UL), 8, efac),
                     expr::op::bv::bvnum(expr::mpz_class(1UL), 16, efac)));
    if (vcTypecheck) {
      auto t0 = std::chrono::steady_clock::now();
      expr::TypeChecker tc;
      expr::TypeChecker::Result chk = tc.check(query);
      double sec =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
              .count();
      statOut() << "vc.typecheck-time: " << llvm::format("%.3f", sec) << "\n";
      if (!chk) {
        std::string t;
        llvm::raw_string_ostream os(t);
        if (chk.culprit)
          os << *chk.culprit << " (" << chk.why << ")";
        sea::diag::error("vc.ill-typed",
                         "the query is ill-typed (a bug in the VC): " +
                             os.str().substr(0, 400));
        return 1;
      }
    }
    // no solver knows the pointer sort: every boundary erases it first;
    // checked again here (after the lowerings that rebuild the query)
    if (ptrSortLeft(query))
      return 1;
    // add reports (through sea::diag) a query the backend cannot encode
    if (solver && !solver->add(query))
      return 1;
    // --split-smt2: the part's query (the solver's SMT-LIB), also without a
    // solver of the verdict (--vcgen-only)
    if (!splitSmt2.empty()) {
      std::unique_ptr<sea::solver::Solver> printer;
      sea::solver::Solver *ps = solver.get();
      if (!ps) {
        printer = makeSolver();
        if (!printer || !printer->add(query))
          return 1;
        ps = printer.get();
      }
      std::string path =
          splitSmt2 + ".part" + std::to_string(splitPartId) + ".smt2";
      std::error_code ec;
      llvm::raw_fd_ostream os(path, ec);
      if (ec)
        return fail("--split-smt2: cannot write " + path + ": " + ec.message());
      ps->to_smt_lib(os);
      vout() << "split.smt2: " << path << "\n";
    }
    // the SeaM counters: with --smt2 stdout is the query, so they go to stderr
    auto printSeamStatsText = [&](raw_ostream &os) {
      os << "vc-time: " << llvm::format("%.3f", vcSeconds) << "\n";
      if (lambdaMem) {
        os << "lambda.writes: " << lstats.writes << "\n";
        os << "lambda.simplify.memory: " << lstats.memory << "\n";
        os << "lambda.simplify.scalar: " << lstats.scalar << "\n";
        os << "lambda.simplify.skipped: " << lstats.skipped << "\n";
        os << "lambda.simplify-time: " << llvm::format("%.3f", lstats.seconds)
           << "\n";
        if (lambdaFinalSimplify)
          os << "lambda.final-simplify-time: "
             << llvm::format("%.3f", lstats.finalSeconds) << "\n";
      }
      if (words) {
        const expr::InitialWords::Stats &ws = words->stats();
        os << "words.candidates: " << ws.candidates << "\n";
        os << "words.eligible: " << ws.eligible << "\n";
        os << "words.chosen: " << ws.chosen << "\n";
        os << "words.translated: " << ws.translated << "\n";
        os << "words.word-reads: " << ws.wordReads << "\n";
        os << "words.aligned-proved: " << ws.alignedProved << "\n";
        os << "words.aligned-by-annotation: " << ws.alignedByAnnotation << "\n";
        os << "words.aligned-partial: " << ws.alignedPartial << "\n";
        os << "words.masked-reads: " << ws.maskedReads << "\n";
        os << "words.masked-parts: " << ws.maskedParts << "\n";
        for (const expr::InitialWords::Census &c : words->census()) {
          os << "words.array: name=" << *expr::op::bind::fname(c.array->first())
             << " chosen=" << (c.chosen ? 1 : 0) << " selects=" << c.selects
             << " wselects=" << c.wselects
             << " aligned-words=" << c.alignedWords
             << " other-bytes=" << c.otherBytes;
          if (!c.chosen)
            os << " reason=" << (c.reason.empty() ? "-" : c.reason);
          os << "\n";
        }
      }
      {
        const expr::Ackermannizer::Stats &as = ack.stats();
        os << "vc.ackermann: " << (ackOn ? "on" : "off") << "\n";
        if (ackOn) {
          os << "vc.ackermann-arrays: " << as.lowered << "\n";
          os << "vc.ackermann-kept: " << as.kept << "\n";
          os << "vc.ackermann-reads: " << as.reads << "\n";
          os << "vc.ackermann-pairs: " << as.pairs << "\n";
          os << "vc.ackermann-numeral-pairs: " << as.numeralPairs << "\n";
          os << "vc.ackermann-offset-pairs: " << as.offsetPairs << "\n";
          os << "vc.ackermann-residue-pairs: " << as.residuePairs << "\n";
          os << "vc.ackermann-leaf-pairs: " << as.leafPairs << "\n";
          if (strideOn)
            os << "vc.ackermann-stride-pairs: " << as.stridePairs << "\n"
               << "vc.ackermann-stride-a-inb-pairs: " << as.strideAInbPairs
               << "\n"
               << "vc.ackermann-stride-not-eligible: " << as.strideNotEligible
               << "\n"
               << "vc.ackermann-stride-other-base: " << as.strideOtherBase
               << "\n"
               << "vc.ackermann-stride-divides: " << as.strideDivides << "\n";
          else if (ackStride)
            os << "vc.ackermann-stride: fallback\n";
          os << "vc.ackermann-constraints: " << as.constraints << "\n";
        }
      }
      if (xlw) {
        const auto &ts = xtr->stats();
        const auto &ls = xlw->stats();
        os << "xrt.mode: " << xReadTable << "\n";
        os << "xrt.reads: " << ts.reads << "\n";
        os << "xrt.read-leaves: " << ts.leafSum << " max=" << ts.leafMax
           << "\n";
        os << "xrt.over-cap: " << xtr->overCap() << "\n";
        os << "xrt.r-calls: " << ts.rCalls << " memo=" << ts.rMemo << "\n";
        os << "xrt.stores: " << ts.stores << " leaf-pairs=" << ts.storePairs
           << "\n";
        os << "xrt.memops: " << ts.memops << " leaf-pairs=" << ts.memopPairs
           << " src-leaves=" << ts.srcLeaves << " src-unknown=" << ts.srcUnknown
           << "\n";
        os << "xrt.leaf-pairs: same-object=" << ts.samePairs
           << " unknown=" << ts.unknownPairs << " decided=" << ts.decided
           << " other-memory=" << ts.other << "\n";
        std::map<std::string, uint64_t> dk;
        for (const auto &d : xtr->deads())
          dk[d.kind]++;
        os << "xrt.dead: " << xtr->deads().size();
        for (auto &[k, n] : dk)
          os << " " << k << "=" << n;
        os << "\n";
        os << "xrt.records: " << ls.records << " unknown=" << ls.unknown
           << " unreached=" << ls.unreached << " arrays=" << ls.arrays << "\n";
        for (auto &[b, n] : ls.perBucket)
          os << "xrt.bucket: " << b << " " << n << "\n";
        os << "xrt.pairs: " << ls.pairs << " exact-drop=" << ls.exactDrop
           << " guard-drop=" << ls.guardDrop
           << " disjoint-drop=" << ls.disjointDrop
           << " solver-array=" << ls.arrayPairs << " mux=" << ls.muxPairs
           << " constraints=" << ls.constraints
           << " (offset=" << ls.constraintsOff
           << " address=" << ls.constraintsAddr << ")\n";
        os << "xrt.mux: objects=" << ls.muxObjects << " cells=" << ls.muxCells
           << " records=" << ls.muxRecords << "\n";
        os << "xrt.eq-widths:";
        for (auto &[w, n] : ls.eqWidths)
          os << " " << w << "x" << n;
        os << "\n";
        if (dagStats)
          os << "xrt.dag: std-boundary=" << xrtDagStd
             << " translated=" << xrtDagTranslated << "\n";
      }
      if (memsetBounds)
        os << "assume.memset-bound: count=" << memsetBounds
           << " bound=" << memsetBoundK << "\n";
      // A-INB, also without --seam: the lowering dropped memhavocs that
      // are no-ops only if no access lands in their object from outside
      if (dropFreshInb)
        os << "assume.a-inb: drop-fresh-memhavoc=" << dropFreshInb << "\n";
      // placement makes null address 0 (the segment "null"): every VC of a
      // lowered program identifies null with address 0 (a fact of the
      // target: no check discharges it)
      if (lower)
        os << "assume.addr0-is-null: vc null-uses=" << nullUses << "\n";
      // the open decision memhavoc(null) = no-op, in the VC: the memhavocs
      // whose pointer may be null run only when it is not
      if (nullGuards)
        os << "assume.memhavoc-null-noop: vc sites=" << nullGuards << "\n";
      if (sem.seam()) {
        // the assumption the answer depends on, always visible
        if (inb)
          os << "seam.inb: assumed accesses=" << sem.inbAccessCount()
             << " conditions=" << sem.inbConditionCount() << "\n";
        else {
          os << "seam.inb: off" << note;
          // the lowering's drops rest on A-INB all the same
          if (dropFreshInb)
            os << " (lowering: drop-fresh-memhavoc=" << dropFreshInb
               << " rests on A-INB)";
          os << "\n";
        }
      }
      for (const auto &[k, v] : fromBitsLeft)
        os << "seam.ptr.from-bits-left." << k << ": " << v << "\n";
      for (const auto &[k, v] : aliasOpen)
        os << "seam.alias.open." << k << ": " << v << "\n";
      for (const auto &[k, v] : aliasOpenPairs)
        os << "seam.alias.open-pairs." << k << ": " << v << "\n";
      for (const auto &[k, v] : memOpOpen)
        os << "seam.alias.open-memop." << k << ": " << v << "\n";
      if (sameObjectReduced)
        os << "seam.alias.same-object-reduced: " << sameObjectReduced << "\n";
      if (seamStats && sem.seamStats()) {
        os << "seam.rewrite-time: "
           << llvm::format("%.3f", sem.seamRewriteSeconds()) << "\n";
        os << "seam.simplify-time: "
           << llvm::format("%.3f", sem.simplifySeconds()) << "\n";
        for (auto &[name, sec] : phases)
          os << "vc." << name << "-time: " << llvm::format("%.3f", sec) << "\n";
        os << "vc.exec-time: " << llvm::format("%.3f", vcgen.execSeconds())
           << "\n";
        os << "vc.join-time: " << llvm::format("%.3f", vcgen.joinSeconds())
           << "\n";
        sem.seamStats()->print(os, "seam.");
      }
    };
    auto printSeamStats = [&](raw_ostream &os) {
      std::string text;
      raw_string_ostream ts(text);
      printSeamStatsText(ts);
      os << ts.str();
      if (!brunchStats)
        return;
      // every `key: value` line with a one-token value, plus the parts of
      // seam.inb
      for (StringRef rest = text; !rest.empty();) {
        auto [line, tail] = rest.split('\n');
        rest = tail;
        auto [key, value] = line.split(": ");
        if (key == "seam.inb" || key.starts_with("assume.")) {
          // a leading state word (assumed / off / vc), then the k=v
          // tokens: `<key> <state>`, `<key>-<k> <v>` (notes in
          // parentheses are left out)
          SmallVector<StringRef> words;
          value.split(words, ' ', -1, /*KeepEmpty=*/false);
          bool inNote = false; // a parenthesised note: left out whole
          for (auto [i, w] : llvm::enumerate(words)) {
            if (inNote || w.starts_with("(")) {
              inNote = !w.ends_with(")");
              continue;
            }
            auto [k, v] = w.split('=');
            if (!v.empty())
              brunchStat(os, key + "-" + k, v);
            else if (i == 0)
              brunchStat(os, key, w);
          }
        } else if (!value.empty() && !value.contains(' '))
          brunchStat(os, key, value);
      }
    };
    // --eq-audit (tools/sea-vc/EqAudit.h): sampled undecided equalities,
    // each by the lowest level of facts that decides it. z3, with a timeout.
    auto runEqAudit = [&]() {
      if (!auditOn)
        return;
      auto t0 = std::chrono::steady_clock::now();
      auto z = std::make_unique<sea::solver::z3_solver_impl>(efac);
      if (eqAuditTimeout) {
        sea::ZParams<sea::EZ3> params(z->get_context());
        params.set("timeout", eqAuditTimeout * 1000);
        z->get_solver().set(params);
      }
      const expr::BaseRegistry &bases = vcgen.baseRegistry();
      sea::EqAudit audit(*z, bases);
      expr::Expr layout = sem.layoutFacts();
      expr::SeaMRewriter *rw = sem.seamRewriter();
      auto pick = [&](size_t n, size_t k) {
        size_t m = std::min<size_t>(auditSample, n);
        return m == n ? k : k * n / m;
      };
      // Ackermann pairs
      const expr::Ackermannizer *aa =
          auditAck ? auditAck.get() : (ackOn ? &ack : nullptr);
      if (aa) {
        const auto &ps = aa->constrainedPairs();
        audit.noteTotal("ack", ps.size());
        size_t m = std::min<size_t>(auditSample, ps.size());
        for (size_t k = 0; k < m; ++k) {
          size_t at = pick(ps.size(), k);
          const auto &p = ps[at];
          const auto &rs = aa->arrays()[p.array].reads;
          expr::Expr a = rs[p.k].index, b = rs[p.l].index;
          bool v1 = false, v2 = false;
          expr::Expr inb =
              rw ? expr::mk<expr::AND>(rw->rules().inBounds(a, 1, v1),
                                       rw->rules().inBounds(b, 1, v2))
                 : expr::Expr();
          // the conditions hold markers (ADDR): to the solver's terms
          if (inb) {
            std::string why;
            inb = sem.solverTerm(inb, why);
          }
          audit.audit("ack", {a, b, layout, inb, uint64_t(at)});
        }
      }
      // reads the rules left undecided (the trail)
      std::vector<const expr::RewriteTrail::Firing *> stores, memops;
      for (const expr::RewriteTrail::Firing &f : trail.kept()) {
        std::string r = trail.rulesOf(f);
        if (r == "row.store")
          stores.push_back(&f);
        else if (r == "row.mcopy" || r == "row.mset" || r == "row.mhavoc")
          memops.push_back(&f);
      }
      auto run = [&](const char *src,
                     const std::vector<const expr::RewriteTrail::Firing *> &fs,
                     bool memop) {
        audit.noteTotal(src, fs.size());
        size_t m = std::min<size_t>(auditSample, fs.size());
        for (size_t k = 0; k < m; ++k) {
          const expr::RewriteTrail::Firing &f = *fs[pick(fs.size(), k)];
          std::string why;
          expr::Expr inb = sem.accessPremise(trail, f, why);
          if (inb)
            inb = sem.solverTerm(inb, why);
          const expr::Expr arr = f.lhs->arg(0), j = f.lhs->arg(1);
          expr::Expr a, b;
          if (memop) {
            a = expr::memOpIn(expr::mk<expr::BSUB>(j, expr::memOpDst(arr)),
                              expr::memOpLen(arr));
          } else {
            a = arr->arg(1);
            b = j;
          }
          a = a ? sem.solverTerm(a, why) : a;
          if (b)
            b = sem.solverTerm(b, why);
          if (!a || (!memop && !b))
            continue;
          audit.audit(src, {a, b, layout, inb, f.id});
        }
      };
      run("row-store", stores, false);
      run("row-memop", memops, true);
      raw_string_ostream os(auditReport);
      audit.report(os, eqAuditShapes);
      // the pairs whose constraint the stride rule dropped are not audited
      // An audit under P3 is future work.
      if (ackOn && strideOn)
        os << "eq-audit.ack-stride-dropped: " << ack.stats().stridePairs
           << " (not audited)\n";
      if (brunchStats)
        audit.stats([&](const std::string &k, uint64_t v) {
          os << "BRUNCH_STAT " << k << " " << v << "\n";
        });
      os << "eq-audit-time: "
         << llvm::format("%.3f", std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count())
         << "\n";
    };
    // every recorded firing (or a sample) as a validity query,
    // `premise => lhs = rhs`, on the solver of the run. Runs after the
    // counters are printed (the store-map expansion counts smap.expanded).
    auto runSeamCheck = [&]() {
      if (!checkOn)
        return;
      raw_string_ostream os(checkReport);
      const auto &fs = trail.kept();
      size_t n = fs.size();
      size_t m = (checkSample && checkSample < n) ? checkSample : n;
      uint64_t valid = 0, invalid = 0, unknown = 0, unexpr = 0;
      unsigned ms = seamCheckTimeout * 1000;
      // a fresh solver per query. Hypothesis (measured, not read in z3's
      // source): z3 decides its auto-configuration at the first push, before
      // anything is asserted, and keeps relevancy 2 for every later query on
      // that solver (leaf-split obligations: 11 s
      // instead of 0.06 s). The non-incremental path (no push) runs the
      // qfaufbv preprocessing chain, which blows up on row.store
      // obligations (24.8 s against 0.33 s pushed). So z3 gets relevancy 0
      // and one push, never popped: the solver is dropped after the query.
      auto mkCheckSolver = [&]() -> std::unique_ptr<sea::solver::Solver> {
        if (backend == Backend::Z3) {
          auto z = std::make_unique<sea::solver::z3_solver_impl>(efac);
          sea::ZParams<sea::EZ3> params(z->get_context());
          if (ms)
            params.set("timeout", ms);
          params.set("relevancy", 0u);
          z->get_solver().set(params);
          z->push();
          return z;
        }
        if (backend == Backend::Bitwuzla) {
#ifdef WITH_BITWUZLA
          sea::solver::bitwuzla_solver_impl::solver_options o;
          if (ms)
            o["time-limit-per"] = std::to_string(ms);
          return std::make_unique<sea::solver::bitwuzla_solver_impl>(
              efac, nullptr, o);
#else
          return nullptr;
#endif
        }
        return makeSolver();
      };
      if (backend != Backend::Z3 && backend != Backend::Bitwuzla && ms)
        sea::diag::warn("seam.check-no-timeout",
                        "--seam-check-timeout is not supported for Yices: "
                        "its checks run without a time limit");
      if (!mkCheckSolver())
        return;
      for (size_t k = 0; k < m; ++k) {
        const expr::RewriteTrail::Firing &f = fs[m == n ? k : k * n / m];
        const expr::RewriteTrail::Context &c = trail.context(f.ctx);
        std::string why;
        expr::Expr prem = sem.firingPremise(trail, f, why);
        if (!prem) {
          ++unexpr;
          sea::diag::warn("seam.check-unexpressible",
                          "--seam-check: the premise of firing " + Twine(f.id) +
                              " (" + trail.rulesOf(f) + ") cannot be stated: " +
                              why + "; not counted as valid");
          os << "seam-check-unexpressible: id=" << f.id
             << " rules=" << trail.rulesOf(f) << " why=" << why << "\n";
          continue;
        }
        // the premise goes through the same boundary as the two sides: a
        // range length or an address in it may read a memory-operation term
        // (a loaded length, --seam-row-expand=0)
        prem = sem.solverTerm(prem, why);
        if (!prem) {
          ++unexpr;
          sea::diag::warn("seam.check-unexpressible",
                          "--seam-check: the premise of firing " + Twine(f.id) +
                              " (" + trail.rulesOf(f) +
                              ") has no solver form: " + why +
                              "; not counted as valid");
          os << "seam-check-unexpressible: id=" << f.id
             << " rules=" << trail.rulesOf(f) << " why=" << why << "\n";
          continue;
        }
        // a premise no assignment satisfies would make every firing valid
        if (!expr::isOpX<expr::TRUE>(prem)) {
          std::unique_ptr<sea::solver::Solver> cs = mkCheckSolver();
          if (!cs)
            return;
          sea::solver::SolverResult pr =
              cs->add(prem) ? cs->check() : sea::solver::SolverResult::UNKNOWN;
          if (pr == sea::solver::SolverResult::UNSAT) {
            ++unexpr;
            sea::diag::warn("seam.check-unexpressible",
                            "--seam-check: the premise of firing " +
                                Twine(f.id) + " (" + trail.rulesOf(f) +
                                ") is unsatisfiable; not counted as valid");
            os << "seam-check-unexpressible: id=" << f.id
               << " rules=" << trail.rulesOf(f) << " why=vacuous premise\n";
            continue;
          }
        }
        // both sides through the boundary (semantics only); a side it
        // refuses (a memory-operation term not under a read) is
        // unexpressible, never valid
        expr::Expr diff = sem.firingObligation(f, why);
        if (!diff) {
          ++unexpr;
          sea::diag::warn(
              "seam.check-unexpressible",
              "--seam-check: firing " + Twine(f.id) + " (" + trail.rulesOf(f) +
                  ") has no solver form: " + why + "; not counted as valid");
          os << "seam-check-unexpressible: id=" << f.id
             << " rules=" << trail.rulesOf(f) << " why=" << why << "\n";
          continue;
        }
        expr::Expr q = expr::mk<expr::AND>(prem, diff);
        std::unique_ptr<sea::solver::Solver> cs = mkCheckSolver();
        if (!cs)
          return;
        sea::solver::SolverResult res =
            cs->add(q) ? cs->check() : sea::solver::SolverResult::UNKNOWN;
        if (res == sea::solver::SolverResult::UNSAT) {
          ++valid;
          continue;
        }
        if (res != sea::solver::SolverResult::SAT) {
          ++unknown;
          os << "seam-check-unknown: id=" << f.id
             << " rules=" << trail.rulesOf(f) << "\n";
          continue;
        }
        ++invalid;
        SmallVector<const char *, 2> pn;
        expr::RewriteTrail::premiseNames(f.premises, pn);
        os << "seam-check-invalid: id=" << f.id << " rules=" << trail.rulesOf(f)
           << " premises=";
        if (pn.empty())
          os << "none";
        for (size_t i = 0; i < pn.size(); ++i)
          os << (i ? "," : "") << pn[i];
        os << " op=" << c.kind << " loc=" << (c.loc.empty() ? "-" : c.loc)
           << "\n";
        os << "seam-check-lhs: " << expr::RewriteTrail::text(f.lhs) << "\n";
        os << "seam-check-rhs: " << expr::RewriteTrail::text(f.rhs) << "\n";
        // the query as SMT-LIB (z3's printer, whatever the solver)
        sea::solver::z3_solver_impl printer(efac);
        printer.add(q);
        std::string smt;
        raw_string_ostream ss(smt);
        printer.to_smt_lib(ss);
        for (StringRef rest = ss.str(); !rest.empty();) {
          auto [line, tail] = rest.split('\n');
          rest = tail;
          os << "seam-check-smt: " << line << "\n";
        }
      }
      os << "seam-check: firings=" << n
         << " checked=" << (valid + invalid + unknown) << " valid=" << valid
         << " invalid=" << invalid << " unknown=" << unknown
         << " unexpressible=" << unexpr << "\n";
      checkInvalid += invalid;
    };
    // EXPERIMENT: premise checks of the read table (sampled): under A-INB
    // of every access (path => in bounds), the layout and the side
    // conditions of the lowered query, an omitted pair is never relevant and
    // equal, a dead branch never relevant, a mux read never out of range
    auto runXrtCheck = [&]() {
      if (!xlw || !xReadTableCheck)
        return;
      raw_ostream &os = statOut();
      expr::SeaMRewriter *rw = sem.seamRewriter();
      xtr->freeze();
      std::string why;
      auto tl = [&](expr::Expr e) -> expr::Expr {
        e = rw->expandStoreMaps(e);
        e = xtr->translate(e, why);
        return e ? xlw->lowerTerm(e) : e;
      };
      expr::ExprVector pv;
      for (const expr::Expr &v : sem.inbViolationTerms())
        if (xReadTableCheckPremise.find('i') != std::string::npos)
          pv.push_back(expr::op::boolop::lneg(v));
      expr::Expr tt = expr::mk<expr::TRUE>(efac);
      expr::Expr prem = tl(expr::mknary<expr::AND>(tt, pv));
      // the side conditions without the sea.assume facts (C): the CFG, the
      // phis, the layout equations; the assumes carry the negated property
      // (SeaHorn's encoding: assume(!prop); assert(false)), so with them
      // (letter a) the premise may be the whole problem
      expr::ExprVector cfg;
      {
        const auto &ap = sem.assumeSidePositions();
        std::set<size_t> skip(ap.begin(), ap.end());
        for (size_t x = 0; x < side.size(); ++x)
          if (xReadTableCheckPremise.find('a') != std::string::npos ||
              !skip.count(x))
            cfg.push_back(side[x]);
      }
      expr::Expr sides = tl(expr::mknary<expr::AND>(tt, cfg));
      if (!prem || !sides) {
        os << "xrt.check: unexpressible (" << why << ")\n";
        return;
      }
      expr::ExprVector lay;
      for (const expr::ObjectBase &o : sem.baseRegistry().objects())
        lay.push_back(expr::mk<expr::EQ>(
            o.constant,
            expr::op::bv::bvnum(expr::toMpz(o.base),
                                sem.seamRewriter()->rules().widthOf(o.constant),
                                efac)));
      expr::Expr layout = expr::mknary<expr::AND>(tt, lay);
      std::unique_ptr<sea::solver::Solver> cs;
      if (backend == Backend::Z3) {
        auto z = std::make_unique<sea::solver::z3_solver_impl>(efac);
        sea::ZParams<sea::EZ3> params(z->get_context());
        params.set("timeout", unsigned(xReadTableCheckTimeout * 1000));
        z->get_solver().set(params);
        cs = std::move(z);
      } else {
        cs = makeSolver();
      }
      if (!cs)
        return;
      auto t0 = std::chrono::steady_clock::now();
      if (xReadTableCheckPremise.find('l') == std::string::npos)
        layout = tt;
      if (xReadTableCheckPremise.find('s') == std::string::npos)
        sides = tt;
      // S: the side conditions through the standard boundary (the original
      // semantics, no table): for the context check only
      if (xReadTableCheckPremise.find('S') != std::string::npos)
        sides = sem.solverTerm(expr::mknary<expr::AND>(tt, side));
      os << "xrt.check-premise: " << xReadTableCheckPremise << "\n";
      cs->add(expr::mk<expr::AND>(sides, expr::mk<expr::AND>(prem, layout)));
      auto ctxRes = cs->check();
      os << "xrt.check-context: "
         << (ctxRes == sea::solver::SolverResult::SAT     ? "sat"
             : ctxRes == sea::solver::SolverResult::UNSAT ? "unsat"
                                                          : "unknown")
         << "\n";
      auto one = [&](expr::Expr ob, uint64_t &u, uint64_t &s, uint64_t &k) {
        cs->push();
        auto r = cs->add(ob) ? cs->check() : sea::solver::SolverResult::UNKNOWN;
        cs->pop();
        if (r == sea::solver::SolverResult::UNSAT)
          ++u;
        else if (r == sea::solver::SolverResult::SAT)
          ++s;
        else
          ++k;
        return r;
      };
      const auto &recs = xtr->records();
      const auto &rel = xlw->relevance();
      {
        const auto &om = xlw->omitted();
        size_t n = om.size(), m = std::min<size_t>(n, xReadTableCheck);
        uint64_t u = 0, s = 0, k = 0;
        for (size_t x = 0; x < m; ++x) {
          auto [a, b] = om[m == n ? x : x * n / m];
          const auto &A = xlw->low(a), &B = xlw->low(b);
          auto r =
              one(expr::mk<expr::AND>(
                      A.rel, expr::mk<expr::AND>(
                                 B.rel, expr::mk<expr::EQ>(A.addr, B.addr))),
                  u, s, k);
          if (r == sea::solver::SolverResult::SAT)
            os << "xrt.check-omitted-sat: " << a << " " << b << "\n";
        }
        os << "xrt.check-omitted: total=" << n << " checked=" << m
           << " unsat=" << u << " sat=" << s << " unknown=" << k << "\n";
        // the same pairs without the address equality: how many are
        // exclusive by relevance alone (semantically, not syntactically)
        uint64_t u2 = 0, s2 = 0, k2 = 0;
        for (size_t x = 0; x < m; ++x) {
          auto [a, b] = om[m == n ? x : x * n / m];
          one(expr::mk<expr::AND>(xlw->low(a).rel, xlw->low(b).rel), u2, s2,
              k2);
        }
        os << "xrt.check-omitted-relevance-only: unsat=" << u2 << " sat=" << s2
           << " unknown=" << k2 << "\n";
      }
      {
        const auto &ds = xtr->deads();
        size_t n = ds.size(), m = std::min<size_t>(n, xReadTableCheck);
        uint64_t u = 0, s = 0, k = 0, unreached = 0;
        for (size_t x = 0; x < m; ++x) {
          const auto &d = ds[m == n ? x : x * n / m];
          expr::Expr r = rel.rel(d.sym.get());
          if (!r) {
            ++unreached;
            continue;
          }
          if (one(xlw->lowerTerm(r), u, s, k) == sea::solver::SolverResult::SAT)
            os << "xrt.check-dead-sat: " << *d.sym << " " << d.kind << "\n";
        }
        os << "xrt.check-dead: total=" << n << " checked=" << m
           << " unsat=" << u << " sat=" << s << " unknown=" << k
           << " unreached=" << unreached << "\n";
      }
      {
        const auto &mr = xlw->muxRecords();
        size_t n = mr.size(), m = std::min<size_t>(n, xReadTableCheck);
        uint64_t u = 0, s = 0, k = 0;
        for (size_t x = 0; x < m; ++x) {
          size_t a = mr[m == n ? x : x * n / m];
          const auto &A = xlw->low(a);
          unsigned w = sem.seamRewriter()->rules().widthOf(A.off);
          one(expr::mk<expr::AND>(
                  A.rel, expr::mk<expr::NEG>(expr::mk<expr::BULT>(
                             A.off, expr::op::bv::bvnum(
                                        expr::mpz_class((unsigned long)A.size),
                                        w, efac)))),
              u, s, k);
        }
        os << "xrt.check-mux-range: total=" << n << " checked=" << m
           << " unsat=" << u << " sat=" << s << " unknown=" << k << "\n";
      }
      {
        // each lowered record: can it be relevant at all (under the premise)?
        uint64_t u = 0, s = 0, k = 0;
        std::map<std::string, std::pair<uint64_t, uint64_t>> perBucket;
        for (size_t x = 0; x < recs.size(); ++x) {
          if (!xlw->lowered(x))
            continue;
          auto r = one(xlw->low(x).rel, u, s, k);
          std::string b = "unknown";
          if (recs[x].leaf.base) {
            std::ostringstream bs;
            bs << *expr::op::bind::fname(recs[x].leaf.base->first());
            b = bs.str();
          }
          auto &pb = perBucket[b];
          ++(r == sea::solver::SolverResult::UNSAT ? pb.first : pb.second);
        }
        os << "xrt.check-record-relevant: unsat=" << u << " sat=" << s
           << " unknown=" << k;
        for (auto &[b, c] : perBucket)
          os << " " << b << "=" << c.second << "/" << (c.first + c.second);
        os << "\n";
      }
      os << "xrt.check-time: "
         << llvm::format("%.3f", std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count())
         << "\n";
    };
    if (smt2) {
      // the block ids of each guard (kBlockIdsAttr), as SMT-LIB comments:
      // `; sea.block-ids <guard> <id>,<id>...` (utils/query-diff.py's
      // Skolem hints); none for a program without them
      auto blockIdLines = [&](raw_ostream &os) {
        for (Block &bb : program.getBody()) {
          ArrayAttr ids = sea::blockIdsOf(&bb);
          expr::Expr g = ids ? vcgen.guard(&bb) : expr::Expr();
          if (!g)
            continue;
          std::ostringstream gs;
          gs << *g;
          os << "; sea.block-ids " << gs.str() << " ";
          llvm::interleave(
              ids, os,
              [&](Attribute a) { os << cast<StringAttr>(a).getValue(); }, ",");
          os << "\n";
        }
      };
      if (gOut->jsonl()) {
        // the query is one record
        sea::out::Record rec("smt2");
        raw_string_ostream qs(rec.text);
        solver->to_smt_lib(qs);
        blockIdLines(qs);
        if (StringRef(rec.text).ends_with("\n"))
          rec.text.pop_back();
        gOut->emit(rec);
      } else {
        solver->to_smt_lib(outs());
        blockIdLines(outs());
      }
      gOut->flush();
      statOut() << "vc.ptr-mode: " << (addressMode ? "address" : "opaque")
                << "\nvc.int-to-ptr: " << intToPtr << "\n"
                << "vc.ptr-from-int-opaque: " << sem.ptrFromIntOpaque() << "\n";
      if (sinkAssumes)
        statOut() << "vc.sink-assumes: " << sunkAssumes << "\n";
      statOut() << lowerStatLines;
      foldLines(statOut());
      companionLine(statOut());
      if (vcgen.joinCases())
        vcgen.joinCases()->print(statOut());
      printSeamStats(statOut());
      runSeamCheck();
      runEqAudit();
      runXrtCheck();
      return 0;
    }

    auto emitResult = [&](StringRef v) {
      // a part's answer (the verdict of a split comes after its parts)
      if (splitOn) {
        splitPart.result = v.str();
        vout() << "split.part-result: " << v << "\n";
        return;
      }
      sea::out::Record rec("result");
      rec.str("value", v);
      rec.text = ("result: " + v).str();
      gOut->emit(rec);
    };
    {
      sea::out::Record rec("program");
      rec.str("name", program.getSymName());
      rec.text = ("program: @" + program.getSymName()).str();
      gOut->emit(rec);
    }
    vout() << "vc.ptr-mode: " << (addressMode ? "address" : "opaque")
           << "\nvc.int-to-ptr: " << intToPtr
           << "\nvc.ptr-from-int-opaque: " << sem.ptrFromIntOpaque() << "\n";
    if (sinkAssumes)
      vout() << "vc.sink-assumes: " << sunkAssumes << "\n";
    vout() << lowerStatLines;
    foldLines(vout());
    companionLine(vout());
    if (vcgen.joinCases())
      vcgen.joinCases()->print(vout());
    brunchStat(vout(), "vc.ptr-mode", addressMode ? "address" : "opaque");
    brunchStat(vout(), "vc.int-to-ptr", llvm::Twine(intToPtr));
    brunchStat(vout(), "vc.ptr-from-int-opaque",
               llvm::Twine(sem.ptrFromIntOpaque()));
    // the base registry (SeaM), in allocation order
    for (const expr::ObjectBase &o : vcgen.baseRegistry().objects()) {
      sea::out::Record rec("object");
      // id: the object's name in every record (print, memread); symbol:
      // the VC constant of its base
      rec.str("id", o.name)
          .str("symbol", "obj." + o.name)
          .num("order", o.order)
          .str("objkind", o.kind)
          .integer("base", o.base)
          .integer("size", o.size)
          .num("align", o.align);
      raw_string_ostream os(rec.text);
      os << "object: name=obj." << o.name << " order=" << o.order
         << " kind=" << o.kind << " base=";
      o.base.print(os, /*isSigned=*/false);
      os << " size=";
      o.size.print(os, /*isSigned=*/false);
      os << " align=" << o.align;
      gOut->emit(rec);
    }
    printSeamStats(vout());
    if (splitOn) {
      splitPart.vcSeconds = vcSeconds;
      auto get = [](const std::map<std::string, uint64_t> &m, const char *k) {
        auto it = m.find(k);
        return it == m.end() ? uint64_t(0) : it->second;
      };
      splitPart.aliasOpen = get(aliasOpen, "all");
      splitPart.aliasOpenPairs = get(aliasOpenPairs, "all");
      splitPart.memopOpen = get(memOpOpen, "all");
      splitPart.rows = 0;
      if (const expr::RewriteStats *st = sem.seamStats())
        for (const auto &kv : st->rules)
          if (kv.first().starts_with("row."))
            splitPart.rows += kv.second;
      splitPart.ackOn = ackOn;
      splitPart.ackReads = ack.stats().reads;
      splitPart.ackPairs = ack.stats().pairs;
      splitPart.ackConstraints = ack.stats().constraints;
      splitPart.dag = expr::dagSize(query);
    }
    if (vcgenOnly) {
      // the VC is generated and checked; no solver of the query (and none
      // of the checks on the run's solver: --seam-check,
      // --x-read-table-check). --eq-audit runs: its checks are its own z3
      // instance with their own timeout, on terms of the VC only
      runEqAudit();
      if (checkOn || xReadTableCheck)
        sea::diag::warn("vc.vcgen-only-check-skipped",
                        "--seam-check and --x-read-table-check do not run: "
                        "--vcgen-only skips them");
      emitResult("none");
      brunchStat(vout(), "result", "none");
      return 0;
    }
    runSeamCheck();
    runEqAudit();
    runXrtCheck();
    if (reach) {
      // the reach queries (lib/VC/Reach.cpp); the solver above holds the
      // query
      auto limitSolver = [&](sea::solver::Solver &s) {
        if (!reachRlimit)
          return;
        auto &z = static_cast<sea::solver::z3_solver_impl &>(s);
        sea::ZParams<sea::EZ3> params(z.get_context());
        params.set("rlimit", unsigned(reachRlimit));
        if (solveTimeout)
          params.set("timeout", unsigned(solveTimeout * 1000));
        z.get_solver().set(params);
      };
      sea::ReachHooks hooks;
      bool first = true;
      hooks.makeSolver = [&]() -> std::unique_ptr<sea::solver::Solver> {
        std::unique_ptr<sea::solver::Solver> s;
        if (first) {
          first = false;
          s = std::move(solver);
        } else {
          s = makeSolver(/*limits=*/true);
          if (s && !s->add(query))
            s = nullptr;
        }
        if (s)
          limitSolver(*s);
        return s;
      };
      // the model of the replay: as for a counterexample (below)
      hooks.withModel =
          [&](sea::solver::Solver &s,
              llvm::function_ref<void(sea::solver::Model &)> use) -> int {
        auto inner = s.get_model();
        // as for a counterexample: a model that puts two reads whose
        // constraint the stride rule dropped at one index breaks a GEP
        // promise; every query is run again without the rule
        if (ackOn && strideOn && ack.stats().stridePairs) {
          expr::AckermannModel probe(*inner, ack);
          for (size_t a = 0; a < ack.arrays().size(); ++a)
            (void)probe.function(a);
          if (probe.strideBroken()) {
            sea::diag::warn("vc.ack-stride-promise-broken",
                            "the model breaks a GEP promise the stride rule "
                            "relied on (a wrapping offset: UB in LLVM); "
                            "running again without --ack-stride");
            return kRetryWithoutStride;
          }
        }
        expr::ReadCellsModel cells(*inner, query);
        std::unique_ptr<expr::AckermannModel> am;
        if (ackOn)
          am = std::make_unique<expr::AckermannModel>(cells, ack);
        std::unique_ptr<expr::xrt::TableModel> xm;
        if (xlw)
          xm = std::make_unique<expr::xrt::TableModel>(cells, *xtr, *xlw);
        sea::solver::Model &solved =
            am   ? static_cast<sea::solver::Model &>(*am)
            : xm ? static_cast<sea::solver::Model &>(*xm)
                 : cells;
        std::unique_ptr<expr::WordsModel> wm;
        if (words && words->stats().chosen)
          wm = std::make_unique<expr::WordsModel>(solved, *words);
        use(wm ? static_cast<sea::solver::Model &>(*wm) : solved);
        if (am && am->inconsistent()) {
          sea::diag::error("vc.ackermann-inconsistent",
                           "the model of the lowered query is not a function "
                           "on an array: a bug in the Ackermann lowering");
          return 2;
        }
        return 0;
      };
      // z3 says `canceled` for both limits: the rlimit unless the check took
      // the time of --timeout
      if (backend == Backend::Z3)
        hooks.reasonUnknown = [](sea::solver::Solver &s,
                                 double sec) -> std::string {
          std::string r = s.reason_unknown();
          if (r == "timeout" && reachRlimit &&
              !(solveTimeout && sec >= solveTimeout))
            return "rlimit";
          return r;
        };
      sea::ReachOptions ropts;
      // concrete runs first leave mostly unreachable targets, which a
      // fresh solver per target proves faster; =true still applies
      const bool incremental = reachIncremental.getNumOccurrences() ||
                                       (reachCorpus.empty() && !reachFuzz)
                                   ? bool(reachIncremental)
                                   : false;
      ropts.incremental = incremental;
      ropts.modelRuns = reachModelRuns;
      ropts.order = reachOrder;
      ropts.witnessDir = reachWitness;
      ropts.programPath = inputFilename;
      ropts.lowered = lower;
      ropts.plantMismatch = reachPlant == "mismatch";
      ropts.query = reachQuery;
      ropts.addr0IsNull = addr0IsNull;
      ropts.memhavocNullNoop = memhavocNull == MemhavocNull::Noop;
      if (inb && abstractModule)
        for (auto p : abstractModule->getOps<sea::ProgramOp>())
          if (p.getSymName() == program.getSymName())
            ropts.abstractProgram = p;
      vout() << "reach.config: kind="
             << (reachKind == sea::ReachKind::Checks ? "checks" : "blocks")
             << " solver=" << (incremental ? "incremental" : "separate")
             << " rlimit=" << reachRlimit << " splits=" << reachSplits
             << " model-runs=" << (reachModelRuns ? 1 : 0) << " order="
             << (reachOrder == sea::ReachOrder::Deepest ? "deepest"
                                                        : "program");
      if (!reachQuery)
        vout() << " query=0";
      if (reachRuns)
        vout() << " corpus=" << reachRuns->seedTrials << " fuzz=" << reachFuzz;
      vout() << "\n";
      Phase reachPhase("reach");
      int rc =
          sea::runReach(program, reachTargets, vcgen, query, sem, ctx, ropts,
                        hooks, *gOut, reachRuns ? &*reachRuns : nullptr);
      SEA_PHASE_END(reachPhase).kv("targets", reachTargets.size());
      return rc;
    }
    Phase solvePhase("solve");
    auto solveStart = std::chrono::steady_clock::now();
    sea::solver::SolverResult res = solver->check();
    const double solveSec = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - solveStart)
                                .count();
    SEA_LOG(logSolve, sea::log::Flow)
        .kv("backend", backend == Backend::Z3         ? "z3"
                       : backend == Backend::Bitwuzla ? "bitwuzla"
                                                      : "yices")
        .kv("result", res == sea::solver::SolverResult::SAT     ? "sat"
                      : res == sea::solver::SolverResult::UNSAT ? "unsat"
                                                                : "unknown")
        .kv("time", solveSec)
        .kv("nodes-made", efac.nodesMade());
    SEA_PHASE_END(solvePhase);
    splitPart.solveSeconds = solveSec;
    std::string solveSeconds =
        llvm::formatv("{0:f3}",
                      std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - solveStart)
                          .count())
            .str();
    vout() << "solve-time: " << solveSeconds << "\n";
    if (backend == Backend::Z3 && z3Tactic != "default")
      vout() << "solver.z3-tactic: " << z3Tactic << "\n";
    if (backend == Backend::Yices) {
      if (!yicesLogic.empty())
        vout() << "solver.yices-logic: " << yicesLogic << "\n";
      for (const std::string &kv : yicesConfig)
        vout() << "solver.yices-config: " << kv << "\n";
      for (const std::string &kv : yicesParams)
        vout() << "solver.yices-param: " << kv << "\n";
    }
    brunchStat(vout(), "solve-time", solveSeconds);
    if (res == sea::solver::SolverResult::UNSAT) {
      emitResult("unsat");
      if (sem.ptrIntChecking() && sem.companionChecked())
        vout() << "seam.ptrint: holds\n";
      brunchStat(vout(), "result", "unsat");
      return 0;
    }
    if (res != sea::solver::SolverResult::SAT) {
      emitResult("unknown");
      brunchStat(vout(), "result", "unknown");
      // why, when a limit of the run explains it: the backend's reason
      // (z3, Yices' watchdog); else the time spent reached --timeout; for
      // Bitwuzla (complete for the VC's logics) an unknown under
      // --mem-limit within the time is the memory limit
      std::string why = solver->reason_unknown();
      const bool timedOut =
          solveTimeout && (why == "timeout" || solveSec >= solveTimeout);
      const bool memHit = memLimit && !timedOut &&
                          (why == "memout" || backend == Backend::Bitwuzla);
      if (timedOut) {
        vout() << "solve-timeout: true\n";
        brunchStat(vout(), "solve-timeout", "true");
      }
      if (memHit) {
        vout() << "mem-limit: hit\n";
        brunchStat(vout(), "mem-limit", "hit");
        // the solver's own limit: in solve (the new-handler's note line)
        vout() << "mem-limit-phase: solve\n";
      }
      // where to go next: the split targets, when the normalized query
      // has open alias questions to attribute, or else Ackermann
      // constraints (--split-candidates attributes the read indices; not a
      // split run, whose parts say which case is hard)
      if (hintQuery) {
        uint64_t open = 0;
        std::unordered_set<expr::ENode *> seen;
        std::vector<expr::ENode *> stack{hintQuery.get()};
        while (!stack.empty()) {
          expr::ENode *n = stack.back();
          stack.pop_back();
          if (!seen.insert(n).second)
            continue;
          if (expr::isOpX<expr::IS_OVERLAP>(n))
            ++open;
          for (size_t i = 0; i < n->arity(); ++i)
            stack.push_back(n->arg(i));
        }
        const uint64_t ackConstraints = ackOn ? ack.stats().constraints : 0;
        if (open || ackConstraints) {
          sea::out::Record rec("hint");
          rec.str("topic", "split").num("open_aliases", open);
          rec.text = "hint: split: open-aliases=" + std::to_string(open);
          if (!open) {
            rec.num("ackermann_constraints", ackConstraints);
            rec.text +=
                " ackermann-constraints=" + std::to_string(ackConstraints);
          }
          rec.str("option", "--split-candidates");
          rec.text += "; rerun with --split-candidates to list split targets "
                      "(no solver)";
          gOut->emit(rec);
        }
      }
      gOut->flush();
      if (!timedOut && !memHit)
        sea::diag::warn("vc.unknown",
                        "the solver returned unknown" +
                            (why.empty() ? std::string() : " (" + why + ")"));
      return 3;
    }
    // the stride rule: a model that puts two reads whose constraint it
    // dropped at one index breaks a GEP promise (or a wide access's A-INB);
    // decided before any result is printed, the run is repeated without
    // the rule (the arrays are built eagerly: AckermannModel)
    if (ackOn && strideOn && ack.stats().stridePairs) {
      auto probeInner = solver->get_model();
      expr::AckermannModel probe(*probeInner, ack);
      for (size_t a = 0; a < ack.arrays().size(); ++a)
        (void)probe.function(a);
      if (probe.strideBroken()) {
        vout() << "vc.ackermann-stride-broken: " << probe.strideBroken() << "\n"
               << "vc.ackermann-stride-retry-solve-time: " << solveSeconds
               << "\n";
        brunchStat(vout(), "vc.ackermann-stride-retry-solve-time",
                   solveSeconds);
        sea::diag::warn("vc.ack-stride-promise-broken",
                        "the model breaks a GEP promise the stride rule "
                        "relied on (a wrapping offset: UB in LLVM); running "
                        "again without --ack-stride");
        return kRetryWithoutStride;
      }
    }
    if (sem.companionAssumed()) {
      sea::diag::warn("vc.ptrint-sat",
                      "sat under A-PTRINT (--ptr-companion=assume): the "
                      "counterexample may rest on the premise; running "
                      "again with --ptr-companion=exact");
      return kRetryExactCompanion;
    }
    emitResult("sat");
    brunchStat(vout(), "result", "sat");
    if (sem.ptrIntChecking() && sem.companionChecked())
      vout() << "seam.ptrint: not discharged\n";

    auto solverModel = solver->get_model();
    // the cells of the solver's arrays no constraint reads: defaults
    // (`..` in prints, docs/design/Print.md)
    expr::ReadCellsModel model(*solverModel, query);
    // with the lowering, the arrays are rebuilt from the model of the lowered
    // query: every evaluation (replay, validation) reads them through it
    std::unique_ptr<expr::AckermannModel> ackModel;
    if (ackOn)
      ackModel = std::make_unique<expr::AckermannModel>(model, ack);
    std::unique_ptr<expr::xrt::TableModel> xrtModel;
    if (xlw)
      xrtModel = std::make_unique<expr::xrt::TableModel>(model, *xtr, *xlw);
    sea::solver::Model &solvedModel =
        ackModel   ? static_cast<sea::solver::Model &>(*ackModel)
        : xrtModel ? static_cast<sea::solver::Model &>(*xrtModel)
                   : model;
    // initial words: the byte arrays read back from the word arrays
    std::unique_ptr<expr::WordsModel> wordsModel;
    if (words && words->stats().chosen)
      wordsModel = std::make_unique<expr::WordsModel>(solvedModel, *words);
    sea::solver::Model &replayModel =
        wordsModel ? static_cast<sea::solver::Model &>(*wordsModel)
                   : solvedModel;
    Phase replayPhase("replay");
    sea::ReplayResult replay =
        sea::replayAndValidate(program, replayModel, sem, ctx, addr0IsNull,
                               memhavocNull == MemhavocNull::Noop);
    SEA_PHASE_END(replayPhase)
        .kv("status", sea::stringifyRunStatus(replay.run.status))
        .kv("checked", replay.checked)
        .kv("mismatches", replay.mismatches.size())
        .kv("validated", replay.mismatches.empty());
    if (xrtModel)
      vout() << "xrt.model: relevant=" << xrtModel->relevant()
             << " inconsistent-relevant=" << xrtModel->inconsistentRelevant()
             << " inconsistent-irrelevant="
             << xrtModel->inconsistentIrrelevant()
             << " defaulted=" << xrtModel->defaulted()
             << " broken=" << xrtModel->broken() << "\n";
    if (wordsModel)
      vout() << "words.model: decoded=" << wordsModel->decoded()
             << " defaulted=" << wordsModel->defaulted() << "\n";
    if (ackModel) {
      vout() << "vc.ackermann-model: defaulted=" << ackModel->defaulted()
             << " inconsistent=" << ackModel->inconsistent();
      if (strideOn)
        vout() << " stride-broken=" << ackModel->strideBroken();
      vout() << "\n";
      // decided before the result (above); here a bug
      if (ackModel->strideBroken())
        sea::diag::error("vc.ackermann-inconsistent",
                         "a stride-rule collision found after the result");
      if (ackModel->inconsistent())
        sea::diag::error("vc.ackermann-inconsistent",
                         "the model of the lowered query is not a function on "
                         "an array: a bug in the Ackermann lowering");
    }
    for (const sea::CellRead &cr : replay.run.cellReads) {
      sea::out::Record rec("memread");
      rec.num("site", sea::memSiteOf(cr.siteOp))
          .integer("addr", cr.addr)
          .num("width", cr.value.getBitWidth())
          .integer("value", cr.value);
      raw_string_ostream os(rec.text);
      os << "memread: site=" << sea::memSiteOf(cr.siteOp) << " addr=";
      cr.addr.print(os, /*isSigned=*/false);
      os << " width=" << cr.value.getBitWidth() << " value=";
      cr.value.print(os, /*isSigned=*/false);
      if (std::string obj = sea::locateAddress(replay.run.objects, cr.addr);
          !obj.empty()) {
        os << " obj=" << obj;
        rec.add(sea::locateAddressFields(replay.run.objects, cr.addr));
      }
      gOut->emit(rec);
    }
    for (sea::NondetRead &rd : replay.run.reads) {
      sea::out::Record rec("nondet");
      rec.num("site", rd.site)
          .str("callee", rd.callee)
          .num("width", rd.value.getBitWidth())
          .integer("value", rd.value);
      raw_string_ostream os(rec.text);
      os << "nondet: site=" << rd.site << " callee=" << rd.callee
         << " width=" << rd.value.getBitWidth() << " value=";
      rd.value.print(os, /*isSigned=*/false);
      gOut->emit(rec);
    }
    // the status line stays exactly `replay: <status>` (harnesses compare
    // it); the location is a line of its own
    // jsonl: one `replay` record of these lines
    bool assertFailed = replay.run.status == sea::RunStatus::AssertFailed;
    {
      sea::out::Record rec("replay");
      rec.str("status", sea::stringifyRunStatus(replay.run.status));
      raw_string_ostream os(rec.text);
      os << "replay: " << sea::stringifyRunStatus(replay.run.status);
      if (assertFailed && replay.run.failLoc) {
        if (std::string l = sea::formatLoc(*replay.run.failLoc); !l.empty())
          os << "\nreplay: assert-at: " << l;
        sea::addLocFields(rec, "at", *replay.run.failLoc);
      }
      if (std::optional<Location> stop = sea::stopLocOf(replay.run)) {
        std::string l;
        raw_string_ostream ls(l);
        ls << *stop;
        os << "\nat: " << l;
        rec.str("loc", l);
      }
      if (assertFailed) {
        std::string ctx;
        raw_string_ostream cs(ctx);
        sea::printAssertContext(cs, "replay: ", replay.run);
        if (!ctx.empty())
          os << "\n" << StringRef(ctx).drop_back();
        if (replay.run.assertLoc)
          sea::addLocFields(rec, "op", *replay.run.assertLoc);
        sea::addPathFields(rec, replay.run.branchPath);
      }
      if (!replay.run.message.empty()) {
        os << "\nmessage: " << replay.run.message;
        rec.str("message", replay.run.message);
      }
      gOut->emit(rec);
    }
    // the program's prints: a run of their own on the same model (the
    // validation run skips them; docs/design/Print.md)
    bool hasPrints = false;
    program.walk([&](Operation *op) { hasPrints |= sea::isPrintOp(op); });
    if (hasPrints && printMode != sea::PrintMode::Off) {
      sea::RunOptions printOptions;
      printOptions.printLimit = printLimit;
      printOptions.addr0IsNull = addr0IsNull;
      printOptions.memhavocNullNoop = memhavocNull == MemhavocNull::Noop;
      sea::RunResult printed =
          sea::replayPrints(program, replayModel, sem, ctx, printOptions);
      sea::printPrintRecords(*gOut, printed, printMode, printLocs);
    }
    {
      sea::out::Record rec("validate");
      rec.num("checked", replay.checked)
          .num("mismatches", replay.mismatches.size())
          .num("dontcare", replay.dontCare);
      raw_string_ostream os(rec.text);
      os << "validate: checked=" << replay.checked
         << " mismatches=" << replay.mismatches.size()
         << " dontcare=" << replay.dontCare;
      gOut->emit(rec);
    }
    // loads of earlier memory versions (Mem-SSA no-clobber claims): checked
    // by the validation above, so a count, not a warning per load
    if (!replay.run.earlierVersionLoads.empty())
      vout() << "interp.earlier-version-loads: "
             << replay.run.earlierVersionLoads.size() << "\n";
    for (const sea::ValueMismatch &m : replay.mismatches) {
      sea::out::Record rec("mismatch");
      std::string where, ty;
      raw_string_ostream ws(where), ts(ty);
      printValue(ws, m.value);
      ts << m.value.getType();
      rec.str("def", where).str("type", ty).integer("interp", m.interp);
      raw_string_ostream os(rec.text);
      os << "mismatch: " << where << " interp=";
      m.interp.print(os, /*isSigned=*/false);
      os << " model=";
      if (m.model) {
        m.model->print(os, /*isSigned=*/false);
        rec.integer("model", *m.model);
      } else {
        os << "none";
        rec.set("model", "null");
      }
      // an address: the object it points into (a location, by range)
      if (isa<sea::AddrType>(m.value.getType()))
        if (std::string obj = sea::locateAddress(replay.run.objects, m.interp);
            !obj.empty()) {
          os << " obj=" << obj;
          rec.add(sea::locateAddressFields(replay.run.objects, m.interp));
        }
      gOut->emit(rec);
    }
    // a mismatch explained by the program's own Mem-SSA no-clobber claim
    // (Memory.md): a load of an earlier version read a clobbered cell
    // (one line per location: a load split into cells keeps its location)
    SmallVector<Operation *> noClobber = sea::noClobberSuspects(replay);
    llvm::StringSet<> noClobberLocs;
    for (Operation *load : noClobber) {
      sea::out::Record rec("no-clobber-violated");
      std::string where;
      raw_string_ostream ws(where);
      if (std::optional<FileLineColLoc> l = sea::fileLocOf(load->getLoc()))
        sea::printFileLoc(ws, *l);
      else
        ws << load->getLoc();
      if (!noClobberLocs.insert(where).second)
        continue;
      rec.str("loc", where);
      raw_string_ostream os(rec.text);
      os << "replay: no-clobber-violated: " << where
         << " (a load of an earlier memory version read a cell overwritten "
            "since; the program's Mem-SSA no-clobber claim fails here)";
      gOut->emit(rec);
    }
    // --ptr-companion=check: the query's property is the A-PTRINT guard of
    // every split pointer load, not the program's assertions; the replay
    // checks the guards on the run (PtrIntObservation) and names each that
    // fails: the stored pointer differs (A-PTRINT violated), or no pointer
    // was stored at the cell (A-PTRINT names none: not discharged)
    const bool ptrIntCheck = sem.ptrIntChecking() && sem.companionChecked();
    unsigned ptrIntViolated = 0, ptrIntUnstored = 0;
    if (ptrIntCheck) {
      for (const sea::PtrIntObservation &o : replay.ptrInt) {
        if (o.holds())
          continue;
        (o.stored ? ptrIntViolated : ptrIntUnstored)++;
        const char *what = o.stored ? "violated" : "unstored";
        sea::out::Record rec("ptrint");
        rec.str("status", what);
        raw_string_ostream os(rec.text);
        os << "seam.ptrint-" << what << ": ";
        std::string l = sea::formatLoc(o.fromBits->getLoc());
        if (l.empty()) {
          raw_string_ostream ls(l);
          ls << o.fromBits->getLoc();
        }
        os << l;
        rec.str("at", l);
        os << " addr=";
        o.addr.print(os, /*isSigned=*/false);
        rec.integer("addr", o.addr);
        if (std::string obj = sea::locateAddress(replay.run.objects, o.addr);
            !obj.empty()) {
          os << " obj=" << obj;
          rec.add(sea::locateAddressFields(replay.run.objects, o.addr));
        }
        os << " bits=";
        o.bits.print(os, /*isSigned=*/false);
        rec.integer("bits", o.bits);
        if (o.stored) {
          os << " stored=";
          o.stored->print(os, /*isSigned=*/false);
          rec.integer("stored", *o.stored);
        }
        gOut->emit(rec);
      }
      sea::out::Record rec("ptrint-replay");
      rec.num("loads", replay.ptrInt.size())
          .num("holds", replay.ptrInt.size() - ptrIntViolated - ptrIntUnstored)
          .num("violated", ptrIntViolated)
          .num("unstored", ptrIntUnstored);
      raw_string_ostream os(rec.text);
      os << "seam.ptrint-replay: loads=" << replay.ptrInt.size()
         << " holds=" << replay.ptrInt.size() - ptrIntViolated - ptrIntUnstored
         << " violated=" << ptrIntViolated << " unstored=" << ptrIntUnstored;
      gOut->emit(rec);
    }
    // what the sat claims, seen on the run: an assertion fails, or (the
    // A-PTRINT discharge) a split load's guard fails
    const bool claimSeen =
        ptrIntCheck ? ptrIntViolated + ptrIntUnstored > 0
                    : replay.run.status == sea::RunStatus::AssertFailed;
    gOut->flush();
    // A failed replay under A-INB: if the model's inputs make the abstract
    // program access memory outside an object, the sat is not a bug but an
    // artifact of the violated assumption (conditional soundness: the answer
    // holds only for executions that satisfy A-INB), and the program has a
    // memory-safety error. Discharging A-INB is a separate memory-safety
    // check's work.
    bool replayFailed = replay.run.status == sea::RunStatus::Error ||
                        !replay.mismatches.empty() || !claimSeen;
    if (replayFailed && inb && abstractModule) {
      sea::ProgramOp absProg;
      for (auto p : abstractModule->getOps<sea::ProgramOp>())
        if (p.getSymName() == program.getSymName())
          absProg = p;
      // nondet sites are stable across the lowering. Cells of nondet
      // memories are addresses in the model, not object offsets, so they are
      // not given: a run that reads one stops (Missing::Error) and decides
      // nothing. A run that reads none is the model's execution (same
      // inputs, deterministic), so a memory-safety error in it is the
      // model's own.
      sea::InputSource inputs(sea::InputSource::Missing::Error);
      for (const sea::NondetRead &rd : replay.run.reads) {
        llvm::SmallString<32> text;
        rd.value.toStringUnsigned(text);
        inputs.set(rd.site, std::string(text));
      }
      sea::RunOptions absOptions;
      absOptions.addr0IsNull = addr0IsNull;
      absOptions.memhavocNullNoop = memhavocNull == MemhavocNull::Noop;
      sea::RunResult abs = sea::interpret(absProg, inputs, absOptions);
      if (abs.addr0IsNullUses)
        vout() << "assume.addr0-is-null: uses=" << abs.addr0IsNullUses << "\n";
      if (abs.memhavocNullNoopUses)
        vout() << "assume.memhavoc-null-noop: uses=" << abs.memhavocNullNoopUses
               << "\n";
      if (sea::isOutOfObjectAccess(abs)) {
        vout() << "seam.inb-violation: " << abs.message << "\n";
        if (abs.at)
          vout() << "seam.inb-violation-at: " << abs.at->getLoc() << "\n";
        gOut->flush();
        sea::diag::error(
            "vc.seam-inb-spurious",
            "sat, but the counterexample does not replay: the program "
            "accesses memory outside an object with these inputs, so A-INB "
            "(assumed by --seam) does not hold; the answer is inconclusive "
            "and the program has a memory-safety error (a separate "
            "memory-safety check of the program discharges A-INB; "
            "--seam-inb=0 drops it from the VC only, not from the lowering's "
            "dropped memhavocs, assume.a-inb)",
            abs.at ? std::optional<Location>(abs.at->getLoc()) : std::nullopt);
        return 4;
      }
      vout() << "seam.inb-violation: undetermined ("
             << sea::stringifyRunStatus(abs.status)
             << (abs.message.empty() ? "" : ": " + abs.message) << ")\n";
      gOut->flush();
    }
    // The interpreter could not run the program (e.g. memory updates that are
    // not in place): no counterexample, but no evidence of a VC bug either.
    if (replay.run.status == sea::RunStatus::Error) {
      sea::diag::error("vc.replay-impossible",
                       "sat, but the counterexample cannot be replayed: " +
                           replay.run.message,
                       replay.run.at
                           ? std::optional<Location>(replay.run.at->getLoc())
                           : std::nullopt);
      return 1;
    }
    // with initial words, a trusted `align` that does not hold makes the
    // annotated fast path read a word the bytes do not form: the word query
    // has a sat the byte query has not (SeaM-initial-words.md rev 3, point 5)
    auto noteWordsAlign = [&]() {
      if (words && words->stats().alignedByAnnotation)
        sea::diag::warn("vc.words-align-trusted",
                        Twine(words->stats().alignedByAnnotation) +
                            " word read(s) relied on a trusted `align` "
                            "(--seam-words): a wrong annotation also explains "
                            "a failed replay; check with --check-align");
    };
    if (!replay.mismatches.empty() || !claimSeen)
      noteWordsAlign();
    if (!replay.mismatches.empty()) {
      if (!noClobber.empty())
        sea::diag::error(
            "vc.replay-mismatch",
            Twine(replay.mismatches.size()) +
                " value(s) differ between the model and the interpreter: the "
                "cause is the program's Mem-SSA no-clobber claim, not the "
                "VC: " +
                Twine(noClobber.size()) +
                " load(s) of an earlier memory version read a cell overwritten "
                "since (replay: no-clobber-violated); the VC reads the version "
                "the program names");
      else
        sea::diag::error("vc.replay-mismatch",
                         Twine(replay.mismatches.size()) +
                             " value(s) differ between the model and the "
                             "interpreter: a VC or interpreter bug, or a load "
                             "of an earlier memory version whose cell was "
                             "clobbered");
      return 2;
    }
    if (ptrIntCheck && !claimSeen) {
      sea::diag::error("vc.replay-no-failure",
                       "sat under --ptr-companion=check, but no split pointer "
                       "load of the replayed run fails its A-PTRINT guard "
                       "(status: " +
                           sea::stringifyRunStatus(replay.run.status) +
                           "): a VC or interpreter bug");
      return 2;
    }
    if (!ptrIntCheck && replay.run.status != sea::RunStatus::AssertFailed) {
      sea::diag::error("vc.replay-no-failure",
                       "sat, but the replayed run does not fail the "
                       "assertion (status: " +
                           sea::stringifyRunStatus(replay.run.status) +
                           "): a VC or interpreter bug");
      return 2;
    }
    if (ackModel && ackModel->inconsistent())
      return 2;
    if (originalModule)
      return replayOnOriginal(replayModel, sem, ctx, replay.run, ptrIntCheck);
    return 0;
  };
  // one VC and solver run with its fallbacks (a part of a split, or the
  // whole query)
  auto runOnce = [&]() -> int {
    int rc = runVc(seamInb, seam && seamInb, "");
    if (rc == kRetryExactCompanion) {
      companionMode = CompanionMode::Exact;
      rc = runVc(seamInb, seam && seamInb, " (fallback: sat under A-PTRINT)");
    }
    if (rc == kRetryWithoutStride) {
      strideOn = false;
      rc = runVc(seamInb, seam && seamInb,
                 " (fallback: the model breaks a GEP promise)");
      if (rc == kRetryExactCompanion) {
        companionMode = CompanionMode::Exact;
        rc = runVc(seamInb, seam && seamInb, " (fallback: sat under A-PTRINT)");
      }
    }
    // the A-INB fallback keeps the rule: its firings that rest on A-INB (K,
    // size-0 bases) are off without it.
    if (rc == kRetryWithoutInb) {
      // the lowering's drops still rest on A-INB, which this program breaks
      if (dropFreshInb)
        sea::diag::warn("vc.seam-inb-fallback-conditional",
                        Twine("the run without A-INB still rests on it: the "
                              "lowering dropped ") +
                            Twine(dropFreshInb) +
                            " memhavoc(s) that are no-ops only under A-INB "
                            "(assume.a-inb), and an access of this program "
                            "leaves its object: the answer is conditional on "
                            "an assumption the program is known to break");
      rc = runVc(false, false, " (fallback: an access leaves its object)");
    }
    return rc;
  };
  int rc = 0;
  if (splitCandidatesOpt) {
    // --split-candidates: the pick run of --split-auto, listed; no split,
    // no solver
    rc = [&]() -> int {
      splitPickRun = true;
      int prc = runVc(seamInb, false, "");
      splitPickRun = false;
      if (prc)
        return prc;
      // no open question: the address candidates by the read indices that
      // hold them (--split-auto's order has nothing to rank by)
      if (llvm::all_of(splitCandidates,
                       [](const SplitCandidate &c) { return c.open == 0; }))
        std::stable_sort(splitCandidates.begin(), splitCandidates.end(),
                         [](const SplitCandidate &a, const SplitCandidate &b) {
                           return a.reads != b.reads ? a.reads > b.reads
                                                     : a.leaves > b.leaves;
                         });
      const size_t nBool = splitBoolCandidates.size();
      const size_t listed = splitCandidatesMax
                                ? std::min<size_t>(nBool, splitCandidatesMax)
                                : nBool;
      {
        std::string t = llvm::formatv("{0:f3}", splitPickSeconds).str();
        sea::out::Record rec("split-candidates");
        rec.num("open_aliases", splitOpenAliases)
            .num("leaves_candidates", splitCandidates.size())
            .num("bool_candidates", nBool)
            .num("bool_listed", listed)
            .num("bool_dropped", nBool - listed)
            .num("bool_values", splitBoolRecorded)
            .num("reads", splitReads)
            .num("same_object_open", splitSameObjectOpen)
            .num("memop_open", splitMemopOpen)
            .set("vc_time", t);
        rec.text =
            "split.candidates: open-aliases=" +
            std::to_string(splitOpenAliases) +
            " leaves-candidates=" + std::to_string(splitCandidates.size()) +
            " bool-candidates=" + std::to_string(nBool) +
            " bool-listed=" + std::to_string(listed) +
            " bool-dropped=" + std::to_string(nBool - listed) +
            " bool-values=" + std::to_string(splitBoolRecorded) +
            " reads=" + std::to_string(splitReads) +
            " same-object-open=" + std::to_string(splitSameObjectOpen) +
            " memop-open=" + std::to_string(splitMemopOpen) + " vc-time=" + t;
        gOut->emit(rec);
      }
      for (const SplitCandidate &c : splitCandidates) {
        sea::out::Record rec("split-leaves-candidate");
        rec.str("name", c.name)
            .str("loc", c.loc)
            .num("leaves", c.leaves)
            .num("unknown_leaves", c.unknownLeaves)
            .num("open_aliases", c.open)
            .num("direct", c.direct)
            .num("own", c.own)
            .num("reads", c.reads);
        rec.text = "split.leaves-candidate: name=" + c.name +
                   " leaves=" + std::to_string(c.leaves) +
                   " unknown-leaves=" + std::to_string(c.unknownLeaves) +
                   " open-aliases=" + std::to_string(c.open) +
                   " direct=" + std::to_string(c.direct) +
                   " own=" + std::to_string(c.own) +
                   " reads=" + std::to_string(c.reads) +
                   " loc=" + (c.loc.empty() ? "-" : c.loc);
        gOut->emit(rec);
      }
      for (size_t i = 0; i < listed; ++i) {
        const SplitBoolCandidate &c = splitBoolCandidates[i];
        sea::out::Record rec("split-bool-candidate");
        rec.str("name", c.name)
            .str("loc", c.loc)
            .str("target", c.guard ? "guard" : "value")
            .num("open_aliases", c.open)
            .num("addrs", c.addrs)
            .num("reads", c.reads)
            .num("cond_reads", c.condReads)
            .num("covers", c.covers)
            .str("same", c.same.empty() ? "-" : llvm::join(c.same, ","));
        rec.text = "split.bool-candidate: name=" + c.name +
                   " target=" + (c.guard ? "guard" : "value") +
                   " open-aliases=" + std::to_string(c.open) +
                   " addrs=" + std::to_string(c.addrs) +
                   " reads=" + std::to_string(c.reads) +
                   " cond-reads=" + std::to_string(c.condReads) +
                   " covers=" + std::to_string(c.covers) +
                   " same=" + (c.same.empty() ? "-" : llvm::join(c.same, ",")) +
                   " loc=" + (c.loc.empty() ? "-" : c.loc);
        gOut->emit(rec);
      }
      const size_t nArith = splitArithCandidates.size();
      const size_t arithListed =
          splitCandidatesMax ? std::min<size_t>(nArith, splitCandidatesMax)
                             : nArith;
      {
        sea::out::Record rec("split-arith-candidates");
        rec.num("arith_nodes", splitArithNodes)
            .num("int_values", splitIntRecorded)
            .num("candidates", nArith)
            .num("listed", arithListed)
            .num("dropped", nArith - arithListed);
        rec.text = "split.arith-candidates: arith-nodes=" +
                   std::to_string(splitArithNodes) +
                   " int-values=" + std::to_string(splitIntRecorded) +
                   " candidates=" + std::to_string(nArith) +
                   " listed=" + std::to_string(arithListed) +
                   " dropped=" + std::to_string(nArith - arithListed);
        gOut->emit(rec);
      }
      for (size_t i = 0; i < arithListed; ++i) {
        const SplitArithCandidate &c = splitArithCandidates[i];
        std::string same = c.same.empty() ? "-" : llvm::join(c.same, ",");
        sea::out::Record rec("split-arith-candidate");
        rec.str("name", c.name)
            .str("loc", c.loc)
            .str("target", c.target)
            .num("arith", c.arith)
            .num("mul", c.mul)
            .num("div", c.div)
            .num("direct", c.direct)
            .num("feeds", c.feeds)
            .num("cases", c.cases)
            .str("same", same);
        rec.text = "split.arith-candidate: name=" + c.name +
                   " target=" + c.target + " arith=" + std::to_string(c.arith) +
                   " mul=" + std::to_string(c.mul) +
                   " div=" + std::to_string(c.div) +
                   " direct=" + std::to_string(c.direct) +
                   " feeds=" + std::to_string(c.feeds) +
                   " cases=" + std::to_string(c.cases) + " same=" + same +
                   " loc=" + (c.loc.empty() ? "-" : c.loc);
        gOut->emit(rec);
      }
      sea::out::Record rec("result");
      rec.str("value", "none");
      rec.text = "result: none";
      gOut->emit(rec);
      brunchStat(vout(), "result", "none");
      gOut->flush();
      return 0;
    }();
  } else if (!splitOn) {
    rc = runOnce();
  } else {
    rc = [&]() -> int {
      // --split-auto: the pick run, then the leaves of the top candidate
      if (splitAuto) {
        splitPickRun = true;
        int prc = runVc(seamInb, false, "");
        splitPickRun = false;
        if (prc)
          return prc;
        unsigned shown = 0;
        for (const SplitCandidate &c : splitCandidates) {
          if (shown++ == 5)
            break;
          sea::out::Record rec("split-candidate");
          rec.str("name", c.name)
              .str("loc", c.loc)
              .num("leaves", c.leaves)
              .num("open_aliases", c.open);
          rec.text = "split.candidate: name=" + c.name +
                     " leaves=" + std::to_string(c.leaves) +
                     " open-aliases=" + std::to_string(c.open) +
                     " loc=" + (c.loc.empty() ? "-" : c.loc);
          gOut->emit(rec);
        }
        const SplitCandidate *pick = nullptr;
        for (const SplitCandidate &c : splitCandidates)
          if (c.open && c.leaves >= 2) {
            pick = &c;
            break;
          }
        if (!pick) {
          sea::diag::warn("vc.split-auto-none",
                          "--split-auto: no ite-valued address with at "
                          "least two leaves is behind an open alias "
                          "question");
        } else {
          for (const sea::SplitTarget &t : splitTargets)
            if (t.value == pick->value)
              pick = nullptr;
          if (pick) {
            splitTargets.push_back({sea::SplitTarget::Kind::Leaves, pick->value,
                                    nullptr, pick->name, "auto"});
            vout() << "split.auto-pick: " << pick->name << "\n";
          }
        }
      }
      // the parts, depth first: a run fixes the choices of its prefix and
      // takes case 0 below it; its levels give the siblings to run later
      std::vector<std::vector<unsigned>> todo{{}};
      unsigned nUnsat = 0, nSat = 0, nUnknown = 0, nNone = 0;
      unsigned nCheckUnknown = 0, nLeafStructural = 0;
      double hardest = -1;
      unsigned hardestId = 0;
      std::vector<std::string> unknownParts;
      bool stopped = false;
      while (!todo.empty()) {
        splitPrefix = std::move(todo.back());
        todo.pop_back();
        splitPart = SplitPartInfo();
        {
          sea::out::Record rec("split-part-start");
          rec.num("part", splitPartId);
          rec.text = "split.part-start: " + std::to_string(splitPartId);
          gOut->emit(rec);
          gOut->flush();
        }
        // a fallback in one part (stride, companion) does not carry over
        const bool strideSaved = strideOn;
        const CompanionMode companionSaved = companionMode;
        int prc = runOnce();
        strideOn = strideSaved;
        companionMode = companionSaved;
        if (prc != 0 && prc != 3)
          return prc; // an error or a bug: stop
        const auto &lv = splitPart.levels;
        if (lv.size() < splitPrefix.size()) {
          sea::diag::error("vc.split-unstable",
                           "a part met fewer split levels than its planned "
                           "choices (the encoding is not deterministic)");
          return 1;
        }
        // siblings below the prefix, deepest last (run first)
        for (size_t i = splitPrefix.size(); i < lv.size(); ++i)
          for (unsigned j = lv[i].first; j-- > 1;) {
            std::vector<unsigned> p;
            for (size_t k = 0; k < i; ++k)
              p.push_back(lv[k].second);
            p.push_back(j);
            todo.push_back(std::move(p));
          }
        {
          const SplitPartInfo &pi = splitPart;
          sea::out::Record rec("split-part");
          auto sec = [](double d) { return llvm::formatv("{0:f3}", d).str(); };
          rec.num("part", splitPartId)
              .str("choice", pi.choice)
              .str("result", pi.result)
              .set("vc_time", sec(pi.vcSeconds))
              .set("solve_time", sec(pi.solveSeconds))
              .num("dag", pi.dag)
              .num("alias_open", pi.aliasOpen)
              .num("alias_open_pairs", pi.aliasOpenPairs)
              .num("alias_open_memop", pi.memopOpen)
              .num("rows", pi.rows)
              .num("ack_reads", pi.ackReads)
              .num("ack_pairs", pi.ackPairs)
              .num("ack_constraints", pi.ackConstraints)
              .str("check", pi.check);
          rec.text = "split.part: " + std::to_string(splitPartId) +
                     " choice=" + pi.choice + " result=" + pi.result +
                     " vc-time=" + sec(pi.vcSeconds) +
                     " solve-time=" + sec(pi.solveSeconds) +
                     " dag=" + std::to_string(pi.dag) +
                     " alias-open=" + std::to_string(pi.aliasOpen) +
                     " alias-open-pairs=" + std::to_string(pi.aliasOpenPairs) +
                     " alias-open-memop=" + std::to_string(pi.memopOpen) +
                     " rows=" + std::to_string(pi.rows) +
                     " ack-reads=" + std::to_string(pi.ackReads) +
                     " ack-pairs=" + std::to_string(pi.ackPairs) +
                     " ack-constraints=" + std::to_string(pi.ackConstraints) +
                     " check=" + pi.check;
          gOut->emit(rec);
          gOut->flush();
          if (pi.result == "unsat")
            ++nUnsat;
          else if (pi.result == "sat")
            ++nSat;
          else if (pi.result == "none")
            ++nNone;
          else {
            ++nUnknown;
            unknownParts.push_back(std::to_string(splitPartId) + ":" +
                                   pi.choice);
          }
          if (pi.solveSeconds > hardest) {
            hardest = pi.solveSeconds;
            hardestId = splitPartId;
          }
        }
        nCheckUnknown += splitPart.checkUnknown;
        nLeafStructural += splitPart.leafStructural;
        ++splitPartId;
        if (nSat && splitStopOnSat) {
          stopped = !todo.empty();
          break;
        }
        if (splitPartId + todo.size() > splitMaxParts) {
          sea::diag::error(
              "vc.split-max-parts",
              "the split has more than " + Twine(unsigned(splitMaxParts)) +
                  " parts (--split-max-parts); " + Twine(splitPartId) +
                  " run, " + Twine(unsigned(todo.size())) +
                  " known left: no verdict");
          return 1;
        }
      }
      const char *verdict = nSat       ? "sat"
                            : nNone    ? "none"
                            : nUnknown ? "unknown"
                                       : "unsat";
      {
        sea::out::Record rec("split-verdict");
        rec.str("result", verdict)
            .num("parts", splitPartId)
            .num("unsat", nUnsat)
            .num("sat", nSat)
            .num("unknown", nUnknown)
            .num("none", nNone)
            .num("hardest_part", hardestId)
            .set("hardest_solve_time",
                 llvm::formatv("{0:f3}", std::max(hardest, 0.0)).str())
            .num("check_unknown", nCheckUnknown)
            .str("check", splitCheck ? "on" : "off")
            .num("leaf_structural", nLeafStructural);
        if (stopped)
          rec.str("stopped", "first-sat");
        rec.text = std::string("split.verdict: ") + verdict +
                   " parts=" + std::to_string(splitPartId) +
                   " unsat=" + std::to_string(nUnsat) +
                   " sat=" + std::to_string(nSat) +
                   " unknown=" + std::to_string(nUnknown) +
                   " none=" + std::to_string(nNone) +
                   " hardest-part=" + std::to_string(hardestId) +
                   " hardest-solve-time=" +
                   llvm::formatv("{0:f3}", std::max(hardest, 0.0)).str() +
                   " check-unknown=" + std::to_string(nCheckUnknown) +
                   " check=" + (splitCheck ? "on" : "off") +
                   " leaf-structural=" + std::to_string(nLeafStructural) +
                   (stopped ? " stopped=first-sat" : "");
        gOut->emit(rec);
        for (const std::string &u : unknownParts)
          vout() << "split.unknown-part: " << u << "\n";
      }
      sea::out::Record rec("result");
      rec.str("value", verdict);
      rec.text = std::string("result: ") + verdict;
      gOut->emit(rec);
      gOut->flush();
      return nSat || nUnknown == 0 ? 0 : 3;
    }();
  }
  if (auditOn) {
    statOut() << auditReport;
    gOut->flush();
  }
  if (checkOn) {
    statOut() << checkReport;
    gOut->flush();
    if (checkInvalid && rc != 1) {
      sea::diag::error("seam.check-invalid",
                       Twine(checkInvalid) +
                           " rule firing(s) are not valid under their "
                           "premises: a rewriting bug (seam-check-invalid "
                           "lines)");
      rc = 5;
    }
  }
  gOut->flush();
  brunchStat(statOut(), "status", Twine(rc));
  return rc;
}
