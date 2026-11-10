// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- sea-run.cpp - run a sea program on concrete inputs -------*- C++ -*-===//
//
// Prints a line-oriented report on stdout:
//
//   program: @main
//   nondet: site=0 callee=nondet_u32 width=32 value=7 origin=input
//   status: assert-failed
//   at: loc("t.mlir":5:3)
//   steps: 9
//
// Exit code 0 when the run ended (halted, assert-failed, assume-blocked,
// step-limit), 4 on a memory-safety finding (memory-error, with
// --defer-memory-errors), 1 on an error.
//
//===----------------------------------------------------------------------===//

#include "sea/Dialect/Sea/SeaDialect.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Interp/Fuzz.h"
#include "sea/Interp/Interpreter.h"
#include "sea/Interp/ParseInput.h"
#include "sea/Support/Diag.h"
#include "sea/Support/Log.h"
#include "sea/Support/Output.h"
#include "sea/Support/Version.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using namespace llvm;

static cl::opt<std::string>
    inputFilename(cl::Positional, cl::desc("<input .mlir>"), cl::init("-"));

static cl::opt<std::string>
    programName("program", cl::desc("Program to run (default: the only one)"),
                cl::value_desc("name"));

static cl::list<std::string>
    inputs("input",
           cl::desc("Value of a nondet site (<site>=<value>) or of a memory "
                    "cell (<site>@<addr>=<value>)"),
           cl::value_desc("site[@addr]=value"), cl::CommaSeparated);

static cl::opt<sea::InputSource::Missing> missing(
    "missing", cl::desc("What a nondet site without --input gets"),
    cl::init(sea::InputSource::Missing::Error),
    cl::values(clEnumValN(sea::InputSource::Missing::Error, "error",
                          "stop with an error (default)"),
               clEnumValN(sea::InputSource::Missing::Zero, "zero", "zero"),
               clEnumValN(sea::InputSource::Missing::Random, "random",
                          "a random value (see --seed)")));

static cl::opt<uint64_t>
    seed("seed", cl::desc("Seed for --missing=random and --fuzz"), cl::init(0));

static cl::opt<uint64_t> maxSteps("max-steps", cl::desc("Step limit"),
                                  cl::init(1'000'000));

static cl::opt<bool> trace("trace", cl::desc("Print every computed value"),
                           cl::init(false));

static cl::opt<sea::PrintMode> printMode(
    "print", cl::desc("Output of the program's print ops (sea.print*)"),
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
    cl::desc("Convert the program's prints first (docs/design/Print.md)"),
    cl::init(sea::PrintRefs::AsIs),
    cl::values(clEnumValN(sea::PrintRefs::AsIs, "asis", "as written (default)"),
               clEnumValN(sea::PrintRefs::Weak, "weak",
                          "by name (sea-print-weaken)"),
               clEnumValN(sea::PrintRefs::Strong, "strong",
                          "SSA operands (sea-print-strengthen)")));

static cl::opt<sea::out::Format>
    outFormat("format", cl::desc("Output format of stdout"),
              cl::init(sea::out::Format::Text),
              cl::values(clEnumValN(sea::out::Format::Text, "text",
                                    "`key: value` lines (default)"),
                         clEnumValN(sea::out::Format::Jsonl, "jsonl",
                                    "one JSON object per line (see --agent)")));

static cl::opt<std::string>
    inputFile("input-file",
              cl::desc("Read inputs from a corpus file (one --input "
                       "assignment, or nondet: / memread: line, per line)"),
              cl::value_desc("file"));

static cl::opt<uint64_t>
    fuzzTrials("fuzz",
               cl::desc("Fuzz: run this many trials on generated inputs "
                        "(docs/reference/fuzzing.md)"),
               cl::value_desc("N"), cl::init(0));

static cl::opt<double> fuzzTime("fuzz-time",
                                cl::desc("Fuzz for at most this many seconds"),
                                cl::value_desc("S"), cl::init(0));

static cl::list<std::string>
    corpus("corpus",
           cl::desc("Seed entries for --fuzz: corpus files or "
                    "directories of them"),
           cl::value_desc("dir|file"), cl::CommaSeparated);

static cl::opt<std::string>
    fuzzOut("fuzz-out",
            cl::desc("Write failing and new-coverage inputs of --fuzz here"),
            cl::value_desc("dir"));

static cl::opt<bool>
    fuzzDict("fuzz-dict",
             cl::desc("Fuzz with the dictionary of the program's constants "
                      "(default on)"),
             cl::init(true));

