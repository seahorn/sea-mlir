# Changelog

## Unreleased

- LLVM/MLIR pin moved to `302c842d6601` (the upstream base of an IREE LLVM
  integrate), from `93f05d753ad3`. The integer ranges of
  `sea.sdiv` / `sea.srem` add sea's values for a zero divisor (upstream now
  treats it as poison and bounds the nonzero divisors only).

## 0.1.1

Same LLVM pin as 0.1.0. Verdicts are unchanged; the release adds reports
and flags, removes a hidden experiment, makes z3 errors visible, and speeds
up the Expr core. Versioning rule: [docs/Release.md](docs/Release.md).

### Visibility
- `sea-vc` progress inside `encode` (`--progress-interval`), the memory at
  the end of each phase, and the place of a `--mem-limit` hit.
- `--arith-stats`: multiplications, divisions and remainders of the query by
  operator, width and operand kind, per source location.
- Integer split: arithmetic candidates for `--split`; `--ite-stats` reports
  ite trees feeding arithmetic and loads at non-ground addresses.
- Logs (`--log`, `SEA_LOG`; docs/reference/logging.md): tagged, leveled
  events on stderr; level 1 traces the flow of a run (`vc.phase`,
  `lower.pass`, `vc.solve`). `--progress` is `--log=vc.phase`.
- `sea-expr-eval` prints `tool.version` and the diagnostic summary.

### Dialect and passes
- `sea.abs` (from `llvm.abs`; `is_int_min_poison` as nsw).
- `sea.box.all`, a claim over a bounded index box; `sea-range-fold` (folds
  settled by integer ranges); `--expand-memmove` (short memmoves as copies);
  constant folding of assumes, asserts, straight blocks and loads.

### VC generator (`sea-vc`)
- Case splitting of the VC (`--split`, `--split-leaves`,
  `--split-candidates`), split targets for Ackermann residues,
  `--print-lowered`, `--replay-original`.
- `--reach`: reachability of check sites through the VC; named assumptions
  `addr0-is-null` and A-INB of dropped memhavocs.
- `--words-check-rlimit`, `--eq-audit-rlimit`, `--seam-check-rlimit`: z3
  resource limit per auxiliary check (deterministic; unset by default, untuned
  until a corpus sweep); an exhausted check is `unknown`, `reason=rlimit`.
- Behaviour changes of the cleanup of the solver and output code:
  - `--seam-check` on a build without Bitwuzla (`--solver=bitwuzla`) now
    reports `vc.solver-unavailable` under `--smt2` and `--vcgen-only`; it was
    skipped silently there.
  - Removed the hidden `--x-read-table*` experiment (a guarded read table at
    the solver boundary; design and data are in the project notes).
  - `--z3-tactic` applies to the solver of the verdict only, no longer to
    the auxiliary checks; a tactic z3 rejects is the error `vc.z3-tactic`.
  - Output files (`--print-lowered`, `--place-out`, `--reach-programs`,
    `--ack-dump-pairs`, `--reach-witness`, `--split-smt2`) are written whole
    (a temporary, then a rename; the path `-` is stdout, a target that is not
    a regular file is written directly), and a failed write is an error of
    its own kind: `vc.print-lowered`, `place.out-file`, `vc.reach-programs`,
    `vc.ack-dump-pairs`, `vc.reach-witness`, `vc.split-smt2`.
- z3 errors are reported. sea-mlir builds without exceptions, so z3++ ignored
  them (and a failing call went on). `--z3-tactic` that z3 cannot parse
  (`vc.z3-tactic`) and `--z3-param` that z3 rejects (`smt.z3-param`) are
  errors; any other z3 error reaches a handler on every z3 context: kind
  `smt.z3-internal`, non-zero exit. A z3 memory-out is not one of
  these: it ends as unknown (exit 3), as a memory limit does.
- Expr core speedup: one unique table for all operators, a lookup before a
  node is built, numeral hash on limbs, parameters by const reference.

### Interpreter (`sea-run`)
- `--fuzz`: greybox fuzzing over the interpreter, with `--fuzz-sites`,
  `--fuzz-serve`, model coverage and corpus feedback.

### Internal cleanup
- `sea::Timer` for all timing; stat lines and `BRUNCH_STAT` from one
  `Report`; one `SolverFactory` for every solver of the tools; `assume.*`
  lines from one assumption ledger; `writeFileAtomic` for output files.
