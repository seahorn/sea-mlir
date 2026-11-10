// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// sea-seam-export: the SeaM terms of one straight-line sea.program as JSON,
// for the Lean conformance checks of seam-lean (docs/SeamExport.md).
//
// It runs production's SeaOpSem (encoding and rewriter) in one of a fixed set
// of lanes (configurations), and serializes: the raw (pointer-sorted) term of
// every integer / address / memory register, the side conditions, the
// sources, each load's operands, the same scalar roots and side conditions
// after production's solver boundary (SeaOpSem::solverTerm), and, with
// --firings, every rule firing of the rewriter with its premise (the trail of
// sea-vc --seam-trail / --seam-check). It computes nothing itself: the terms
// are printed as built, and no rule is re-implemented. No solver is invoked.
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "sea/Dialect/Sea/SeaDialect.h"
#include "sea/Expr/RewriteTrail.hh"
#include "sea/Expr/SeaMRules.hh"
#include "sea/Expr/TypeChecker.hh"
#include "sea/Support/Diag.h"
#include "sea/Support/Log.h"
#include "sea/Support/Version.h"
#include "sea/VC/OpSem.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include <cstdlib>
#include <map>
#include <sstream>
using namespace mlir;
using namespace expr;
using namespace expr::op;

namespace {
/// A lane: one configuration of SeaOpSem. `seaVc` is the sea-vc command
/// line whose VC terms the lane exports (empty: no sea-vc run builds them).
struct Lane {
  const char *name;
  const char *seaVc;
  const char *what;
  bool seam, wide, inb, simplify, memoryRules, rules, layout;
  unsigned pullConcat = 0; ///< SeaMOptions::pullConcatIte
};
// Each sea-vc flag set below sets exactly these SeaMOptions fields
// (sea-vc.cpp, runVc); every other field is at its SeaMOptions default in
// both (sea-vc's other --seam-* flags default to the same values).
const Lane kLanes[] = {
    {"memory-rules0", "",
     "the --seam --seam-wide encoding with the memory rules, simplification, "
     "the layout decision and A-INB off; the pointer rules run (default)",
     true, true, false, false, false, true, false},
    {"seam-wide-inb0-simplify0-layout0",
     "sea-vc --seam --seam-wide --seam-inb=0 --seam-simplify=0 --seam-layout=0",
     "SeaM with the memory rules, no A-INB, no simplification (--rewrite)",
     true, true, false, false, true, true, false},
    {"raw-wide", "",
     "the --seam --seam-wide encoding before any rule: terms as OpSem builds "
     "them",
     true, true, false, false, false, false, false},
    {"raw", "", "the --seam encoding (byte accesses) before any rule", true,
     false, false, false, false, false, false},
    {"seam-wide-inb0", "sea-vc --seam --seam-wide --seam-inb=0",
     "SeaM, wide accesses, no A-INB, simplification on", true, true, false,
     true, true, true, true},
    {"seam-inb0-pull-concat", "sea-vc --seam --seam-inb=0 --seam-pull-concat=4",
     "seam-inb0 with split (byte) pointer loads resolved: at most 4 pulls "
     "per load (ptr.load-resolve-concat)",
     true, false, false, true, true, true, true, 4},
    {"seam-inb0", "sea-vc --seam --seam-inb=0",
     "SeaM, byte accesses, no A-INB, simplification on", true, false, false,
     true, true, true, true},
    {"seam-wide", "sea-vc --seam --seam-wide",
     "SeaM, wide accesses, under A-INB (ainb: true)", true, true, true, true,
     true, true, true},
    {"seam", "sea-vc --seam",
     "SeaM as sea-vc --seam runs it, under A-INB (ainb: true)", true, false,
     true, true, true, true, true},
    {"plain", "sea-vc",
     "no SeaM, no rewriter: the boundary is PtrIndexExpander's arithmetic",
     false, false, false, false, false, false, false},
    {"simplify", "sea-vc --simplify",
     "no SeaM; the bit-vector / Boolean normalization rules only", false, false,
     false, true, false, true, false},
};
} // namespace