static cl::opt<bool> keepGoing("keep-going",
                               cl::desc("Fuzz past the first failure, "
                                        "counting failures"),
                               cl::init(false));

static cl::opt<bool> deferMemoryErrors(
    "defer-memory-errors",
    cl::desc("Go on past a memory-safety violation (null, out of bounds, "
             "freed, bad free); it is a finding (status memory-error, exit 4) "
             "only if the run then reaches the end or a failing assert, not "
             "if an assume blocks it"),
    cl::init(false));

static cl::opt<bool> agent("agent",
                           cl::desc("Print a terse guide for agents and exit"),
                           cl::init(false));

static const char *const agentGuide = R"(sea-run agent guide (plain, terse)

WHAT: concrete interpreter for one sea.program. It is the golden semantics
of the sea dialect: values are computed by sea's op table (SMT-LIB
bit-vector semantics), so its answer is the reference for what a program
does on given nondet values.

WHY BEAT MANUAL: evaluating bit-vector ops by hand gets the edge cases
wrong (division by zero, srem sign, shifts >= width, wrap-around at the
width). Tracing block arguments through a CFG by hand is slow. Run it.

TYPICAL:
  sea-run p.mlir --input=0=5,1=-1           # nondet site 0 := 5, site 1 := -1
  sea-run p.mlir --input=2@16=7             # cell 16 of the memory at site 2 := 7
  sea-run p.mlir --missing=zero             # unlisted sites read 0
  sea-run p.mlir --format=jsonl             # one JSON object per line (jq)
  sea-run p.mlir --missing=random --seed=7  # unlisted sites random, reproducible
  sea-run p.mlir --trace                    # also print every computed value
  sea-run p.mlir --print=tree --print-locs  # the program's prints, indented
  sea-run p.mlir --print-refs=strong        # resolve weak prints to operands
  sea-run m.mlir --program=main             # pick one of several programs
  sea-run p.mlir --input-file=f.txt         # inputs from a corpus file
  sea-run p.mlir --fuzz=10000 --seed=1 --fuzz-out=out   # fuzz (see FUZZING)
  sea-run p.mlir --defer-memory-errors      # classify memory errors (DEFERRED)

