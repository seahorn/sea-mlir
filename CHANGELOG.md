# Changelog

## 0.3.0

A minor release: fixes to the lowering and to `--seam` change verdicts,
and output lines and counters change (docs/Release.md, "Version numbers of
the 0.x preview"). Triage verdict differences against v0.2.0.

### Verdict changes
- **Verdict change.** `sea-drop-fresh-memhavoc` (in `--lower`) dropped a
  memhavoc of a fresh object that was read before it, for example the
  second of two havocs with a read between them, and the VC answered unsat
  for a failing program. Such a memhavoc is now kept. (#101)
- **Verdict change.** Under `--memhavoc-null=error`, a memhavoc of a fresh
  object whose pointer may be null is now kept, so the run on null keeps
  its error; before, it was dropped in both modes. A memhavoc of null plus
  an offset is also kept. (#102)
- **Verdict change.** `--seam` separated an access whose address has an
  absolute base (`ptr.index(from_int(16), ...)`) from other objects by
  A-SEP with no A-INB condition behind it, a wrong unsat. The rule
  `arm.skip` now refuses such a decision (counter `arm.skip-no-inb`). (#104)
- **Verdict change.** With `--seam-stride`, the rule `ite.same` now keeps
  one branch of an ite only when both branches carry the same promise;
  before, the kept branch's `ptr.index` promise applied on the other path
  too, a wrong unsat (counter `ite.same-promise-kept`). (#105)
- **Verdict change.** For fat-pointer programs (`sea-lower-fat-pointers`),
  `--seam` accepted a keyed store mirrored only by a data load and
  separated it from another object's keyed access by A-SEP, a wrong unsat.
  A keyed access now needs a data access of the same kind; an unmatched
  one falls back with the warning `vc.seam-companion-unmirrored`. (#109)

### Output and command-line changes
- The `assume.*` counts move. `assume.a-sep: drop-fresh-memhavoc=` counts
  a dropped memhavoc when any access of the memory before it may reach the
  object, or when its range is not provably in the object; a drop of a
  pointer that may be null counts in `assume.memhavoc-null-noop: vc sites=`
  instead. On the vcc corpus the A-SEP count falls from 338 to 22 and 192
  jobs now print the null line. (#101, #102)
- New warnings: `drop-fresh-memhavoc.range` for a dropped memhavoc whose
  range is not provably in its object (#102), `check-ptr-index.not-followed`
  where `--check-ptr-index` stops following a pointer (#112), and
  `vc.limit-clamped` for an rlimit above z3's 32-bit limit (#85).
- `--seam-check-inb-premise` also checks the premises of reads decided
  apart from an mset, mcopy or mhavoc range, and no longer reports an
  in-bounds range destination as unrecorded. (#103)
- The static out-of-object report (`vc.seam-out-of-object`,
  `seam.out-of-object`) changes: a memmove source counts as a read, so a
  read and a read in one gap cell are no longer `shared` (#106, #109); a
  gap cell reached by an out-of-object leaf of an unflagged access lands
  `shared`, and the `gap` text no longer claims that A-SEP holds (#107);
  the warning names A-NULL for an access through null and A-NOWRAP for an
  address that wraps, in any class (#108). `utils/fuzz-seam.py` reads only
  whether the line is present.
- Overflow predicates are printed under z3's names (`bvumul_noovfl`,
  `bvsadd_noovfl`, ...) instead of names with a stray `b`. This changes
  `--arith-stats`, the `vc.dag-op` stats and the `"op"` field of the
  rewrite trail's JSONL; no reader in the repository matched the old names.
  (#80)
- An op the VC does not encode is the error `vc.unsupported-op` with the op
  and its location, instead of a bare `unsupported op`. `sea-seam-export`
  refuses an abstract program with `vc.abstract-program`, as `sea-vc`
  does. (#82)
- The rlimit flags (`--words-check-rlimit`, `--eq-audit-rlimit`,
  `--seam-check-rlimit`, `--reach-rlimit`, `--split-check-rlimit`) take
  64-bit values; before, a value above 4294967295 was refused, and through
  the API it was truncated. (#85)
- `--widen-memory=<k>` gives each memory the largest word size among k,
  k/2, ..., 2 that it qualifies for, instead of k for every memory or none.
  Queries change for `--widen-memory=8` (9 of 228 vcc jobs);
  `--widen-memory-per-class=false` keeps one size, and `vc.widen-memory`
  reports the classes per size. `utils/check-widen.py` follows. (#99)

### New flags and features
- The lowering folds facts, on by default and exact: uses of an i1 that a
  dominating assume, assert or branch edge fixes (`sea-assume-fold`), and
  integers that an equality fact fixes (#86, #87, #88). It also removes
  edges on which no execution reaches a check (`sea-prune-edges`, #89).
  Turn them off with `--lower-disable=assume-fold,prune-edges`; under
  `--reach` both are off unless `--reach-fold-facts`. New lines:
  `vc.assume-fold`, `vc.prune-edges`, `vc.assumed-branches`, and
  `split.assumed-candidate` with `--split-candidates`.
- Op folders fold by identities (`x & 0`, `icmp eq x, x`, `(x ^ k) ^ k`,
  `addr.cmp` of one address, ...), and fold constants of ops with `nsw` or
  `nuw` when the promise holds. `utils/check-semantics.py --fold` checks
  them. (#90)
- Ackermann: an exact decider settles read pairs whose indices are never
  equal or always equal, so fewer constraints are emitted; on by default
  (`--ack-decider=false` turns it off). (#95)
- Ackermann: `--ack-own` (with `--seam`) drops the constraint of two reads
  of different objects. It rests on A-INB and on a new premise A-OWN, both
  counted in the `assume.*` lines. (#96)
- Ackermann: `--ack-relax=all` solves without the constraints and adds, in
  rounds, those a model breaks, for diagnosis; `--ack-relax-mode=incremental`
  runs the rounds on one solver. The eager lowering stays the default.
  (#97, #98)
- Ackermann: `--ro-arrays=uf`, with the eager lowering off, gives the
  solver each read-only array as an uninterpreted function. (#98)
- Ackermann: `--narrow-offsets` compares reads of one object at the bits
  of its size; it rests on A-INB (`assume.a-inb`). `--narrow-check` proves
  the bound of each narrowed read. (#93)
- `--vc-defs=inline|nest|eq|hidden` shares or names the address sums of
  the query; the default `inline` leaves the query unchanged. `hidden`
  helps z3 on queries with many Ackermann address equalities. (#92)
- `--seam-check-cost` prints the rlimit count and time of each
  `--seam-check` query, to choose `--seam-check-rlimit` from data. (#83)
- `sea-seam-export` exports the lanes `seam-stride`, `seam-inb0-stride` and
  `seam-wide-stride`, and nodes for every operator OpSem makes (signed
  compares, extensions, division, bitwise ops, shifts, native multiply
  overflow). (#84)
- `utils/bench.py`: a job may declare `premise-class: B | C` when its
  counterexamples may leave their object; the gate then accepts a replay
  failure explained by a premise violation. The summary gains a
  `premises:` line. (#78)
- `sea-vc` prints `solver.z3-logic` when `--z3-logic` is given, next to
  `solver.z3-tactic`. (#98)

### Fixes
- `sea-drop-fresh-memhavoc` keeps a memhavoc in a loop, which may run more
  than once; `sea-vc` takes only loop-free programs, so no `sea-vc` query
  changes. (#116)
- `sea-drop-fresh-memhavoc` shows the range of the allocator's memhavoc in
  its object through selects, block arguments and a symbolic size. (#102)
- `sea-merge-asserts` and the replay name the failing assert after a held
  constant assert; before, `--replay-original` could report a mismatch
  (exit 6). (#86)
- `--seam-words=auto` no longer asserts in the width-term expansion. A
  constant memory widened by `--widen-memory` now passes the verifier.
  (#99)
- The Yices backend can apply uninterpreted functions; before, any
  application aborted. (#98)
- Expr: only a binary `bvsub` is a sum, and n-ary div/rem terms from z3
  fold into binary ones. (#95)
- `--check-align` with an alignment above 2^w no longer aborts in Debug
  builds, and the ALIGN premise of OpSem takes its mask at the address
  width. (#110, #115)
- The overlap test of a range of 2^w units or more is true; before, the
  width wrapped to 0 and the test was false. (#111)
- The Yices backend keeps its error string alive until it is printed.
  (#114)
- OpSem's firing premise keeps one sort per symbol. (#81)
- `utils/fuzz-seam.py` keeps the address when it writes a `ptr.add` as a
  `ptr.index` with a stride other than 1. (#100)
- Tests: the z3 memout limits of `test/VC/z3-errors.test` sit far from z3's
  baseline allocation (#77), and `test/VC/seam/fuzz-check.test` bounds its
  checks by rlimit instead of time (#79).
- The build puts the unit-test drivers (`sea-*-test`) in `test-bin/`
  instead of `bin/`, next to the tools; lit finds them there. An existing
  build directory keeps its old copies in `bin/` until they are deleted.
  (#117)
- `just release-check` expects the `seam.premises:` line, renamed in
  0.2.0, and passes each cmake argument through intact. (#118, #119)

### Performance
- `--seam` caches each node's view and does its modular arithmetic on
  64-bit integers when the width allows; queries are unchanged. Encoding
  of NKI gather N256 takes 25% less time. (#75)

### Docs
- Tutorials on the gate for z3's tableau pivot (`docs/tutorials/z3-pivot.md`,
  `z3-pivot-gate.md`). (#76)
- The rule table of the lowering's fact folds, with the argument that each
  rule is exact (`docs/design/FactFolds.md`). (#91)
- A playbook for a slow or timed-out VC (`docs/agents/slow-vc.md`) and a
  SLOW OR TIMEOUT section in `sea-vc --agent`. (#94)
- `--seam-stride` is documented as opt-in with three known wrong answers.
  (#113)

## 0.2.0

A minor release: what a verdict rests on changes (docs/Release.md, "What
a verdict rests on"): verdicts change for programs with a static
out-of-object access.

### Verdict basis: named pointer premises (`--seam`)
- The encoding of `--seam` rests on three named premises instead of A-INB:
  A-SEP (no memory cell is accessed through two different objects in one
  execution), A-NULL (no access of positive width through an object of size
  0, such as null) and A-NOWRAP (an access address does not wrap). The
  query is the same as before for every program without a static
  out-of-object access: A-INB's uses were exactly these premises. A-INB
  implies all three and stays the property a bounds check discharges.
  docs/design/SeaM.md, "Soundness stance".
- **Verdict change.** A static out-of-object access (an access whose every
  address alternative lies outside its object) no longer makes `sea-vc`
  encode again without the assumption. It is reported, classified by where
  it lands (another object; a gap cell that another provenance also
  reaches, `shared`, since A-SEP is pairwise; unknown; null; a gap), and the
  premises stay:
  an unsat then holds for the executions that reach none of those accesses.
  A program that does overflow into a neighbour can therefore change from
  sat to unsat (conditional on a premise it breaks if the access executes);
  programs whose flagged accesses sit on paths that never execute keep
  their verdict and solve faster. `--seam-out-of-object=fallback` restores
  the old rerun.
- Output changes (consumers: `utils/query-diff.py`, whose summary line `inb:` is
  now `premises:`, and `utils/fuzz-seam.py`
  read the new lines; query-diff still reads `seam.inb:` of an older
  binary):
  - `seam.inb: assumed|off ...` is now `seam.premises: assumed|off ...`
    (same fields; BRUNCH_STAT `seam.premises`, `-accesses`, `-conditions`).
  - `assume.a-sep: seam=<n> [drop-fresh-memhavoc=<m>]`, `assume.a-null:
    seam=<n>`, `assume.a-nowrap: seam=<n>`: the rewrites that rest on each
    premise, on every `--seam` run. `assume.a-inb: drop-fresh-memhavoc=` is
    now `assume.a-sep: drop-fresh-memhavoc=`.
  - The warning `vc.seam-inb-violated` and the lines `vc.seam-inb-violated:
    at ...` / `vc.seam-inb-violated-count:` are now the warning
    `vc.seam-out-of-object`, `seam.out-of-object-at: <loc> <op>: <reason>
    lands=<class> [with=<loc>]` and `seam.out-of-object: accesses= object=
    shared= unknown= null= gap=`. `vc.seam-inb-fallback-conditional` is now
    `vc.seam-fallback-conditional` (with the opt-in fallback only).
  - Counters: `seam.rule.premise.a-inb` is split into `premise.a-sep`,
    `premise.a-null`, `premise.a-nowrap`; `vc.ackermann-stride-a-inb-pairs`
    into `-stride-a-null-pairs` and `-stride-a-nowrap-pairs`. The rule trail
    (`--seam-trail`, `--seam-trace`, `sea-seam-export --firings`) names the
    premises `A-SEP`, `A-NULL`, `A-NOWRAP` instead of `A-INB`, and its run
    record says `"premises":<bool>` instead of `"inb":<bool>`.
  - `sea-seam-export` schema 3 (premise names and the `sea_vc` command of
    the lanes changed; lane names and the `ainb` key are unchanged).
- Flags: `--seam-inb` is now `--seam-premises` (`=0`: no premise, as
  before); new `--seam-out-of-object=report|fallback` (default report).

### Other
- Built as ISO C++17 without GNU extensions (`-std=c++17`), as LLVM is.
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