static llvm::cl::opt<std::string> input(llvm::cl::Positional,
                                        llvm::cl::desc("<program.mlir>"),
                                        llvm::cl::Optional);
static llvm::cl::opt<std::string>
    laneOpt("lane",
            llvm::cl::desc("The configuration to export (see --agent; "
                           "default memory-rules0)"),
            llvm::cl::init(""));
static llvm::cl::opt<bool> rewriteEnabled(
    "rewrite",
    llvm::cl::desc("Same as --lane=seam-wide-inb0-simplify0-layout0 (kept "
                   "for existing callers)"),
    llvm::cl::init(false));
static llvm::cl::opt<bool>
    firingsOpt("firings",
               llvm::cl::desc("Also export every rule firing (lhs, rhs, "
                              "rules, premise kinds and premise term)"),
               llvm::cl::init(false));
static llvm::cl::opt<bool>
    agent("agent", llvm::cl::desc("Print a terse guide for agents and exit"),
          llvm::cl::init(false));
static llvm::cl::opt<unsigned>
    addressBits("address-bits",
                llvm::cl::desc("Address width of the program (default 8)"),
                llvm::cl::init(8));

static const char *const agentGuide =
    R"(sea-seam-export agent guide (plain, terse)

WHAT: exports the SeaM terms of one straight-line sea.program as one JSON
document: the raw pointer-sorted term of every register, the same scalars
after production's solver boundary (SeaOpSem::solverTerm), and optionally
every rule firing with its premise. Production code builds every term
(SeaOpSem, SeaMRewriter); the tool only prints them. No solver runs.

WHY: the input of the Lean conformance checks of seam-lean
(github.com/agurfinkel/seam-lean): concrete runs compare sea-run against
Lean's evaluation of these terms (raw and erased), and a witness checker
checks each firing `premise -> lhs = rhs` against the proved rules.
Format and lanes: docs/SeamExport.md.

TYPICAL:
  sea-seam-export p.mlir --address-bits=8                  # lane memory-rules0
  sea-seam-export p.mlir --lane=raw-wide                   # before any rule
  sea-seam-export p.mlir --lane=seam-wide-inb0 --firings   # with witnesses
  sea-seam-export p.mlir --lane=plain                      # no-SeaM boundary

INPUT: a module with exactly one sea.program: one block, no block
arguments, ending in sea.halt, placed (sea.segment, !sea.addr<N>), N =
--address-bits. Taken as given: no lowering (sea-vc --lower would run).
Memories must be byte memories indexed by addresses of width N.
Pointer mode: address if the program has sea.addr.from_int, else opaque
(as sea-vc --ptr-mode=auto decides it).

LANES (--lane=<name>; then the sea-vc command whose VC terms the lane
builds, the "sea_vc" key; "-": no sea-vc run builds them):
  memory-rules0 (default)  -  SeaM wide encoding, memory rules /
        simplification / layout decision / A-INB off; pointer rules run
  seam-wide-inb0-simplify0-layout0 (= --rewrite)
        sea-vc --seam --seam-wide --seam-inb=0 --seam-simplify=0 --seam-layout=0
  raw-wide   -  the --seam --seam-wide encoding before any rule
  raw        -  the --seam encoding (byte accesses) before any rule
  seam-wide-inb0   sea-vc --seam --seam-wide --seam-inb=0
  seam-inb0        sea-vc --seam --seam-inb=0
  seam-inb0-pull-concat
        sea-vc --seam --seam-inb=0 --seam-pull-concat=4 (split pointer
        loads resolved; the lane where ptr.load-resolve-concat can fire)
  seam-wide        sea-vc --seam --seam-wide   (A-INB: "ainb": true)
  seam             sea-vc --seam               (A-INB: "ainb": true)
  plain            sea-vc (no --seam): boundary = PtrIndexExpander
  simplify         sea-vc --simplify (bit-vector rules only)
Lanes without wide accesses (raw, seam-inb0, seam-inb0-pull-concat, seam,
plain, simplify) refuse
a multi-byte access (vc.wide-access): sea-vc --lower would split it.
Lanes with simplification also normalize the roots, probes and side
conditions with SeaOpSem::simplify, as sea-vc normalizes its query.
An A-INB lane whose program statically violates A-INB is an error (sea-vc
would rerun without A-INB).