INPUTS: a module with sea.program ops. --input values are decimal, negative
decimal, or 0x-hex, and must fit the nondet's width (a negative v means
2^w + v). The key is the site number in `sea.nondet "<callee>" [<site>]`;
for a memory cell, <site>@<addr> with the site of `sea.mem.nondet` or
`sea.memhavoc` (the cell's initial or havocked value), or
<site>@<object>+<offset> for abstract memories (objects are named as in the
segment map: stack.<site>, heap.<site>, global.<name>). Cells are asked
lazily, only when read; --missing applies to them too.

ABSTRACT PROGRAMS (!sea.ptr) run directly: pointers are (object, offset);
null, freed and out-of-bounds accesses, double free and a free of a
pointer that is not a heap object's start (memory safety) stop the run with
an error (--defer-memory-errors: see DEFERRED); ptr.to_int, ptr.from_int of
non-zero, and reading a pointer cell as an integer stop it too (no
addresses before placement: limits, never deferred).

DEFERRED (--defer-memory-errors; docs/reference/fuzzing.md): a run may
reach a memory-safety violation and later an assume that blocks it
(late assumptions: unified assumes, prophecy variables): infeasible, not a
bug. With the flag the run records the first violation, counts them, and
goes on: a load reads 0 (a pointer load: null), a store / memset / memmove
/ memhavoc with a violation does nothing, a bad free does nothing. These
substitutes pick one continuation and only decide whether a later assume
holds. Assumed: past the first violation any behaviour is allowed; a
finding = the violation is reached on an exact prefix and some continuation
reaches the end (blocked may hide a finding; none is made up). Not a VC
counterexample with array semantics past it: an out-of-bounds store is
dropped here, sea-vc's arrays read it back. At the end: assume-blocked (infeasible),
with `deferred:`; reaching sea.halt or a failing sea.assert: status
memory-error at the first violation (a failing assert after it is not
reported separately: its condition may depend on the substitutes), exit 4;
step-limit / error: not classified, with `deferred:`. Layout errors and
limits are never deferred.

MEMORY: executed in place. Every memory update must be marked `inplace`
and consume the current version of its chain, else status: error.
Loads may read earlier versions (they see the current contents).
Word layouts (`!sea.mem<..., iC, aligned|unaligned>`): a cell is k bytes;
an input cell is keyed by its start byte. aligned: an access off a word
boundary, or to an object aligned below k, is an error; unaligned: an
access overlapping a cell that starts elsewhere is an error (accesses must
match). Range-op lengths must be whole cells.

OUTPUT (stdout, one `key: value` per line, stable order):
  program: @<name>
  nondet: site=<n> callee=<f> width=<w> value=<unsigned> origin=input|zero|random
  memread: site=<n> addr=<a> width=<w> value=<unsigned> origin=...
          [obj=&<o>[<off>]]                (each cell read from a source;
                                           obj: the placed object holding a
                                           concrete address)
  value: <op> <loc> : <type> = <unsigned>  (only with --trace; block
                                           arguments print as block-arg;
                                           an abstract pointer prints its
                                           token, &heap.3[8]; an address
                                           its number and ptr=<token>)
  print: <scope>.<tag> = <value>          (the program's print ops, in
                                           execution order; see PRINTS)
  print.object: <obj> kind=<k> size=<n> [label=..] [at=..]
                [inlined-at=..] [at_name=..] [base=..]
  print.note: <unmatched sea.print.end>
  print.truncated: <records dropped by --print-limit>
  status: halted|assert-failed|assume-blocked|step-limit|memory-error|error
  at: <loc of the op that ended the run>   (a failing assert: the
                                    original assert of a sea-merge-asserts
                                    program, its whole MLIR location)
  assert-at: <where>                (assert-failed; see LOCATIONS: where
                                    the failing condition is computed. A
                                    computed condition: its defining op,
                                    or through a chain of sea.and the first
                                    false conjunct's. A constant or
                                    block-argument condition (SeaHorn's one
                                    `assert false`): the last executed op
                                    before the assert with a file location
                                    (a branch, or the condition; source
                                    locations over named ones over .mlir
                                    positions; print ops never), which
                                    names the C check that failed. Only
                                    the program can name the reach site
                                    of an `assert false`: an exporter
                                    that leaves the error branch and the
                                    assert unlocated leaves the last op
                                    before them, maybe inside an inlined
                                    callee)
  assert-op: <where>                (assert-failed; see LOCATIONS: the
                                    sea.assert's own; the original one
                                    under sea-merge-asserts)
  assert-path: <loc> <- <loc> ...   (assert-failed: up to 5 distinct
                                    locations of executed branches, most
                                    recent first; each <loc> is
                                    <file:line:col>[ inlined-at=..], see
                                    LOCATIONS. In a program with source
                                    locations only those: a sea.cond_br
                                    without one shows its condition's,
                                    else that of the last op before it
                                    in its block; else, like an
                                    unlocated sea.br, it is left out)
  reached: halted|assert-failed at=<where>
                                    (memory-error: the op that made the
                                    violation reachable; at: is the
                                    violation)
  message: <why, for error and memory-error>
  deferred: <n> kind=null|out-of-bounds|use-after-free|double-free|bad-free
            at=<where>              (--defer-memory-errors, a run that went
                                    past n violations: the first one)
  steps: <ops executed>
Values print as unsigned decimal. assume-blocked means the run is
infeasible (not a bug); assert-failed means the run reached a failing
sea.assert (a bug if the inputs are feasible).

PRINTS (docs/design/Print.md): sea.print "t" = "x" : i64 shows the value
named x (loc("x"), or nd.<site>) most recently defined on the run; none
prints `<dangling: x>`. sea.print "t" %x : i64 (strong) reads its operand.
sea.print.mem reads memory without affecting the run (an unread cell is
peeked: `??` when no --input gives it).
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
Pointers print as object + offset (&heap.3[8], `end`, `oob`, `freed`,
null), also after placement (provenance through address ops and memory);
an address without provenance is a number with a `~obj[off]` hint.
--print=lines|tree|off, --print-limit=N (default 1000), --print-locs
(at=<file:line:col> [inlined-at=..] [at_name=<name>] of each print op).

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

OUTPUT FORMATS (--format=text|jsonl; default text, the lines above):
jsonl: every stdout line is one JSON object
  {"kind":"<kind>", <fields>..., "text":"<the text line(s)>"}
`text` is the text format's rendering, so `jq -r .text` gives the text
output (timings aside). Integers are JSON numbers when below 2^53, else
decimal strings; field order is stable. Diagnostics stay on stderr as text
(no diag records). --agent prints this guide as text whatever --format.
Objects: one id in every record (stack.1, heap.3, global.g):
print.object.id, and obj in print / print.mem / memread; join on it.
Addresses (base, addr) are numbers.
Kinds and fields:
  program       name
  nondet        site, callee, width, value, origin
  memread       site, addr | obj + off (abstract memory: no address),
                width, value, origin, [obj + off] (a placed cell in an
                object; text obj=&<obj>[<off>])
  value         (--trace) def (op name | block-arg), loc, type, [value],
                [ptr] (the pointer token)
  status        status, [loc] (at:), [at] [at_inlined] [at_name]
                (assert-at), [op] [op_inlined] [op_name] (assert-op),
                [path] (array) [path_inlined], [reached] [reached_at]
                [reached_at_inlined] [reached_at_name], [message],
                [deferred] [deferred_kind] [deferred_at]
                [deferred_at_inlined] [deferred_at_name]; one
                record for the status / at / assert-* / reached /
                message / deferred lines (text has several lines)
  stat          key, value: `steps`; the fuzz.* lines (key, value and
                their k=v fields)
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
  sea-run p.mlir --format=jsonl | jq -r 'select(.kind=="status").status'
  ... | jq -c 'select(.kind=="print") | {path, obj, off, value}'

FUZZING (docs/reference/fuzzing.md): --fuzz=N runs N trials in-process,
--fuzz-time=S at most S seconds (both: whichever ends first). A trial runs
the program on generated nondet values: a corpus entry (or none) mutated
per site and cell (fresh random, bit flip, +-1, 0/1/-1/min/max of the
width, dictionary); sites the entry does not set get random or dictionary
values. The dictionary is the program's integer constants used by
comparisons and arithmetic, +-1, sized to the site's width
(--fuzz-dict=false: none). A site has one value per trial, as with
--input. An input that reaches a new block, edge or passed sea.assume
joins the corpus. The fuzzer stops at the first failure: an assert
failure, or with --defer-memory-errors a memory-safety finding (status
memory-error) (--keep-going: counts them instead). A run that stops with
status: error is recorded per op and never stops it: a memory-safety
check (fuzz.memory-check; without --defer-memory-errors, so feasibility is
unknown) or a limit (fuzz.limit: an interpreter limit, unsupported op, an
access off a word of a word-layout memory; never a finding; warning
fuzz.limit once). A broken `align` promise is a warning,
interp.align-promise, not an error.
Same --seed and corpus = same run (--fuzz-time: best effort, the trial
count depends on speed).
  --corpus=DIR|FILE,...  seed entries, run first, in order (directory:
                         its files by name)
  --fuzz-out=DIR         (new or empty) writes failure-<trial>.txt /
                         memory-check-<trial>.txt / limit-<trial>.txt
                         (the first trial of each failing / erring op)
                         and cov-<trial>.txt (new coverage), as corpus
                         files
  --input-file=F         replays one corpus file (also without --fuzz)
Corpus file: one assignment per line, `<site>=<v>`, `<site>@<addr>=<v>`,
`<site>@<object>+<offset>=<v>` (the --input syntax), or a `nondet:` /
`memread:` line of a sea-run or sea-vc output (site, addr, value fields);
`#` comments and blank lines are skipped, other `key: ...` lines (the rest
of a saved run) are skipped and counted. A saved run is a seed.
Output of --fuzz: the first failure as a plain run on its input
(the lines of OUTPUT; what --input-file=<its file> [--defer-memory-errors]
prints; the fuzzer checks the replay ends the same way), then stat lines:
  fuzz.failure: <runs> trial=<first> [file=<path>] at=<where>
                                 (per failing sea.assert)
  fuzz.memory-safety: <runs> trial=<first> [file=<path>] op=<op>
                      [site=<n>] at=<where>
  fuzz.memory-safety-message: <message of the first>
                                 (--defer-memory-errors: per op with a
                                 memory-safety finding)
  fuzz.memory-check: <runs> trial=<first> [file=<path>] op=<op> [site=<n>]
                     at=<where>
  fuzz.memory-check-message: <message of the first>
                                 (per op that stopped a run at a
                                 memory-safety check; site: the op's site,
                                 if it has one)
  fuzz.limit: <runs> trial=<first> [file=<path>] op=<op> [site=<n>]
              at=<where>
  fuzz.limit-message: <message of the first>
                                 (per op that stopped a run at a limit or
                                 a layout error)
  fuzz.trials: <n> seeds=<n> time=<s> execs/s=<n>
  fuzz.status: halted=<n> assert-failed=<n> assume-blocked=<n>
               step-limit=<n> error=<n> [memory-error=<n>]
  fuzz.deferred: <runs> memory-error=<n> assume-blocked=<n> step-limit=<n>
                 error=<n>       (--defer-memory-errors: runs that went
                                 past a violation, by final status)
  fuzz.coverage: blocks=<n> blocks-total=<n> edges=<n> assumes=<n>
                 assumes-total=<n>  (edges include the entry; assumes:
                                     sea.assume ops that held at least once)
  fuzz.corpus: <entries> seeds=<files> skipped-lines=<n> files=<written>
               dictionary=<n>
  fuzz.blocked: <runs> op=sea.assume|sea.unreachable at=<where>
                                 (each blocking site, most runs first)
<where> is a location as in LOCATIONS. Logs: --log=fuzz.run (start, end),
--log=fuzz.run:2 (each trial that reached new coverage).

DIAGNOSTICS (stderr): "Warning: ... [kind]" (yellow), once per kind, with a
summary of repeats at exit (--log=warn.<kind>:2 prints each); "Error: ...
[kind]" (red) when the run stops with status: error. Logs: --log, SEA_LOG
(docs/reference/logging.md).

STATUS CODES: 0 the run ended (any status but error or memory-error); 4
memory-error (--defer-memory-errors: a memory-safety violation on a run
that reached the end or a failing assert; like sea-vc's 4 (A-INB), plus
double free and bad free); 1 error (bad
input, missing nondet value, unsupported op). --fuzz: 0 when the trials ran
(assert failures are in the output), 4 when there is a memory-safety
finding (fuzz.memory-safety), 1 on a bad option or corpus, an unwritable
--fuzz-out, or a failure that does not replay (fuzz.replay-mismatch).
)";

