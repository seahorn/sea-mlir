# sea-mlir

An MLIR dialect (`sea`) for the programs SeaHorn generates verification
conditions from, with an interpreter (golden semantics, counterexample
replay) and a VC generator. Research prototype: uses the newest MLIR, no
text-format stability.

## Build (out of tree, against an installed LLVM+MLIR)

LLVM `main` at the commit in `LLVM_REVISION`, built with
`-DLLVM_ENABLE_PROJECTS=mlir -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_INSTALL_UTILS=ON`.

```bash
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 -DLLVM_ENABLE_LLD=ON \
  -DMLIR_DIR=<llvm-install>/lib/cmake/mlir -DZ3_ROOT=<z3 install> \
  -DSEA_Z3=<path to z3 binary> \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build check-sea
```

`check-sea` runs lit with `LLVM_LIT_ARGS`, default `-sv -j8` (a bounded
default, not tuned; one worker per core otherwise). Override:
`-DLLVM_LIT_ARGS="-sv -j<N>"`. An existing build dir keeps its cached value
(configured without it: `-sv`); set `-DLLVM_LIT_ARGS` once.

Also needs Boost (headers), GMP and z3 (`Z3_ROOT`). Yices 2 is optional
(`-DSEA_ENABLE_YICES=AUTO|ON|OFF`, `YICES2_ROOT` if not system-wide; GPLv3:
release binaries are built without it; tests that need it: `REQUIRES: yices`
or `%if yices %{ ... %}`). Bitwuzla 0.9 is optional: `-DBITWUZLA_ROOT=<install>`
enables `--solver=bitwuzla` (`docs/design/Bitwuzla.md`).

## Layout

- `include/sea/Dialect/Sea/*.td` — the dialect (ODS); `lib/Dialect/Sea/`
- `include/sea/Dialect/Sea/Semantics.h` (`lib/Dialect/Sea/Semantics.cpp`) —
  op semantics (`evalPureOp`, the only place values are computed: the
  interpreter and the op folders call it; it lives in the dialect library
  so the folders can); `include/sea/Interp/Interpreter.h` — CFG execution,
  nondet sources
- `include/sea/Expr/`, `lib/Expr/` — Expr (SMT terms) and its z3 / Yices
  / Bitwuzla (optional)
  backends, ported from SeaHorn; read `include/sea/Expr/README.md` first.
  One C++ style everywhere: LLVM / MLIR (`.clang-format`).
- `include/sea/VC/` — VC generator, SeaHorn's split: `OpSem.h` (symbolic
  execution of sea ops on a `SymStore`) and `VCGen.h` (block guards, edges,
  block arguments; BMC form check); `Names.h` (VC symbol names = printed
  MLIR names); `Replay.h` (model -> interpreter run, with validation);
  `Split.h` (case splitting of the VC at the definitions of chosen values
  / block guards, `sea-vc --split`; also the ite census of
  `sea-vc --ite-stats`; docs/design/Split.md);
  `Origins.h` (Expr node -> location of the sea op that made it, recorded
  by OpSem and followed through simplify / boundary / Ackermann: `sea-vc
  --arith-stats`, `--ack-dump-pairs`; the census is `tools/sea-vc/ArithStats.h`);
  `sea/Expr/ArithShape.hh` (the arithmetic nodes and their operands' value
  trees, shared by those reports and Origins);
  `sea/Expr/ExprMlir.hh` (Expr terminals holding mlir::Value / Block)