OPTIONS: --lane=<name>, --rewrite (old name of the second lane),
--firings (rule firings; needs a lane with rules), --address-bits=<n>
(default 8), --version, --help.

OUTPUT (stdout, one JSON document, schema 2; keys sorted):
  schema 2 | lane | sea_vc (command, "" if none) | ptr_mode address|opaque
  address_bits | ainb (A-INB used) | rewrite (memory rules on)
  roots: [{name, location, operation, term}]  every iN / addr / memory result
  side: [term]          side conditions (layout equations obj = base, ...)
  sources: [{site, term} | {fixed, term}]  nondet / mem.nondet / memhavoc
                        by site; sea.segment with its placed base
  probes: [{location, bytes, memory, address}]  each load's operands
  erased_roots: [{name, location, term}], erased_side: [term]
                        the same after solverTerm (no pointer sort left)
  firings (--firings): [{id, rules, premises, op, [loc], [units], lhs, rhs,
                        premise} | {id, rules, premises, op, unexpressible}]
  firings_unexpressible (--firings): count
Terms: {sort: bv|bool|ptr|memory, [bits], op, args, ...}; ops and their
fields are listed in docs/SeamExport.md. Additions are new keys / ops;
an incompatible change bumps schema.

FIRINGS (--firings; the rewrite trail, include/sea/Expr/RewriteTrail.hh):
one record per rule call that changed its term, in order. rules: the rule
and the decisions it rests on (row.store-skip,cmp.diff-base); premises:
kinds used (A-INB, LAYOUT, ALIGN, PTR-INDEX); op: load|store|addr (the
VC operation; loc, units = access size) or side|query (the final
normalization); lhs (kids already rewritten) and rhs as raw terms;
premise: SeaOpSem::firingPremise, the statement sea-vc --seam-check
checks (`premise -> lhs = rhs`, there after the boundary): true, or the
layout (addr(obj) = base) and the in-bounds conditions the rule relies
on. Pointers compare by address, memories cell by cell. A firing whose
premise or terms cannot be expressed has `unexpressible: <why>` instead
(warning export.firing-unexpressible) and makes the status 3.

STATUS CODES: 0 exported; 1 error (bad input, unsupported construct:
an operator, sort or width the format has no node for -> `Error: ...
[validation.export]` on stderr, nothing on stdout); 3 exported, but some
firings are unexpressible (their records say why; a witness check must not
count them).
)";

static std::string printed(Expr e) {
  std::ostringstream s;
  s << *e;
  return s.str();
}
[[noreturn]] static void fail(const std::string &message) {
  sea::diag::error("validation.export", message);
  std::exit(1);
}
static std::string numeral(Expr e) { return printed(e->arg(0)); }