static int fail(const Twine &msg) {
  errs() << "sea-run: " << msg << "\n";
  return 1;
}

static std::string locText(Location l) {
  std::string t;
  raw_string_ostream os(t);
  os << l;
  return t;
}

/// The report of one run: program, its inputs, prints, status, steps.
static void printRun(sea::out::Emitter &out, sea::ProgramOp program,
                     const sea::RunResult &r) {
  {
    sea::out::Record rec("program");
    rec.str("name", program.getSymName());
    rec.text = ("program: @" + program.getSymName()).str();
    out.emit(rec);
  }
  for (const sea::NondetRead &rd : r.reads) {
    sea::out::Record rec("nondet");
    rec.num("site", rd.site)
        .str("callee", rd.callee)
        .num("width", rd.value.getBitWidth())
        .integer("value", rd.value)
        .str("origin", rd.origin);
    raw_string_ostream os(rec.text);
    os << "nondet: site=" << rd.site << " callee=" << rd.callee
       << " width=" << rd.value.getBitWidth() << " value=";
    rd.value.print(os, /*isSigned=*/false);
    os << " origin=" << rd.origin;
    out.emit(rec);
  }
  for (const sea::CellRead &cr : r.cellReads) {
    sea::out::Record rec("memread");
    rec.num("site", sea::memSiteOf(cr.siteOp));
    // an abstract memory's cell: object id + offset (no address)
    if (!cr.object.empty())
      rec.str("obj", cr.object).integer("off", cr.addr);
    else
      rec.integer("addr", cr.addr);
    rec.num("width", cr.value.getBitWidth())
        .integer("value", cr.value)
        .str("origin", cr.origin);
    raw_string_ostream os(rec.text);
    os << "memread: site=" << sea::memSiteOf(cr.siteOp) << " addr=";
    if (!cr.object.empty())
      os << cr.object << "+";
    cr.addr.print(os, /*isSigned=*/false);
    os << " width=" << cr.value.getBitWidth() << " value=";
    cr.value.print(os, /*isSigned=*/false);
    os << " origin=" << cr.origin;
    if (cr.object.empty())
      if (std::string obj = sea::locateAddress(r.objects, cr.addr);
          !obj.empty()) {
        os << " obj=" << obj;
        rec.add(sea::locateAddressFields(r.objects, cr.addr));
      }
    out.emit(rec);
  }
  sea::printPrintRecords(out, r, printMode, printLocs);
  {
    // one record: the status and where the run stopped
    sea::out::Record rec("status");
    rec.str("status", sea::stringifyRunStatus(r.status));
    raw_string_ostream os(rec.text);
    os << "status: " << sea::stringifyRunStatus(r.status);
    if (std::optional<Location> stop = sea::stopLocOf(r)) {
      std::string l = locText(*stop);
      os << "\nat: " << l;
      rec.str("loc", l);
    }
    if (r.status == sea::RunStatus::AssertFailed) {
      if (r.failLoc) {
        if (std::string l = sea::formatLoc(*r.failLoc); !l.empty())
          os << "\nassert-at: " << l;
        sea::addLocFields(rec, "at", *r.failLoc);
      }
      std::string ctx;
      raw_string_ostream cs(ctx);
      sea::printAssertContext(cs, "", r);
      if (!ctx.empty())
        os << "\n" << StringRef(ctx).drop_back();
      if (r.assertLoc)
        sea::addLocFields(rec, "op", *r.assertLoc);
      sea::addPathFields(rec, r.branchPath);
    }
    if (r.status == sea::RunStatus::MemoryError) {
      // the end or failing assert the run reached past the violation
      std::optional<Location> reached = sea::reachedLocOf(r);
      std::string l = reached ? sea::formatLoc(*reached) : "";
      os << "\nreached: " << sea::stringifyRunStatus(r.reachedStatus)
         << " at=" << (l.empty() ? "-" : l);
      rec.str("reached", sea::stringifyRunStatus(r.reachedStatus));
      if (reached)
        sea::addLocFields(rec, "reached_at", *reached);
    }
    if (!r.message.empty()) {
      os << "\nmessage: " << r.message;
      rec.str("message", r.message);
    }
    if (r.deferredCount) {
      // --defer-memory-errors: the violations the run went past
      std::string l = sea::formatLoc(r.deferredAt->getLoc());
      os << "\ndeferred: " << r.deferredCount
         << " kind=" << sea::stringifyRunErrorKind(r.deferredKind)
         << " at=" << (l.empty() ? "-" : l);
      rec.num("deferred", r.deferredCount)
          .str("deferred_kind", sea::stringifyRunErrorKind(r.deferredKind));
      sea::addLocFields(rec, "deferred_at", r.deferredAt->getLoc());
    }
    out.emit(rec);
  }
  out.emit(sea::out::parseLine("steps: " + std::to_string(r.steps)));
}