- `check-sea` honours `LLVM_LIT_ARGS`.

### Tools and utilities
- `utils/output-diff.py`: tool output of two builds (a gate tool);
  `utils/encoding-diff.py`: query identity of two encodings;
  `utils/cover-diff.py`; `utils/query-diff.py` classes and evidence line.

## 0.1.0: the first release

Built against LLVM/MLIR at `93f05d753ad3` (`LLVM_REVISION`). Linux only.
Scope, build, assumptions and known issues: [docs/Release.md](docs/Release.md).

### Dialect and semantics
- The `sea` dialect for SeaHorn's programs: signless bit-vectors with total
  SMT-LIB semantics, CFGs with block arguments, nondet sources, assume /
  assert, `sea.unreachable`.
- Memory: Mem-SSA memories with `inplace` updates; pointers and abstract
  objects (malloc / alloca / globals, `sea.segment`); memset / memmove /
  memhavoc; memory layouts in the type (exact, aligned, unaligned, keyed).
- Structs (register-only, `sea.struct.nondet`), intrinsics (min/max, bswap,
  funnel shifts, overflow ops, ctpop), `sea.concat` / `sea.extract`.
- `sea.ptr.index`: GEPs with trusted no-wrap flags (docs/design/PtrIndex.md);
  `sea.addr.from_bits` / `to_bits`; `sea.ptr.base` / `dereferenceable`.
- Print ops: `sea.print`, `sea.print.mem`, scopes, weak and strong
  references, object labels (docs/design/Print.md).
- Folders for the pure ops, through the one op-semantics table.

### Passes (`sea-opt`, `sea-vc --lower`)
- Static placement of objects (overflow-checked), struct scalarization,
  memset expansion with an explicit bound, critical-edge splitting, wide
  accesses, intrinsic expansion, no-wrap promises, `sea-widen-memory`.
- Tracking and shadow memory, fat-pointer companions, `sea-lower-ptr-index`.
- The VC lowering pipeline with upstream sccp / cse / canonicalize.
- Check jobs: `sea-check-ptr-index` (GEP promises), the align check.
- Every pass states its contract and checks its postcondition.

### Interpreter (`sea-run`)
- The reference semantics: concrete and abstract (bounds-checked) memory,
  inputs per nondet site and memory cell, traces, prints.

### VC generator (`sea-vc`)
- SeaHorn-style BMC VCs over the Expr library (ported from SeaHorn), with z3,
  Yices 2 and Bitwuzla (optional) backends; array and lambda memory
  encodings; ite or equality joins.
- Counterexample replay through the interpreter, validated value by value
  (exit 2 on a disagreement); the source location of a failed assertion.
- SeaM (`--seam`): object-aware rewriting of loads and stores, with ARM,
  Store-Map, memory operations as terms, null as an object, mixed-width
  terms, initial word encoding (`--seam-words`), and the A-INB assumption
  assumed and reported (exit 4 for a sat that leaves an object).
- The pointer sort (PTR_INDEX, PTR_EQ, PTR_DIFF, ...; `--ptr-mode`), erased
  at the solver boundary; whole-query type check (`--vc-typecheck`).
- Bit-vector / Boolean normalization (`--simplify`); Ackermann lowering of
  read-only arrays with the stride rule (`--ackermannize`, `--ack-stride`).
- Assurance: the per-firing rule check (`--seam-check`), the rule trail
  (`--seam-trail`), `--eq-audit`, `--dag-stats`.
- Limits and progress: `--timeout`, `--mem-limit`, `--vcgen-only`,
  `--progress`.
- Profiles (`--config`, LLVM config files), `--format=jsonl`, `--agent`.

### Tools and utilities
- `sea-reduce` (mlir-reduce with sea patterns and the sea reduction tree)
  and `sea-reduce-test` (`utils/reduce-test.py`) for reduction.
- `sea-seam-export`: exports a straight-line program's SeaM terms (raw,
  per `sea-vc` configuration, and after the solver boundary) and the
  rewriter's rule firings with their premises, as versioned JSON for the
  Lean conformance checks of seam-lean ([docs/SeamExport.md](docs/SeamExport.md)).
- `--version` for every tool; `sea-vc` prints `tool.version:` first.
- Source tree only: `utils/bench.py` (benchmark suites in separate
  repositories, tiers and gates), `utils/query-diff.py`, the fuzzers and
  semantic oracles; `just release-check`.