struct Exporter {
  TypeChecker tc;
  bool erased = false;
  /// the first unsupported construct of the current term (empty: none)
  std::string err;
  std::map<std::string, Expr> symbolSorts;
  void bad(const std::string &message) {
    if (err.empty())
      err = message;
  }
  /// \p e as a term; on an unsupported construct, `fail`s
  llvm::json::Value term(Expr e) {
    llvm::json::Value v = termOrNull(e);
    if (!err.empty())
      fail(err);
    return v;
  }
  /// \p e as a term, or null with `err` set
  llvm::json::Value termOrNull(Expr e) {
    err.clear();
    llvm::json::Value v = rec(e);
    return err.empty() ? std::move(v) : llvm::json::Value(nullptr);
  }
  llvm::json::Value rec(Expr e) {
    if (!err.empty())
      return nullptr;
    llvm::json::Object o;
    Expr ty = tc.typeOf(e);
    if (isOpX<BVSORT>(ty)) {
      o["sort"] = "bv";
      o["bits"] = bv::width(ty);
    } else if (isOpX<PTRSORT>(ty)) {
      if (erased || ptr::width(ty) != addressBits)
        bad("unexpected pointer sort");
      o["sort"] = "ptr";
      o["bits"] = ptr::width(ty);
    } else if (isOpX<BOOL_TY>(ty))
      o["sort"] = "bool";
    else if (isOpX<ARRAY_TY>(ty)) {
      if (!(erased ? isOpX<BVSORT>(ty->arg(0)) : isOpX<PTRSORT>(ty->arg(0))) ||
          !isOpX<BVSORT>(ty->arg(1)) ||
          ptr::addrSortWidth(ty->arg(0)) != addressBits ||
          bv::width(ty->arg(1)) != 8)
        bad("only configured-address-width byte arrays supported");
      o["sort"] = "memory";
    } else
      bad("unsupported sort: " + printed(ty));
    if (!err.empty())
      return nullptr;
    if (bv::is_bvnum(e)) {
      o["op"] = "bv";
      o["value"] = numeral(e);
      return o;
    }
    if (isOpX<TRUE>(e) || isOpX<FALSE>(e)) {
      o["op"] = "bool";
      o["value"] = isOpX<TRUE>(e);
      return o;
    }
    if (bind::isFapp(e) && e->arity() == 1) {
      std::string name = printed(bind::fname(e->arg(0)));
      auto [it, added] = symbolSorts.emplace(name, ty);
      if (!added && it->second != ty)
        bad("symbol name used at different sorts: " + name);
      o["op"] = "var";
      o["name"] = name;
      return o;
    }
    llvm::json::Array args;
    auto children = [&](const char *name) {
      o["op"] = name;
      for (unsigned i = 0; i < e->arity(); ++i)
        args.push_back(rec(e->arg(i)));
    };
#define NODE(OP, NAME)                                                         \
  if (isOpX<OP>(e)) {                                                          \
    children(NAME);                                                            \
  }
    NODE(ITE, "ite")
    else NODE(ADDR, "addr") else NODE(PTR_TO_INT, "toInt") else NODE(PTR_TO_BITS, "toBits") else NODE(
        PTR_FROM_INT,
        "fromInt") else NODE(PTR_FROM_BITS,
                             "fromBits") else NODE(PTR_DIFF,
                                                   "diff") else NODE(PTR_EQ,
                                                                     "ptrE"
                                                                     "q") else if (isOpX<IS_OVERLAP>(e)) {
      if (!ptr::isUnitOverlap(e))
        bad("only unit overlap supported");
      o["op"] = "unitOverlap";
      args.push_back(rec(e->arg(0)));
      args.push_back(rec(e->arg(2)));
    }
    else if (ptr::isPtrIndex(e)) {
      o["op"] = "index";
      o["flag"] = isOpX<PTR_INDEX_NUSW>(e)  ? "nusw"
                  : isOpX<PTR_INDEX_NUW>(e) ? "nuw"
                                            : "plain";
      args.push_back(rec(ptr::base(e)));
      llvm::json::Array indices;
      for (size_t k = 0; k < ptr::numIndices(e); ++k)
        indices.push_back(
            llvm::json::Object{{"stride", ptr::stride(e, k).to_string()},
                               {"value", rec(ptr::index(e, k))}});
      o["indices"] = std::move(indices);
      o["constant"] = ptr::constant(e).to_string();
      o["byte_offset"] = getTerm<mpz_class>(e->arg(e->arity() - 1)).to_string();
    }
    else if (isOpX<OFFSET>(e)) {
      o["op"] = "offset";
      llvm::json::Array indices;
      for (size_t k = 0; k < ptr::offsetIndices(e); ++k)
        indices.push_back(
            llvm::json::Object{{"stride", ptr::offsetStride(e, k).to_string()},
                               {"value", rec(ptr::offsetIndex(e, k))}});
      o["indices"] = std::move(indices);
      o["constant"] = getTerm<mpz_class>(e->arg(e->arity() - 2)).to_string();
      o["byte_offset"] = getTerm<mpz_class>(e->arg(e->arity() - 1)).to_string();
    }
    else NODE(EQ, "eq") else NODE(BULT, "ult") else NODE(BULE, "ule") else NODE(BADD, "add") else NODE(BSUB, "sub") else NODE(BMUL, "mul") else NODE(BCONCAT, "concat") else NODE(
        SELECT,
        "select") else NODE(STORE,
                            "store") else NODE(MEM_SET,
                                               "mset") else NODE(MEM_COPY,
                                                                 "mcop"
                                                                 "y") else NODE(MEM_HAVOC,
                                                                                "mhavoc") else NODE(NEG,
                                                                                                    "not") else NODE(AND,
                                                                                                                     "and") else NODE(OR,
                                                                                                                                      "or") else if (isOpX<CONST_ARRAY>(e)) {
      o["op"] = "constMem";
      args.push_back(rec(e->arg(1)));
    }
    else if (isOpX<BEXTRACT>(e)) {
      o["op"] = "extract";
      o["start"] = bv::low(e);
      o["length"] = bv::high(e) - bv::low(e) + 1;
      args.push_back(rec(e->arg(2)));
    }
    else if (isOpX<W_SELECT>(e) || isOpX<W_STORE>(e)) {
      bool read = isOpX<W_SELECT>(e);
      o["op"] = read ? "wselect" : "wstore";
      unsigned bits =
          read ? bv::width(e->arg(2)) : bv::width(tc.typeOf(e->arg(2)));
      if (bits % 8 || bits == 0)
        bad("invalid wide width");
      o["bytes"] = bits / 8;
      o["alignment"] = (int64_t)array::promisedAlign(e);
      args.push_back(rec(e->arg(0)));
      args.push_back(rec(e->arg(1)));
      if (!read)
        args.push_back(rec(e->arg(2)));
    }
    else if (isOpX<STORE_MAP>(e)) {
      o["op"] = "smap";
      args.push_back(rec(e->arg(0)));
      args.push_back(rec(e->arg(1)));
      llvm::json::Array entries;
      Expr list = e->arg(2);
      while (err.empty() && isOpX<SMAP_CONS>(list)) {
        if (!bv::is_bvnum(list->arg(0))) {
          bad("non-numeral map offset");
          break;
        }
        entries.push_back(llvm::json::Object{{"offset", numeral(list->arg(0))},
                                             {"value", rec(list->arg(1))}});
        list = list->arg(2);
      }
      if (err.empty() && !isOpX<SMAP_NIL>(list))
        bad("malformed map list");
      o["entries"] = std::move(entries);
    }
    else bad("unsupported expression: " + printed(e));
#undef NODE
    if (!err.empty())
      return nullptr;
    o["args"] = std::move(args);
    return o;
  }
};