/// sea-run --fuzz (docs/reference/fuzzing.md): the first failure is
/// replayed from its input and reported as a plain run; then the `fuzz.*`
/// stat lines.
static int runFuzz(sea::ProgramOp program) {
  if (trace || !inputs.empty() || !inputFile.empty() ||
      missing.getNumOccurrences())
    return fail("--fuzz does not take --trace, --input, --input-file or "
                "--missing");
  std::vector<sea::CorpusEntryText> seeds;
  std::string error;
  for (const std::string &c : corpus)
    if (failed(sea::readCorpus(c, seeds, error)))
      return fail(error);
  sea::FuzzOptions fo;
  fo.trials = fuzzTrials;
  fo.seconds = fuzzTime;
  fo.seed = seed;
  fo.dictionary = fuzzDict;
  fo.keepGoing = keepGoing;
  fo.deferMemoryErrors = deferMemoryErrors;
  fo.outDir = fuzzOut;
  fo.maxSteps = maxSteps;
  FailureOr<sea::FuzzReport> rep = sea::fuzz(program, seeds, fo, error);
  if (failed(rep))
    return fail(error);

  sea::out::Emitter out(outs(), outFormat);
  raw_ostream &os = out.lines();
  // The first failure, as a plain run on its input: what
  // `sea-run --input-file=<its file>` prints (with --defer-memory-errors
  // for a memory-error).
  if (!rep->failures.empty()) {
    const sea::FuzzFailure &f = rep->failures.front();
    sea::InputSource source;
    sea::addAssignments(source, f.input);
    sea::RunOptions options;
    options.maxSteps = maxSteps;
    options.deferMemoryErrors = deferMemoryErrors;
    options.prints = printMode != sea::PrintMode::Off;
    options.printLimit = printLimit;
    sea::RunResult r = sea::interpret(program, source, options);
    printRun(out, program, r);
    if (r.status != f.status || r.at != f.at) {
      out.flush();
      sea::diag::error("fuzz.replay-mismatch",
                       "the failing input of trial " + Twine(f.trial) +
                           " replays to status " +
                           sea::stringifyRunStatus(r.status) + ", not " +
                           sea::stringifyRunStatus(f.status));
      return 1;
    }
  }
  auto where = [](const sea::FuzzFailure &f) {
    std::string at = f.at ? sea::formatLoc(f.at->getLoc()) : "";
    return at.empty() ? std::string("-") : at;
  };
  // `<key>: <runs> trial=<n> [file=<path>] [op=<op> [site=<n>]] at=<where>`
  auto head = [&](StringRef key, const sea::FuzzFailure &f, bool op) {
    os << key << ": " << f.count << " trial=" << f.trial;
    if (!f.file.empty())
      os << " file=" << f.file;
    if (op && f.at) {
      os << " op=" << f.at->getName();
      if (auto site = f.at->getAttrOfType<IntegerAttr>("site"))
        os << " site=" << site.getInt();
    }
    os << " at=" << where(f) << "\n";
  };
  bool memoryFinding = false;
  for (const sea::FuzzFailure &f : rep->failures)
    if (f.status == sea::RunStatus::AssertFailed)
      head("fuzz.failure", f, false);
  for (const sea::FuzzFailure &f : rep->failures)
    if (f.status == sea::RunStatus::MemoryError) {
      memoryFinding = true;
      head("fuzz.memory-safety", f, true);
      os << "fuzz.memory-safety-message: " << f.message << "\n";
    }
  for (const sea::FuzzFailure &f : rep->memoryChecks) {
    head("fuzz.memory-check", f, true);
    os << "fuzz.memory-check-message: " << f.message << "\n";
  }
  for (const sea::FuzzFailure &f : rep->limits) {
    head("fuzz.limit", f, true);
    os << "fuzz.limit-message: " << f.message << "\n";
  }
  double rate = rep->seconds > 0 ? rep->trials / rep->seconds : 0;
  os << "fuzz.trials: " << rep->trials << " seeds=" << rep->seedTrials
     << " time=" << format("%.3f", rep->seconds)
     << " execs/s=" << format("%.0f", rate) << "\n";
  os << "fuzz.status:";
  for (sea::RunStatus st :
       {sea::RunStatus::Halted, sea::RunStatus::AssertFailed,
        sea::RunStatus::AssumeBlocked, sea::RunStatus::StepLimit,
        sea::RunStatus::Error})
    os << " " << sea::stringifyRunStatus(st) << "="
       << rep->statusCount[static_cast<unsigned>(st)];
  if (deferMemoryErrors)
    os << " memory-error="
       << rep->statusCount[static_cast<unsigned>(sea::RunStatus::MemoryError)];
  os << "\n";
  if (deferMemoryErrors) {
    // the runs that went past a violation, by how they ended
    uint64_t runs = 0;
    for (uint64_t n : rep->deferredStatusCount)
      runs += n;
    os << "fuzz.deferred: " << runs;
    for (sea::RunStatus st :
         {sea::RunStatus::MemoryError, sea::RunStatus::AssumeBlocked,
          sea::RunStatus::StepLimit, sea::RunStatus::Error})
      os << " " << sea::stringifyRunStatus(st) << "="
         << rep->deferredStatusCount[static_cast<unsigned>(st)];
    os << "\n";
  }
  os << "fuzz.coverage: blocks=" << rep->blocksHit
     << " blocks-total=" << rep->blocksTotal << " edges=" << rep->edgesHit
     << " assumes=" << rep->assumesPassed
     << " assumes-total=" << rep->assumesTotal << "\n";
  unsigned skipped = 0;
  for (const sea::CorpusEntryText &s : seeds)
    skipped += s.skippedLines;
  os << "fuzz.corpus: " << rep->corpusSize << " seeds=" << seeds.size()
     << " skipped-lines=" << skipped << " files=" << rep->filesWritten
     << " dictionary=" << (fuzzDict ? rep->dictionarySize : 0) << "\n";
  for (auto [op, n] : rep->blocked) {
    std::string at = sea::formatLoc(op->getLoc());
    os << "fuzz.blocked: " << n << " op=" << op->getName()
       << " at=" << (at.empty() ? "-" : at) << "\n";
  }
  out.flush();
  return memoryFinding ? 4 : 0;
}
int main(int argc, char **argv) {
  InitLLVM y(argc, argv);
  // SEA_LOG / SEA_LOG_FILE, before --log entries override them
  sea::log::init();
  // Repeated warnings are summarized at exit, whatever the exit path.
  struct Summary {
    ~Summary() { sea::diag::printSummary(); }
  } summary;
  sea::setVersionPrinter("sea-run");
  cl::ParseCommandLineOptions(argc, argv, "Run a sea program\n");
  if (agent) {
    outs() << agentGuide;
    return 0;
  }
  if (fuzzTime < 0)
    return fail("--fuzz-time must not be negative");
  if (!fuzzTrials && fuzzTime <= 0 &&
      (!corpus.empty() || !fuzzOut.empty() || keepGoing ||
       fuzzDict.getNumOccurrences()))
    return fail("--corpus, --fuzz-out, --fuzz-dict and --keep-going need "
                "--fuzz or --fuzz-time");

  sea::log::setFormat(outFormat);
  MLIRContext context;
  context.getOrLoadDialect<sea::SeaDialect>();
  OwningOpRef<ModuleOp> module = sea::parseInputFile(inputFilename, &context);
  if (!module)
    return 1;

  if (printRefs != sea::PrintRefs::AsIs) {
    PassManager pm(&context);
    sea::addPrintRefsPass(pm, printRefs);
    if (failed(pm.run(*module)))
      return 1;
  }

  sea::ProgramOp program;
  for (auto p : module->getOps<sea::ProgramOp>()) {
    if (!programName.empty() && p.getSymName() != programName)
      continue;
    if (program)
      return fail("several programs; pick one with --program");
    program = p;
  }
  if (!program)
    return fail(programName.empty() ? "no sea.program in input"
                                    : "no program named '" + programName + "'");

  sea::InputSource source(missing, seed);
  if (!inputFile.empty()) {
    if (sys::fs::is_directory(inputFile))
      return fail("--input-file '" + inputFile +
                  "' is a directory; give one corpus file");
    std::vector<sea::CorpusEntryText> entries;
    std::string error;
    if (failed(sea::readCorpus(inputFile, entries, error)))
      return fail(error);
    sea::addAssignments(source, entries.front().assignments);
  }
  for (StringRef in : inputs) {
    sea::Assignment a;
    std::string expected;
    if (failed(sea::parseAssignment(in, a, expected)))
      return fail("bad --input '" + in + "', " + expected);
    sea::addAssignments(source, a);
  }

  if (fuzzTrials || fuzzTime > 0)
    return runFuzz(program);

  sea::RunOptions options;
  options.maxSteps = maxSteps;
  options.prints = printMode != sea::PrintMode::Off;
  options.printLimit = printLimit;
  options.deferMemoryErrors = deferMemoryErrors;
  sea::out::Emitter out(outs(), outFormat);
  // `value: <op> <loc> : <type> = `
  auto traceHead = [&](Value val, sea::out::Record &rec) {
    raw_string_ostream os(rec.text);
    std::string loc;
    if (Operation *op = val.getDefiningOp()) {
      loc = locText(op->getLoc());
      os << "value: " << op->getName() << " " << loc;
      rec.str("def", op->getName().getStringRef());
    } else {
      loc = locText(val.getLoc());
      os << "value: block-arg " << loc;
      rec.str("def", "block-arg");
    }
    std::string ty;
    raw_string_ostream ts(ty);
    ts << val.getType();
    os << " : " << ty << " = ";
    rec.str("loc", loc).str("type", ty);
  };
  if (trace) {
    options.onValue = [&](Value val, const APInt &v) {
      if (isa<sea::AddrType>(val.getType()))
        return; // with its pointer token (onPointer)
      sea::out::Record rec("value");
      traceHead(val, rec);
      rec.text += toString(v, 10, /*Signed=*/false);
      rec.integer("value", v);
      out.emit(rec);
    };
    options.onPointer = [&](Value val, const APInt *bits,
                            const std::string &token) {
      sea::out::Record rec("value");
      traceHead(val, rec);
      if (bits) {
        rec.text += toString(*bits, 10, /*Signed=*/false) + " ptr=" + token;
        rec.integer("value", *bits);
      } else {
        rec.text += token;
      }
      rec.str("ptr", token);
      out.emit(rec);
    };
  }

  sea::RunResult r = sea::interpret(program, source, options);

  printRun(out, program, r);
  out.flush();
  if (r.status == sea::RunStatus::Error) {
    sea::diag::error("interp.stopped", "the interpreter stopped: " + r.message,
                     r.at ? std::optional<Location>(r.at->getLoc())
                          : std::nullopt);
    return 1;
  }
  return r.status == sea::RunStatus::MemoryError ? 4 : 0;
}
