// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- sea-vc.cpp - verify a sea program with a VC --------------*- C++ -*-===//
//
// Encodes a sea program (in BMC form) as a SeaHorn-style VC, solves it, and on
// sat replays the model in the interpreter with validation.
//
//===----------------------------------------------------------------------===//

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
#include "sea/Support/Output.h"
#include "sea/Support/Version.h"
#include "sea/Transforms/Passes.h"

#include "mlir/Pass/PassManager.h"
#include "sea/VC/Replay.h"
#include "sea/VC/VCGen.h"

#include <chrono>
#include <cstdlib>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
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
             "after the lowerings (merge equal values, fold, erase dead "
             "ops and unreachable blocks; default on; =false keeps every op)"),
    cl::init(true));

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
             "by pulling an ite condition its bytes share out of the "
             "concat, at most N pulls per load, kept only when a side "
             "reassembles a stored pointer (PtrSort S6 probe, counter "
             "ptr.load-resolve-concat). 0 (default): off"),
    cl::init(0));

static cl::opt<bool> seamCongruence(
    "seam-congruence",
    cl::desc("With --seam: decide two addresses apart when their "
             "difference is sum c_k x_k + d with 2^t dividing every c_k "
             "and not d (exact, no premise; rule cmp.congruence; e.g. "
             "T + 8i + 1 vs T + 8j). Default off"),
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

static cl::opt<bool> progress(
    "progress",
    cl::desc("Print `phase: <name>` on stderr at the start of each phase "
             "(lower, encode, simplify, solver-term, ackermann, solve, "
             "replay)"),
    cl::init(false));

/// --progress: the phase starting now, flushed
static void phase(const char *name) {
  if (!progress)
    return;
  errs() << "phase: " << name << "\n";
  errs().flush();
}

/// --mem-limit: an allocation that fails (operator new; Bitwuzla, the VC)
/// ends the run as unknown. Writes without allocating.
static void memLimitHit() {
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
  static const char err[] =
      "Error: memory limit (--mem-limit) hit [vc.mem-limit]\n";
  (void)!::write(2, err, sizeof(err) - 1);
  ::_exit(3);
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
  no critical edges; sea.cond_br targets distinct. nsw / nuw flags are
  ignored (the value wraps, as in SeaHorn); use sea-wrap-flags to assume or
  assert them.

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
  sea-vc p.mlir --vcgen-only     # generate and check the VC, print its
                                 #   statistics and result: none; no solver
                                 #   (also not --seam-check / --eq-audit /
                                 #   --x-read-table-check); not with --smt2
  sea-vc p.mlir --print-refs=strong  # keep every printed value alive
                                 #   through the lowering (PRINTS)
  sea-vc p.mlir --progress       # phase: <name> lines on stderr (flushed)
                                 #   as each phase starts
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
                                 #   before splitting edges (--lower-opt)
  sea-vc p.mlir --lower --lower-opt=false  # the lowering without cse /
                                 #   canonicalize: every op stays (A/B of
                                 #   the clean-up)
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
                                 #   pointer load by pulling an ite its
                                 #   bytes share out of the concat (<= N
                                 #   pulls; counter ptr.load-resolve-concat)
  sea-vc p.mlir --lower --seam --ptr-companion=exact  # split pointer
                                 #   loads read a companion memory of the
                                 #   stored pointers, guarded (opaque mode;
                                 #   counters vc.ptr-companion*); =assume:
                                 #   unguarded under A-PTRINT
                                 #   (seam.ptrint), a sat answer rerun exact
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
  sea-vc p.mlir --seam --seam-stride  # R-STRIDE: store-map entries
                                 #   skipped for reads at promised GEP
                                 #   nodes whose offsets never reach them
                                 #   (premise.ptr-index; --seam-check proves
                                 #   each firing under P1-P3). Off: on vcc
                                 #   it slows the solver (PtrIndex.md 0c)
  sea-vc p.mlir --ack-dump-pairs=<f>  # with the lowering on: every lowered
                                 #   read (`read <a> <k> <index>`) and every
                                 #   constrained pair (`pair <a> <k> <l>`)
                                 #   to <f>, for a census of undecided pairs
  sea-vc p.mlir --vc-join=eq     # joins as fresh constants with guarded
                                 #   equalities (default ite: a block
                                 #   argument is ite over the incoming edges,
                                 #   as SeaHorn's --horn-vcgen-use-ite)

OUTPUT (stdout, `key: value` lines):
  tool.version: sea-mlir <x.y.z> (<git describe>)
                                  (first line, always; with --smt2 on
                                  stderr; --version: also LLVM and solvers)
  vc.config: <path>               (--config: each config loaded, in order,
                                  first; with --smt2 on stderr)
  program: @<name>
  vc.ptr-companion: loads=<n> stores=<n> copies=<n> fresh=<n>
                                  (--ptr-companion: split pointer loads read
                                  through the companion, pointer stores /
                                  memmoves it records, fresh companion
                                  arrays)
  vc.ptr-companion-arrays: <n>    (companion arrays the query still reads)
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
  result: sat|unsat|unknown|none  (none: --vcgen-only)
  solve-timeout: true             (unknown because --timeout expired)
  mem-limit: hit                  (unknown because --mem-limit was hit
                                  (RLIMIT_AS: address space, not resident
                                  memory, so it can hit before the resident
                                  size reaches the limit):
                                  z3's memory-out; an allocation that
                                  fails (operator new: the VC, Bitwuzla's
                                  C++) prints result: unknown and this line
                                  and exits 3; Bitwuzla: an unknown within
                                  the time under --mem-limit. Not detected:
                                  Yices exits on its own out-of-memory
                                  (status 16, "Out of memory"), and GMP
                                  aborts)
  nondet: site=<n> callee=<f> width=<w> value=<unsigned>   (sat: from the model)
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
  assume.memset-bound: count=<n> bound=<K>   (n > 0: --lower without --seam
                                  expanded n symbolic-length memsets /
                                  memmoves with assume(len <= K), the
                                  sea.assume ops marked sea.memset_bound; an
                                  unsat holds only for lengths <= K; K the
                                  largest bound; with --smt2 on stderr)
  seam.inb: assumed accesses=<n> conditions=<m>
  seam.inb: off [(fallback: an access leaves its object)]
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
                                  pointers say; same-object: the offset
                                  compares one-object questions reduce to)
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

PROGRESS (--progress, stderr, flushed): `phase: <name>` at the start of
each phase: lower, encode, simplify (with the rewriter's normalization),
solver-term, ackermann, solve, replay (sat only). A run that restarts
(the vc.seam-inb-violated fallback, the --ack-stride retry) repeats them
from encode.

DIAGNOSTICS (stderr): "Warning: ... [kind]" (yellow) for incomplete or
approximate handling, once per kind; repeats are counted and summarized at
exit (--log=<kind> prints each). "Error: ... [kind]" (red) when the tool
cannot continue.
  memread: site=<n> addr=<a> width=<w> value=<v> [obj=&<o>[<off>]]
                                  (sat: cells from the model; obj: the
                                  placed object the cell lies in)

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
  mismatch      def (`<op> <loc>`), type, interp, model (null: none),
                [obj + off] (an address: the object it points into)
  no-clobber-violated  loc (the load's file:line:col)
  smt2          (--smt2) the query in text; the statistics follow as
                records on stdout (text mode: stderr)
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
`seam.inb-violation: undetermined (...)` and the replay's own status).
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

int main(int argc, char **argv) {
  InitLLVM y(argc, argv);
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
  if (memLimit) {
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
       seamPullIndex || seamResolveIteNodes || seamPullConcat ||
       seamCongruence || ptrCompanion != CompanionMode::Off || !seamStoreMap ||
       !seamSimplify) &&
      !seam)
    return fail("--seam-inb, --seam-row-expand, --seam-row-group, "
                "--seam-check-inb-premise, --seam-layout, --seam-prune-size0, "
                "--seam-wide, "
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
  if (checkAlign && checkPtrIndex) {
    sea::diag::error("vc.check-jobs",
                     "--check-align and --check-ptr-index are separate jobs "
                     "(each drops the program's asserts): run one at a time");
    return 1;
  }
  if (printRefs != sea::PrintRefs::AsIs) {
    PassManager pm(&context);
    sea::addPrintRefsPass(pm, printRefs);
    if (failed(pm.run(*module)))
      return 1;
  }
  if (lower) {
    phase("lower");
    PassManager pm(&context);
    // with --seam, memset / memmove / memhavoc are terms (SeaM-memops.md):
    // no sea-expand-memset, symbolic lengths are exact
    sea::buildVCLoweringPipeline(
        pm, abstract, nativeMulOverflow,
        /*fold=*/true, widenMemory,
        /*expandMemSet=*/!seam && !lambdaMem, widenMemoryReport,
        /*keepByteWide=*/seam && seamWide, checkAlign,
        checkPtrIndex ? (checkPtrIndexAssumeAsserts ? 2u : 1u) : 0u, lowerOpt);
    if (failed(pm.run(*module)))
      return 1;
  } else if (abstract) {
    sea::diag::error("vc.abstract-program",
                     "the program uses abstract pointers; place it first "
                     "(sea-opt --sea-place-static, or sea-vc --lower)",
                     program.getLoc());
    return 1;
  }
  if (failed(sea::verifyBmcForm(program)))
    return 1;
  // the bounded expansions of symbolic memset / memmove lengths
  // (sea-expand-memset, without --seam): assume(n <= K), an
  // under-approximation the result rests on (assume.memset-bound)
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
    if (failed(pm.run(program)))
      return 1;
  }

  // One run of the VC and the solver. Returns kRetryWithoutInb if A-INB
  // (assumed by the rewriting) is found not to hold.
  constexpr int kRetryWithoutInb = -1;
  // a sat model that breaks a GEP promise the stride rule relied on: rerun
  // without the rule (PtrIndex.md section 5)
  constexpr int kRetryWithoutStride = -2;
  // a sat answer under A-PTRINT: rerun with the exact companion (the
  // counterexample may rest on the premise)
  constexpr int kRetryExactCompanion = -3;
  CompanionMode companionMode = ptrCompanion;
  bool strideOn = ackStride;
  auto runVc = [&](bool inb, bool mayRetry, const char *note) -> int {
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
    seamOpts.storeMap = seamStoreMap;
    seamOpts.simplify = simplify;
    seamOpts.plantBug = plantBug;
    seamOpts.maxRuleCalls = seamRuleLimit;
    sea::SeaOpSem sem(efac, names, nativeMulOverflow, seam, &seamOpts);
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
    sea::VCGen vcgen(sem, joinEncoding);
    lap("setup");
    phase("encode");
    if (failed(vcgen.genVc(program, ctx)))
      return 1;
    lap("encode");
    // the final normalization pass: every side condition
    // and guarantee, then the query, with the cache of the load and store
    // rewriting
    if (sem.simplifies())
      phase("simplify");
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
    expr::Expr query = sea::mkQuery(ctx);
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
        }
        if (expr::isOpX<expr::IS_OVERLAP>(n) && sem.seamRewriter()) {
          const auto &R = sem.seamRewriter()->rules();
          auto va = R.view(expr::Expr(n->arg(0))),
               vb = R.view(expr::Expr(n->arg(2)));
          const char *k =
              va && vb ? (va->base == vb->base ? "same-object" : "diff-object")
              : va || vb ? "one-unknown"
                         : "both-unknown";
          ++aliasOpen[k];
          ++aliasOpen["all"];
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
    phase("solver-term");
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
    phase("ackermann");
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
    if (ackOn)
      query = ack.lower(query);
    if (ackOn && !ackDumpPairs.empty()) {
      std::error_code ec;
      llvm::raw_fd_ostream os(ackDumpPairs, ec);
      if (ec)
        return fail("--ack-dump-pairs: " + ec.message());
      const auto &arrs = ack.arrays();
      for (size_t a = 0; a < arrs.size(); ++a) {
        os << "array " << a << " "
           << *expr::op::bind::fname(arrs[a].array->first()) << "\n";
        for (size_t k = 0; k < arrs[a].reads.size(); ++k)
          os << "read " << a << " " << k << " " << *arrs[a].reads[k].index
             << "\n";
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
      query = xlw->lower(query);
      lap("xrt-lower");
    }
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
      if (sem.seam()) {
        // the assumption the answer depends on, always visible
        if (inb)
          os << "seam.inb: assumed accesses=" << sem.inbAccessCount()
             << " conditions=" << sem.inbConditionCount() << "\n";
        else
          os << "seam.inb: off" << note << "\n";
      }
      for (const auto &[k, v] : fromBitsLeft)
        os << "seam.ptr.from-bits-left." << k << ": " << v << "\n";
      for (const auto &[k, v] : aliasOpen)
        os << "seam.alias.open." << k << ": " << v << "\n";
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
        if (key == "seam.inb") {
          auto [state, more] = value.split(' ');
          brunchStat(os, key, state);
          SmallVector<StringRef> kvs;
          more.split(kvs, ' ', -1, /*KeepEmpty=*/false);
          for (StringRef kv : kvs) {
            auto [k, v] = kv.split('=');
            brunchStat(os, "seam.inb-" + k, v);
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
      std::unique_ptr<sea::solver::Solver> cs;
      unsigned ms = seamCheckTimeout * 1000;
      if (backend == Backend::Z3) {
        auto z = std::make_unique<sea::solver::z3_solver_impl>(efac);
        if (ms) {
          sea::ZParams<sea::EZ3> params(z->get_context());
          params.set("timeout", ms);
          z->get_solver().set(params);
        }
        cs = std::move(z);
      } else if (backend == Backend::Bitwuzla) {
#ifdef WITH_BITWUZLA
        sea::solver::bitwuzla_solver_impl::solver_options o;
        if (ms)
          o["time-limit-per"] = std::to_string(ms);
        cs = std::make_unique<sea::solver::bitwuzla_solver_impl>(efac, nullptr,
                                                                 o);
#endif
      } else {
        if (ms)
          sea::diag::warn("seam.check-no-timeout",
                          "--seam-check-timeout is not supported for Yices: "
                          "its checks run without a time limit");
        cs = makeSolver();
      }
      if (!cs)
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
          cs->push();
          sea::solver::SolverResult pr =
              cs->add(prem) ? cs->check() : sea::solver::SolverResult::UNKNOWN;
          cs->pop();
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
        cs->push();
        sea::solver::SolverResult res =
            cs->add(q) ? cs->check() : sea::solver::SolverResult::UNKNOWN;
        cs->pop();
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
      if (gOut->jsonl()) {
        // the query is one record
        sea::out::Record rec("smt2");
        raw_string_ostream qs(rec.text);
        solver->to_smt_lib(qs);
        if (StringRef(rec.text).ends_with("\n"))
          rec.text.pop_back();
        gOut->emit(rec);
      } else {
        solver->to_smt_lib(outs());
      }
      gOut->flush();
      statOut() << "vc.ptr-mode: " << (addressMode ? "address" : "opaque")
                << "\nvc.int-to-ptr: " << intToPtr << "\n"
                << "vc.ptr-from-int-opaque: " << sem.ptrFromIntOpaque() << "\n";
      if (sinkAssumes)
        statOut() << "vc.sink-assumes: " << sunkAssumes << "\n";
      companionLine(statOut());
      printSeamStats(statOut());
      runSeamCheck();
      runEqAudit();
      runXrtCheck();
      return 0;
    }

    auto emitResult = [&](StringRef v) {
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
    companionLine(vout());
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
    if (vcgenOnly) {
      // the VC is generated and checked; no solver (and none of the checks
      // that run one: --seam-check, --eq-audit, --x-read-table-check)
      emitResult("none");
      brunchStat(vout(), "result", "none");
      return 0;
    }
    runSeamCheck();
    runEqAudit();
    runXrtCheck();
    phase("solve");
    auto solveStart = std::chrono::steady_clock::now();
    sea::solver::SolverResult res = solver->check();
    const double solveSec = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - solveStart)
                                .count();
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
    phase("replay");
    sea::ReplayResult replay =
        sea::replayAndValidate(program, replayModel, sem, ctx);
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
    // memory-safety error. Discharging A-INB is the bounds-check job's work.
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
      sea::RunResult abs = sea::interpret(absProg, inputs);
      if (sea::isMemorySafetyError(abs)) {
        vout() << "seam.inb-violation: " << abs.message << "\n";
        if (abs.at)
          vout() << "seam.inb-violation-at: " << abs.at->getLoc() << "\n";
        gOut->flush();
        sea::diag::error(
            "vc.seam-inb-spurious",
            "sat, but the counterexample does not replay: the program "
            "accesses memory outside an object with these inputs, so A-INB "
            "(assumed by --seam) does not hold; the answer is inconclusive "
            "and the program has a memory-safety error (run the bounds-check "
            "job, or --seam-inb=0)",
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
    return 0;
  };
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
  if (rc == kRetryWithoutInb)
    rc = runVc(false, false, " (fallback: an access leaves its object)");
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