static std::string locString(Operation &op) {
  std::string loc;
  llvm::raw_string_ostream ls(loc);
  op.getLoc().print(ls);
  return loc;
}

int main(int argc, char **argv) {
  llvm::InitLLVM init(argc, argv);
  struct Summary {
    ~Summary() { sea::diag::printSummary(); }
  } summary;
  sea::log::init(); // SEA_LOG / SEA_LOG_FILE, before --log
  sea::setVersionPrinter("sea-seam-export");
  llvm::cl::ParseCommandLineOptions(
      argc, argv,
      "Export the SeaM terms of a straight-line sea.program as JSON "
      "(docs/SeamExport.md)\n");
  if (agent) {
    llvm::outs() << agentGuide;
    return 0;
  }
  if (input.empty())
    fail("no input program (sea-seam-export <program.mlir>; --agent)");

  const Lane *lane = &kLanes[0];
  if (rewriteEnabled && !laneOpt.empty())
    fail("--rewrite is --lane=seam-wide-inb0-simplify0-layout0: not with "
         "--lane");
  std::string laneName =
      rewriteEnabled ? std::string(kLanes[1].name) : laneOpt.getValue();
  if (!laneName.empty()) {
    lane = nullptr;
    for (const Lane &l : kLanes)
      if (laneName == l.name)
        lane = &l;
    if (!lane) {
      std::string names;
      for (const Lane &l : kLanes)
        names += std::string(names.empty() ? "" : ", ") + l.name;
      fail("unknown lane '" + laneName + "' (lanes: " + names + ")");
    }
  }
  const bool hasRewriter = lane->seam || lane->simplify;
  if (firingsOpt && !(hasRewriter && lane->rules))
    fail(std::string("--firings needs a lane with rules (lane ") + lane->name +
         " has none)");

  MLIRContext c;
  c.getOrLoadDialect<sea::SeaDialect>();
  auto m = parseSourceFile<ModuleOp>(input, ParserConfig(&c));
  if (!m || failed(verify(*m)))
    return 1;
  auto programs = m->getOps<sea::ProgramOp>();
  if (std::distance(programs.begin(), programs.end()) != 1)
    fail("exactly one program required");
  auto p = *programs.begin();
  if (!llvm::hasSingleElement(p.getBody()) ||
      p.getBody().front().getNumArguments())
    fail("single block without arguments required");
  if (!isa<sea::HaltOp>(p.getBody().front().getTerminator()))
    fail("straight-line program must end in sea.halt");
  Block &body = p.getBody().front();

  ExprFactory ef;
  sea::ValueNames names(p);
  expr::SeaMOptions opts;
  opts.assumeInBounds = lane->inb;
  opts.memoryRules = lane->memoryRules;
  opts.simplify = lane->simplify;
  opts.wideAccess = lane->wide;
  opts.layoutDecide = lane->layout;
  opts.rules = lane->rules;
  opts.pullConcatIte = lane->pullConcat;
  sea::SeaOpSem sem(ef, names, false, lane->seam, &opts);
  bool addressMode = false;
  for (Operation &op : body)
    if (isa<sea::AddrFromIntOp>(op))
      addressMode = true;
  sem.setAddressMode(addressMode);
  expr::RewriteTrail trail;
  if (firingsOpt) {
    trail.setKeep(true);
    trail.beginRun(lane->inb);
    sem.setTrail(&trail);
  }
  sea::SymStore store(ef);
  ExprVector side;
  sea::OpSemContext ctx(store, side);
  if (failed(sem.exec(body, ctx)))
    return 1;
  if (lane->inb && sem.inbStaticViolationCount())
    fail("the program statically violates A-INB (sea-vc would rerun "
         "without it): use an inb0 lane");
  // the final normalization, as sea-vc runs it: side conditions, then the
  // registers (the query); identity without simplification
  if (sem.simplifies()) {
    sem.setTrailContext("side");
    for (Expr &e : side)
      e = sem.simplify(e);
    sem.setTrailContext("query");
  }
  auto value = [&](Value v) { return sem.simplify(sem.lookup(v, ctx)); };

  Exporter ex, erasedEx;
  erasedEx.erased = true;
  llvm::json::Array roots, conditions, sources, probes, erasedRoots,
      erasedConditions;
  for (Operation &op : body)
    for (Value v : op.getResults()) {
      if (!isa<IntegerType>(v.getType()) && !isa<sea::MemType>(v.getType()) &&
          !isa<sea::AddrType>(v.getType()))
        continue;
      std::string loc = locString(op);
      Expr t = value(v);
      roots.push_back(
          llvm::json::Object{{"name", names.name(v).getValue().str()},
                             {"location", loc},
                             {"operation", op.getName().getStringRef().str()},
                             {"term", ex.term(t)}});
      if (!isa<sea::MemType>(v.getType()))
        erasedRoots.push_back(
            llvm::json::Object{{"name", names.name(v).getValue().str()},
                               {"location", loc},
                               {"term", erasedEx.term(sem.solverTerm(t))}});
    }
  for (Operation &op : body) {
    if (auto seg = dyn_cast<sea::SegmentOp>(op)) {
      sources.push_back(llvm::json::Object{
          {"fixed", numeral(sem.numeral(seg.getBase()))},
          {"term", ex.term(sem.lookup(op.getResult(0), ctx))}});
      continue;
    }
    Expr source;
    uint64_t site = 0;
    if (auto nd = dyn_cast<sea::NondetOp>(op)) {
      site = nd.getSite();
      source = sem.lookup(op.getResult(0), ctx);
    } else if (auto nd = dyn_cast<sea::MemNondetOp>(op)) {
      site = nd.getSite();
      source = sem.lookup(op.getResult(0), ctx);
    } else if (auto nd = dyn_cast<sea::MemHavocOp>(op)) {
      site = nd.getSite();
      source = sem.havocArray(nd);
    }
    if (source)
      sources.push_back(llvm::json::Object{{"site", (int64_t)site},
                                           {"term", ex.term(source)}});
  }
  for (Operation &op : body)
    if (isa<sea::LoadOp>(op)) {
      auto ty = dyn_cast<IntegerType>(op.getResult(0).getType());
      unsigned width = ty ? ty.getWidth()
                       : isa<sea::AddrType>(op.getResult(0).getType())
                           ? addressBits
                           : 0;
      if (!width || width % 8)
        fail("memory probes require whole-byte loads");
      probes.push_back(
          llvm::json::Object{{"location", locString(op)},
                             {"bytes", width / 8},
                             {"memory", ex.term(value(op.getOperand(0)))},
                             {"address", ex.term(value(op.getOperand(1)))}});
    }
  for (auto e : side) {
    conditions.push_back(ex.term(e));
    erasedConditions.push_back(erasedEx.term(sem.solverTerm(e)));
  }

  llvm::json::Object doc{{"schema", 2},
                         {"lane", lane->name},
                         {"sea_vc", lane->seaVc},
                         {"ptr_mode", addressMode ? "address" : "opaque"},
                         {"erased_roots", std::move(erasedRoots)},
                         {"erased_side", std::move(erasedConditions)},
                         {"address_bits", (int64_t)addressBits},
                         {"rewrite", lane->memoryRules && lane->rules},
                         {"probes", std::move(probes)},
                         {"ainb", lane->inb},
                         {"sources", std::move(sources)},
                         {"roots", std::move(roots)},
                         {"side", std::move(conditions)}};

  // the firings: each a witness `premise -> lhs = rhs` (sea-vc --seam-check
  // checks the same statement after the boundary). The premise is
  // SeaOpSem::firingPremise, the statement --seam-check builds.
  int status = 0;
  if (firingsOpt) {
    llvm::json::Array firings;
    uint64_t unexpressible = 0;
    for (const RewriteTrail::Firing &f : trail.kept()) {
      const RewriteTrail::Context &cx = trail.context(f.ctx);
      llvm::json::Array rules, premises;
      for (unsigned r : f.rules)
        rules.push_back(trail.ruleName(r).str());
      llvm::SmallVector<const char *, 4> pn;
      RewriteTrail::premiseNames(f.premises, pn);
      for (const char *n : pn)
        premises.push_back(n);
      llvm::json::Object rec{{"id", (int64_t)f.id},
                             {"rules", std::move(rules)},
                             {"premises", std::move(premises)},
                             {"op", cx.kind}};
      if (!cx.loc.empty())
        rec["loc"] = cx.loc;
      if (cx.units)
        rec["units"] = (int64_t)cx.units;
      std::string why;
      Expr prem = sem.firingPremise(trail, f, why);
      llvm::json::Value lhs = nullptr, rhs = nullptr, pt = nullptr;
      if (prem) {
        lhs = ex.termOrNull(f.lhs);
        if (ex.err.empty())
          rhs = ex.termOrNull(f.rhs);
        if (ex.err.empty())
          pt = ex.termOrNull(prem);
        if (!ex.err.empty())
          why = "the format has no node for it: " + ex.err;
        ex.err.clear();
      } else {
        why = "the premise cannot be stated: " + why;
      }
      if (!why.empty()) {
        ++unexpressible;
        sea::diag::warn("export.firing-unexpressible",
                        "firing " + std::to_string(f.id) + " (" +
                            trail.rulesOf(f) + ") is not exported: " + why);
        rec["unexpressible"] = why;
      } else {
        rec["lhs"] = std::move(lhs);
        rec["rhs"] = std::move(rhs);
        rec["premise"] = std::move(pt);
      }
      firings.push_back(std::move(rec));
    }
    doc["firings"] = std::move(firings);
    doc["firings_unexpressible"] = (int64_t)unexpressible;
    if (unexpressible)
      status = 3;
  }
  llvm::outs() << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(doc)));
  return status;
}