- `lib/Transforms/` — passes: `sea-place-static` (abstract pointers ->
  constant addresses; each object is a `sea.segment`, whose result, the base
  address, replaces the object's uses: no folder, so sccp keeps it;
  `place-out` / `place-in`: the placement written to / taken from a file,
  checked, `sea-vc --place-out` / `--place-in`), `sea-scalarize-structs`
  (register structs -> their leaves; runs first), `sea-expand-memset`
  (symbolic length -> bounded store chain + explicit assume),
  `sea-expand-small-memmove` (constant length <= K -> all source loads,
  then the stores: exact; `sea-vc --expand-memmove=K`, off by default,
  for --seam reads whose mcopy range test stays open;
  docs/design/SeaM-memops.md),
  `sea-split-critical-edges`,
  `sea-split-wide-access`, `sea-expand-intrinsics` (min/max, abs, bswap,
  funnel shifts, overflow ops; mul overflow kept for z3 and Bitwuzla),
  `sea-wrap-flags`
  (nsw/nuw promises -> assume/assert/drop), `sea-sink-assumes` (every
  assume of a BMC-form program -> one assume of the conjunction on the
  path, before the assert: SeaHorn's UnifyAssumes; `sea-vc
  --sink-assumes`, off by default: a solver tradeoff), `sea-range-fold`
  (icmp / i1 op / select condition settled by integer ranges: the sea ops'
  InferIntRangeInterface, upstream transfer functions adapted to total
  semantics in `lib/Dialect/Sea/SeaIntRanges.cpp`, intersected with the
  simple bounds of dominating assumes, at the candidate's definition; `sea-vc --range-fold`,
  off by default; `check`: the paranoid job; docs/design/RangeFold.md),
  `sea-widen-memory` (byte
  memory -> aligned k-byte words when every access provably fits; in the
  lowering with `sea-vc --widen-memory=k`), `sea-apply-tracking`
  (tracking_on/off markers -> `tracked` updates; first in the lowering),
  `sea-lower-tracking` (tracking -> a shadow memory chain per tracked
  memory, keyed by sea.ptr.base, starting as sea.mem.const 0 shadow:
  read by object identity in the VC, never by A-INB),
  `sea-lower-fat-pointers` (offset / size companions of pointers and of
  pointer-holding memories; ptr.base / dereferenceable become arithmetic);
  `sea-lower-ptr-index` (`sea.ptr.index`, explicit GEP pointer arithmetic with trusted no-wrap flags, -> mul / add / ptr.add, flags dropped; standalone only: the VC encodes the op as a PTR_INDEX node, docs/design/PtrIndex.md),
  `sea-check-ptr-index` (paranoid job: assert every used GEP no-wrap promise, docs/design/PtrIndex.md section 6; `sea-vc --check-ptr-index`),
  `sea-lower-box-all` (`sea.box.all`, a conjunction over a box of 1-2
  i64 indices, in assert polarity only -> its body at one nondet index
  with `assume(t ult tripcount)`, guarded when the box may be empty; other
  uses are errors; a constant box of at most K indices is expanded instead,
  `sea-vc --box-all-expand=K`; first after sea-apply-tracking in the lowering;
  docs/design/BoxAll.md),
  `sea-print-strengthen` / `sea-print-weaken` (counterexample print ops,
  weak = by name vs strong = SSA operands, docs/design/Print.md),
  `sea-drop-fresh-memhavoc` (memhavoc of an object nothing wrote since its
  allocation is a no-op: removed; others stay), `sea-guard-null-memhavoc`
  (a memhavoc whose pointer may be null runs only when it is not: the open
  decision memhavoc(null) = no-op, `--memhavoc-null`); all run before placement
  in the lowering of abstract programs. `Passes.cpp`: registration and
  the VC lowering order (what `sea-vc --lower` runs; upstream `sccp`
  first, to fold values computed from constants; upstream `cse` after it
  and `canonicalize` + `cse` before the final `sea-split-critical-edges`,
  off with `sea-vc --lower-opt=false` / `sea-vc-lower{opt=false}`). Each pass states its
  contract at the top of its file and checks its postcondition
  (`<pass>.postcondition` = a bug in the pass)
- `tools/sea-opt` (mlir-opt driver), `tools/sea-run` (interpreter CLI),
  `tools/sea-vc` (VC + solve + replay), `tools/sea-expr-eval` (test
  utility: evaluate terms through Expr), `tools/sea-reduce` (mlir-reduce
  with the sea dialect: test-case reduction, `docs/Reducer.md`; its
  patterns and passes `sea-reduce-apply` / `sea-reduce-cleanup` are in
  `lib/Reduce/`, the driver of interestingness tests is
  `utils/reduce-test.py`)
- `unittests/` — plain-main C++ test drivers run by lit (`sea-expr-rewriter-test`,
  `sea-seam-rules-test`, `sea-addr-range-test`: ARM, `include/sea/Expr/AddrRangeMap.hh`,
  `sea-store-map-test`: Store-Map, `include/sea/Expr/StoreMap.hh`, `sea-bv-rules-test`: the bit-vector / Boolean normalization rules, `include/sea/Expr/BvRules.hh`, `sea-log-test`: the log selection syntax and event text, `include/sea/Support/Log.h`, `sea-timer-test`: the timers of the `*-time` keys, `include/sea/Support/Timer.h`). `sea-vc --seam`: SeaM, the object-aware rewriting of loads and
  stores in the VC (`include/sea/Expr/SeaMRules.hh`, `docs/design/SeaM.md`);
  `sea-vc --simplify` (default on under `--seam`, `--seam-simplify=0` off) runs the same rewriter with the rules of `BvRules.hh` over every VC term, side conditions and query included (valid over plain bit-vectors; also usable without `--seam`); `store-map` is an Expr operator the solvers do not know: memory values of the
  VC may hold it, and whatever goes to a solver or a model evaluation passes
  `SeaOpSem::solverTerm`, which expands it into stores; `--seam` also encodes memset / memmove / memhavoc as
  memory-valued terms (`mset` / `mcopy` / `mhavoc`, `include/sea/Expr/MemOps.hh`,
  `docs/design/SeaM-memops.md`) read through one range test each, and
  skips `sea-expand-memset` (symbolic lengths exact); `solverTerm`
  removes them by their read equations only (semantics-only boundary)
- `test/` — lit + FileCheck; `utils/check-semantics.py` — z3 oracle
  (interpreter, Expr, expanded intrinsics, folders); `utils/query-diff.py`
  — the gate of a change that must not change queries: two sea-vc binaries
  over a corpus, each job identical / order / both-unsat (equisat only) /
  equiv-sat (z3) / DIFF / unknown (`--project-blocks`: up to renamed
  block guards; `--agent` says what each class proves);
  `utils/encoding-diff.py` — its complement: one sea-vc on two programs
  (two encodings of one job): identical / same-dag / equiv-parts / equiv /
  both-unsat / DIFF / unknown (`--agent`; both-unsat is not equivalence);
  `utils/output-diff.py` — the gate of a refactoring that must not change
  output: two builds of sea-vc / sea-run over a corpus x argument matrix,
  text and jsonl, timings masked, stat and BRUNCH_STAT keys compared;
  `utils/bench.py` (`--agent`) — benchmarks: corpora, runs, and the suites
  in separate repositories (`just bench-connect`, `just bench-gate`,
  `docs/Benchmarks.md`); `utils/release-check.sh` (`just release-check`)
  — the release gate (docs/Release.md: what is installed, the version
  mechanism `include/sea/Support/Version.h` + `cmake/SeaVersion.cmake`)

## Semantics (decisions)

- Values are builtin signless `iN` bit-vectors. `i1` is a bv1; the VC
  encoding decides Bool vs `(_ BitVec 1)`.
- Addresses are pointers in the VC: the Expr sort `ptr(w)`, with explicit
  constructors (PTR_INDEX, PTR_FROM_BITS, PTR_FROM_INT) and observations
  (ADDR, PTR_TO_INT, PTR_TO_BITS, PTR_DIFF, PTR_EQ), erased to `bv(w)` at
  the solver boundary (`SeaOpSem::solverTerm`); every VC but
  `--mem=lambda`. Two pointer modes (`sea-vc --ptr-mode`, default auto):
  opaque refuses int-to-ptr, address admits it as PTR_FROM_INT (no
  provenance). Read `docs/design/PtrSort.md`.
- Every op is total, SMT-LIB bit-vector semantics (`bvudiv x 0 = ~0`,
  `bvurem x 0 = x`, shifts by >= width give 0 / sign fill). No poison/undef.
- `sea.nondet "<callee>" [<site>]`: the site is the stable name of the
  value (VC constant, model lookup on replay); unique per program.
- Program forms needed by a consumer (loop-free, single assert, no critical
  edges) are established by passes and checked by verifier passes; the VC
  generator refuses input not in its form. Every program change is a pass.
- VC (sea-vc) input is BMC form: loop-free, one sea.halt, one sea.assert
  right before it, no critical edges, distinct cond_br targets. i1 is a bv1
  in the VC; `SeaOpSem::toBool/fromBool` are the only places that decide it.
- Print ops (`sea.print`, `sea.print.mem`, scopes; docs/design/Print.md)
  show counterexample values and change nothing: the VC encodes no term
  for them (the query is byte-identical without them). Weak prints name
  values (`nd.<site>`, NameLoc names) and keep nothing alive; strong ones
  have SSA operands, and a pass that rewrites values must handle them as
  users.
- Counterexamples come from the interpreter: a sat model is replayed, and
  every computed value is checked against the model (sea-vc exit 2 = VC and
  interpreter disagree = a bug).
- Structs (`!sea.struct<...>`) are register-only aggregates (LLVM's
  first-class structs and arrays); no undef: `sea.struct.nondet` is the
  explicit nondet initialization (one bit-vector of the integer leaves, low
  bits first; pointer leaves null). The VC needs them scalarized.
- Memory: read `docs/design/Memory.md` and `docs/design/Memory-Step2.md`.
  Mem-SSA chains (memory is a value); `inplace` on updates; the interpreter
  executes in place only; the VC uses the array encoding (load/store;
  constant-length memset/memmove/memhavoc as store chains; a symbolic
  memset length is expanded by a pass with an explicit bound; other
  symbolic lengths are errors). The memory type names its layout
  (`!sea.mem<I, iC[, aligned|unaligned]>`): exact (default) = one cell per
  index unit (pointer-indexed: i8; address-indexed: the unit); aligned =
  k-byte words at offsets 0 mod k; unaligned = k-byte words anywhere, sound
  only if accesses match; keyed = a cell of any width at every byte
  address, independent cells (more storage than the bytes: metadata
  memories, the fat-pointer companions). Offsets and lengths are in index units (bytes);
  memset's value is one unit (i8), repeated per word. The interpreter
  runs all three and checks alignment / matching accesses; the VC and
  sea-expand-memset refuse word layouts for now (`vc.mem-layout`). `align` on load / store / memset / memmove
  (`src_align`) is LLVM's promise in bytes: no value depends on it, the
  interpreter warns `interp.align-promise` when it is broken (also when the
  object is less aligned), passes may drop it.
  Object metadata (fat pointers / shadow memory; lowered before placement
  by sea-lower-tracking and sea-lower-fat-pointers): `sea.ptr.base`, `sea.ptr.dereferenceable`;
  modification tracking belongs to the memory value (contents + set of
  objects written by `tracked` updates): `sea.mem.is_modified`,
  `sea.mem.reset_modified`.
  Abstract programs (`!sea.ptr`) run in the interpreter with
  exact bounds; the VC needs them placed first (`sea-place-static`,
  `sea-vc --place`).

## Rules

- Commits: Conventional Commits, header <= 72 (`.commitlintrc.yml`); `just lint` before committing; `just attest` posts check-sea and the vcc gates as commit statuses on a clean HEAD (docs/agents/gates.md).

- Publish curated branches with `just publish <branch> --strict`; install the public-remote guard with `just install-hooks` (docs/agents/gates.md).

- Keep source and documentation free of personal paths and unpublished review or study references; run `python3 utils/privacy-check.py` before committing.

- Guiding principle: conditional soundness (Conway, Dams, Namjoshi,
  Barrett, SAS 2008). Encodings and rewrites may rely on named
  assumptions (A-INB: every access stays in its object), reported in the
  output. An assumption is discharged by a separate check (a bounds-check
  instrumented job), never inside the same run by an encoding without it.
  If the discharging check is missing, build that check; do not try to
  verify without the assumption.
- Unsupported input is an error, never silently skipped or approximated.
- Every unimplemented, incomplete or approximate path is visible: report it
  with `sea::diag::warn(kind, ...)` (yellow, once per kind, repeats
  summarized at exit, `--log=warn.<kind>:2` shows each) or `sea::diag::error`
  (red) and stop (`include/sea/Support/Diag.h`). Tools call
  `diag::printSummary()` at exit. Kinds are short stable tags
  (`vc.mem-op-symbolic-length`); tests match them.
- z3 `sat` on a validation query means the validation failed, not that z3
  is incomplete.
- Tests: read `test/README.md`. Oracles (listed there, marked `ORACLE`)
  are protected: never weaken one to make it pass; changing what it
  accepts needs maintainer sign-off. All other tests are disposable: recreate,
  disable (`REQUIRES: disabled-test`) or kill one that blocks a refactor,
  and name it in the commit message. Write tests that pin only the
  property they test.
- Derive semantics questions with z3 before coding them.
- After fixing a soundness bug, look for the same pattern elsewhere.
- New op: add it to `Semantics.cpp`, to `utils/check-semantics.py`, and to
  roundtrip and interpreter tests in the same change. A pure value op gets
  a folder through `evalPureOp` (`foldByEval`, SeaOps.cpp), never its own
  arithmetic; ops with nsw / nuw do not fold. `sea.segment` is in `evalPureOp` but has no folder on purpose (sccp would erase the object it names), and it is not in `check-semantics.py`: that oracle folds every case to a constant and checks bit-vector arithmetic against z3, while a segment's value is its `base` attribute (checked by `test/Interp/ptr/segment.mlir` and `test/VC/segment-replay.mlir`).
- Tag conjectures in comments and notes as hypotheses; cite SeaHorn
  source (file:line, commit) when porting behavior from it.
- Tool output is line-oriented `key: value` text so agents can parse it.
- sea-vc / sea-run `--format=jsonl`: every stdout line one JSON object
  (`kind`, fields, `text` = the text line). Every stdout line goes through
  `sea::out::Emitter` (`include/sea/Support/Output.h`): build a `Record`
  for structured output, or write `key: value [k=v ...]` text to
  `lines()` (a `stat` record in jsonl); never `llvm::outs()` directly.
  Keep the `OUTPUT FORMATS` section of `--agent` in sync;
  `utils/jsonl-check.py` checks text vs jsonl.
- Every tool has `--agent` (as in ctac): a terse plain-text guide (what,
  why use it, typical commands, output format, status codes); keep it in
  sync when flags or output change. Run `<tool> --agent` before using one.
- Every new code or test file gets the two-line SPDX header (copyright `2026 The sea-mlir Authors`, license
  `Apache-2.0 WITH LLVM-exception`; code: top, tests: end); `just spdx-fix` adds it, `just spdx` checks it
  (lit runs the check). Files that cannot carry a comment are listed in `REUSE.toml`.
