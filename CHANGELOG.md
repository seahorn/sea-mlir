# Changelog

## 0.6.0

A minor release: the output of `sea-vc` changes (one settled `result:` line
per run, the `solver-answer:` line, the header of `--smt2` dumps, new fatal
kinds) and `--z3-logic` refuses a query outside its logic
(docs/Release.md, "Version numbers of the 0.x preview"). It is the first
release with binary artifacts: two Linux x86-64 tarballs (sea-mlir, and a
bundle with SeaHorn) on the GitHub release. The Homebrew formulae and a
macOS build guide are included; the tap is not published and macOS has not
been built. The solver pins are those of 0.5.0 (z3
master `bbd46c9`, Yices 2.8.0, Bitwuzla 0.9.1). Triage verdict differences
against v0.5.0.

0.6.0 has not had a full check against the Lean model of SeaM. The last
full parity check was at `19a433a`. For this release, the fold of
top-level equalities (`--vc-fold-top-eqs`, on by default) was checked
against its correctness obligation in the code, and the two defects found
there were fixed (#163). The other changes since `19a433a` have not been
checked against the model. No known soundness defect is open
(docs/Release.md, "Formal foundations").

### Verdict changes
- Measured against v0.5.0 with the default configuration: vcc (228 jobs)
  and firedancer hard (120 jobs) give the same verdict on every job (vcc:
  227 unsat, 1 sat; firedancer: 64 sat, 26 unsat, 23 timeouts, 6 unknown)
  and no replay mismatch. On both, `txn-mut-datasz-nonstrict` ends in a
  memory error of `--replay-original`, a known premise case of that job.
- None are expected from the code changes. The default configuration
  builds the same queries as 0.5.0 (checked on vcc: 228 of 228 queries
  identical; a run without `--z3-logic` is byte-identical). The new
  options are off by default. (#151, #149)
- Under `--seam`, a `sea.mem.fill` of a byte memory is now one `mfill` term
  instead of one store per byte, so the query of a program with such a
  fill differs from 0.5.0 (the verdict is expected to be the same). No
  benchmark suite contains `sea.mem.fill` yet. (#148)
- A run that printed `result: sat` and then failed the replay now prints
  `result: unknown` (see the output changes). This changes the reported
  verdict line, not the solver's answer. (#150)
- With an explicit `--z3-logic=L`, a query outside `L` is refused. Before,
  z3 skipped the terms outside its logic and could answer a wrong `sat`
  (an array query under `QF_BV`). (#149)

### Output and command-line changes
- `sea-vc` prints one settled `result:` line per run. After the solver
  check it prints `solver-answer: sat|unsat|unknown`. `result: sat` and
  `result: unsat` (and the `BRUNCH_STAT` line) now mean a confirmed answer
  and come with exit 0: a sat whose replay confirmed it, with no failed
  `--seam-check` (exit 5) and no unproven `--narrow-check` (exit 7). Every
  other ending prints `result: unknown`: replay mismatch (exit 2), a
  premise failure and the other exit 4 outcomes, exit 6, replay
  impossible, a memory limit in the replay, a `--split` stopped by a failed
  part, and a `--check-inb` job with a premise-failed part. The error
  lines and exit codes are unchanged and say why. Before, `result: sat`
  was printed right after the check, before the replay. Consumers that read
  the first `result:` line must read `solver-answer:` for the solver's
  answer, and `result:` as the final verdict. `bench.py`, the
  `--check-inb` parts parser, `fuzz-seam.py`, `reduce-test.py` and
  `check-widen.py` are updated. In `reduce-test.py` an expected `--status`
  now overrides the error classification. (#150)
- `--smt2` dumps of z3 queries start with a header: `; sea.smt2-dialect
  ...`, one `; sea.smt2-nonportable <construct>: read by ...; <fix>` line
  for each construct another solver cannot read, and `(set-logic ALL)`.
  The in-process `--z3-logic` is recorded as `; sea.z3-logic L`, so the
  dump stays readable by z3. `utils/query-diff.py` ignores the `set-logic`
  line. A construct with no portable form (const arrays, lambda,
  quantifiers, ...) warns `smt2.nonportable.<name>`; a portable dump whose
  content is outside the given logic warns `smt2.logic-mismatch`. (#147)
- New fatal `vc.z3-logic-mismatch`, naming the construct, when a term given
  to a z3 solver with an explicit `--z3-logic` is outside the logic. The
  scan covers the verdict, `--reach`, `--ack-relax`, the parts of
  `--split` and the parts of `--check-inb`. Logic names follow SMT-LIB
  (`[QF_][A][UF][BV][LIA|LRA|...]`, `ALL`); unknown names, IDL and RDL are
  errors. UF is strict: `--check-inb` parts use `--ro-arrays=array` under
  an array logic without UF, and a logic with neither (`QF_BV`) is refused
  up front. A run that only prints `--smt2` is not guarded. (#149)
- `--seam-check` always prints `covered=` and `failed=` in its summary
  line. It proves `row.mfill-expand` firings up to 64 bytes; a larger one
  prints `seam-check-covered: ... theorem=RangeExpr.eval_mfill_eq_fillStores`.
  An unknown at or below the cap counts as a failure (exit 5). (#148)
- `sea-seam-export` schema 5 gains the `mfill` node,
  `{"op":"mfill","sort":"memory","args":[M,d],"data":[bytes]}`. The schema
  number stays 5; readers must handle the new op (the formal model's
  importer needs the matching update). (#148)
- Under `--seam`, byte-memory fills are no longer expanded
  (`sea-expand-mem-fill keep-bytes`). Word-layout and keyed fills are
  still expanded and counted in `vc.expand-mem-fill:`. (#148)
- Every `--agent` guide (`sea-vc`, `sea-run`, `sea-opt`, `sea-reduce`,
  `sea-seam-export`, `sea-reduce-test`) prints a `DOCS:` line naming the
  local docs root, and a `DOCS-WEB:` line naming the docs site and, for
  design pages that live only in the repository,
  `https://github.com/seahorn/sea-mlir/tree/<ref>/docs` (`<ref>` is the
  release tag for a release build, else `main`). (#153)
- `sea-vc` finds its profiles at run time: `$SEA_MLIR_PROFILE_PATH`, then
  `<exe>/../share/sea-mlir/profiles`, then the source tree, for a build
  tree only. `SEA_SOURCE_PROFILES_DIR` is gone. The tools no longer
  compile in a source or build path: `-fmacro-prefix-map` makes the
  `__FILE__` of `assert` relative in builds against an LLVM with
  assertions. (#153)
- On macOS, `--mem-limit` warns `vc.mem-limit-unenforced`: macOS accepts
  `RLIMIT_AS` but does not enforce it. (#155)
- `termForLog` (the text of a term in a log line) walks at most 65 nodes:
  a larger term logs as `#<id> (> 64 nodes)`, and a null term logs as
  `null` (docs/reference/logging.md). (#159)
- New fatal kinds `expr.marks-exhausted` (a traversal needed more mark bits
  than an `Expr` node has) and `expr.canonical-sums-reused` (a second
  `rewrite()` of one canonical-sums object). Both indicate a bug. (#159)
- `--vc-canonical-sums` and `--solver-nary-sums` change the text of the
  query that `--smt2` prints when they are on (see New flags). Off, the
  output is as in 0.5.0. (#151, #156)

### New flags and features
- `--smt2-dialect=z3|portable` (default `z3`). The `portable` dialect
  writes z3's multiplication-overflow predicates (`bvumul_noovfl`,
  `bvsmul_noovfl`, `bvsmul_noudfl`) as an exact double-width product with a
  range test, and `bvredor` / `bvredand` as comparisons; z3, `yices-smt2`
  and `bitwuzla` read it. It is an equivalent query, not the one z3 solves
  in-process, so it is opt-in. The lit suite gains the features `yices-bin`
  and `bitwuzla-bin`. (#147)
- `mfill(M, d, n, data)`, a native SeaM term for `sea.mem.fill`, following
  the formal model's `rangeFill` (docs/design/MemFill.md, "SeaM: the mfill
  term"). Scope: byte memories with full bytes. It shares the range
  machinery of `mset` / `mcopy` (A-INB counting of the destination, shadow
  memories without premises). Rules: `row.mfill-hit`, `-skip`,
  `-skip-disjoint`, `-split`, `-expand`; `wide.mfill-hit`, `-skip`,
  `-skip-disjoint`; the byte fallback for partial overlap; and
  `mem.fill-replicate` (an all-equal fill is an `mset`). An undecided read
  falls back to the in-order store expansion, built once per fill. At the
  solver boundary a leftover read becomes `ite((j-d) <u n, lookup(j-d),
  M[j])`. (#148)
- `--vc-canonical-sums` (off): an exact rewrite of the final solver query
  into one canonical linear form per sum (docs/design/CanonicalSums.md).
  Every maximal linear bit-vector term becomes `P`, `(bvadd P c)` or
  `(bvadd (bvadd P (bvnot N)) c+1)`, with `P` and `N` n-ary chains of leaves
  in one width-independent order; equalities with a sum side are balanced;
  sums stay full width with extracts on top; bitwise-only terms are left
  alone. It runs last on the query, so the in-process solvers and `--smt2`
  see the same term; side checks see the terms before it. Options, all with
  the defaults listed first: (#151, #157)
  - `--vc-canonical-sums-neg=bvnot|sub|auto`: how the negative chain is
    written. `auto` picks `sub` for Yices (its polynomial normal form does
    not see through `bvnot`) and `bvnot` for z3 and Bitwuzla.
  - `--vc-canonical-sums-eq=balance|unshared|keep`: `unshared` balances an
    equality only when a side is a sum used once.
  - `--vc-canonical-sums-inner=flatten|shared-leaf`: `shared-leaf` keeps a
    sum used more than once as one leaf.
- `--solver-nary-sums` (off): the z3 marshaller builds an n-ary `bvadd`,
  `bvmul`, `bvand`, `bvor` or `bvxor` as one n-ary application (z3's
  flat-associative declaration, as its SMT-LIB parser builds it) instead
  of a right-nested binary chain; Yices `bvadd` becomes `yices_bvsum`;
  Bitwuzla keeps its left-associated chain. It applies to the verdict
  solver, `--smt2`, `--split-smt2`, `--seam-check`, `--narrow-check` and
  `--reach`; the words, split and eq-audit checks keep the chain. (#156)
- The profile `canonical` (`sea-vc --config=canonical`, `profiles/canonical.cfg`):
  the configuration measured for the options above: n-ary sums, canonical
  sums with `neg=auto` and `eq=unshared`, `inner=flatten`, and for z3 the
  tactic S2 (som simplify; max-bv-sharing; bit-blast; aig; sat). S2 needs a
  pure bit-vector query: with arrays or UF left (`--ackermannize=off`) z3
  answers unknown, never a wrong verdict. It is an example, not a
  recommended default; the default configuration is decided on more
  suites. (#157)
- `Expr` nodes carry a 64-bit `marks` word (the node stays in its 64-byte
  allocation) leased through the scoped classes `ScopedExprSet` (one bit, a
  visited set) and `ScopedUseCount` (two bits, a use count saturating at
  2). There is no hash-set fallback: running out of bits is the fatal
  `expr.marks-exhausted`. `--vc-canonical-sums` uses them for its use
  counts and its shared-sum set. (#159)

### Binary releases
- A Linux x86-64 tarball for Ubuntu 24.04, and a bundle with the SeaHorn
  dev18 frontend (`docker/release.py`, `just binary-release`). The build
  is a pinned Docker build: LLVM 18.1.8 from apt.llvm.org (signer checked
  by fingerprint) and z3 4.12 for SeaHorn, Yices 2.8.0. A license gate
  checks every file; no GPL code ships. GMP, GMPXX and MPFR (LGPL) are
  runtime dependencies and are not bundled. Layout: `bin/`, `lib/`,
  `share/sea-mlir/profiles`, `share/doc/sea-mlir/docs`, `share/seahorn`
  and `RELEASE-MANIFEST.json`. (#155)
- A Homebrew tap (`packaging/homebrew`) with three formulae and separate
  bottles: `sea-mlir-llvm` (build-only), `sea-mlir`, and `seahorn` (depends
  on `sea-mlir`). Linux x86-64 bottles are built in a pinned Homebrew
  image with clang 18 and lld; measured sizes: `sea-mlir-llvm` 958 MB,
  `sea-mlir` 34 MB, `seahorn` 90 MB. Only `--brew-source formula` on a
  clean tree writes the publishable `Formula/`; other builds write
  `Formula-local/`, marked not for publishing. (#155)
- A first macOS build: `docs/BuildMacOS.md` and `utils/build-macos.sh`. The
  macOS bottles and the publication of the tap are not part of this
  change. (#155)
- Portability fixes for it: no hard-coded Bitwuzla rpath in installs;
  `ru_maxrss` is read in bytes on macOS; `ldd` only where it exists; the
  `sed` and `timeout` tests run on macOS too. (#155)
- Design and license audit: `docs/design/BinaryRelease.md`. (#155)
- Release builds report their tag: `docker/release.py` computes the
  version with the same `git describe` as the CMake build, so a tarball of
  `v<version>` prints `(v<version>)` and its `--agent` DOCS-WEB links name
  that tag; a new smoke check, `version-tag`, checks both. The publishing
  steps (prerelease, `release-test.yml` on GitHub's Ubuntu 24.04 and
  26.04 runners, promotion) are in docs/Release.md, "Publishing a binary
  release". (#162)

### Performance
- Canonical sums and n-ary sums, measured on 44 firedancer and vcc jobs
  that take more than 5 s (z3, 3 seeds): `--solver-nary-sums` is neutral
  under z3's default pipeline (its first simplify flattens anyway), and
  with the custom tactic S2 it solves 132 of 132 runs against 130 of 132.
  The `canonical` profile solves 132 of 132 runs against 130 of 132 with
  the defaults, with a total time of -40%. Slower: `h2fc-settings-proof`
  (225 to 625 s) and `step-reach-sz` (11 to 45 s); Yices and Bitwuzla are
  faster or equal. (#156, #157)
- `txn-mut-writable-nonstrict`: adder bits 246k with the default, 195k
  with `--vc-canonical-sums`, 86k with `eq=unshared`. (#157)
- A 4 KiB constant initializer under `--seam` is a DAG of 19 nodes with
  `mfill`, against 24,889 with one store per byte. z3 takes 1.1 s on a 4
  KiB skip against 21 s for the store chain. (#148)
- `termForLog` walks at most 65 nodes; the unused visited set of
  `contains()` is dropped. The attested perf change of the scoped marks is
  +0.2% on vcc. (#159)
- Against v0.5.0, default configuration: on the 90 firedancer hard jobs
  both versions solve, z3 CPU time, wall time and peak RSS in total are
  equal (4870 s vs 4871 s CPU); no job moves by more than 5 s. On vcc, CPU
  time in total is within 4% (the runs shared the machine with other
  benchmarks, so small differences are noise).

### Fixes
- `--vc-fold-top-eqs` (on by default) could drop a fact from the query: for
  a top-level `concat(..) = c` with a part that mentions an object, the
  other parts were replaced inside their own fact, so `x + y = 7` in
  `concat(x + y, addr(o)) = c` left the query. The folded query was weaker
  than the VC: an `unsat` stayed correct and a `sat` failed its replay
  (`result: unknown`), but a `--smt2` or `--split-smt2` dump could be sat
  for an external solver on an unsat VC. The fact now stays; other uses
  are still replaced. No vcc query changes (228 of 228 identical). (#163)
- `utils/seavc_flags.py`: a failed or timed-out probe of `sea-vc`'s hidden
  options made query-diff, encoding-diff, cover-diff and memops-equiv
  compare folded queries without saying so; it is now an error with each
  tool's error status. (#163)
- The build and `check-sea` pass with GCC 13 as well as clang 18 (672 of
  672 tests in both). The compiler no longer decides the shape of the
  query: the load split in `SeaMRules.hh` and the `smulo` AND in
  `ExprOverflow.hh` built terms inside one call's arguments, so node ids
  followed the compiler's argument evaluation order. Verdicts were the same
  either way. Queries of a GCC build equal those of a clang build (186 of
  186). About a dozen other sites whose output shape depends only on
  argument evaluation order remain. (#154)
- `--z3-logic=QF_BV` on a query with arrays answered a wrong `sat` and the
  replay then ran out of memory; the query is now refused. (#149)
- `sea-vc` no longer prints `result: sat` before a replay that then fails.
  (#150)

### Tests and docs
- `test/VC/seam/mfill-padded.test`: a 64-entry table of 3-byte fills at
  stride 4 with the padding byte never written (the shape SeaHorn exports
  for a table of structs), in sea and seaimp form. docs/Benchmarks.md
  lists the `mem-fill` and `canary` tags. (#152)
- Every optional lit feature goes through `build_feature()`;
  `Tools/lit-build-features.test` fails on a direct
  `config.available_features.add()`. A Debug run no longer shows
  `Tools/no-source-paths.test` as unsupported. (#158)
- `Tools/no-source-paths.test` (lit feature `release-build`) checks that
  the five tools contain no source or build path. (#153)
- The addr-range semantics oracle in `unittests/AddrRange` models ARM's
  stride-1 index provenance, which its contract allows; the old evaluator
  rejected allowed cases (47 of 100000 random cases under clang). A pinned
  regression pair is added. (#154)
- New design notes: `docs/design/CanonicalSums.md`,
  `docs/design/BinaryRelease.md`; `docs/design/MemFill.md` gains the
  `mfill` term. (#151, #155, #148)

## 0.5.0

A minor release: the solver pins change (a dependency switch with behaviour
change) and the `sea-seam-export` output changes incompatibly (schema 5)
(docs/Release.md, "Version numbers of the 0.x preview"). Triage verdict
differences against v0.4.0, which stays the compare point on the old pins
(z3 5.3.3, Yices 2.7.0).

### Verdict changes
- None are expected from the code changes. The pin switch was measured
  against v0.4.0: lit passes, the 228 vcc jobs give the same verdicts, and
  firedancer hard (120 jobs) has no verdict disagreement and no replay
  mismatch. Two jobs are newly solved (`txn-mut-programid-zero`,
  `txn-mut-writable-nonstrict`) and none is lost. (#144)

### Output and command-line changes
- Solver pins: z3 moves to the master commit `bbd46c9` (it reports 5.4.2;
  a master commit, not a release) and Yices to 2.8.0. Bitwuzla stays 0.9.1.
  `sea-vc --version` shows z3 5.4.2.0 and yices 2.8.0. Models and timings
  can differ from v0.4.0 for solver reasons. (#144)
- `sea-seam-export` writes schema 5. An object is a segment node,
  `{"op":"segment","sort":"ptr","bits":n,"name":"obj.<k>","addr":"<numeral>"}`,
  and the layout equations `obj.<k> = a` leave `side`; `erased_side` keeps
  them, as the solver sees them. Schema 3 readers must be updated. (#145)
- The counter `vc.fold-top-eqs-skipped-ptr` is renamed
  `vc.fold-top-eqs-skipped-base`. `--vc-fold-top-eqs` now skips only a fact
  that mentions an object (outside a memory read); facts on loaded
  pointers, their integer values and differences are substituted. Without
  `--seam` there are no segments, so under `--simplify --vc-fold-top-eqs`
  pointer facts are now substituted too; the substitution is exact and the
  option is opt-in. (#145)
- Diagnostics flush stdout first, so the output of `2>&1` is in program
  order. Before, a diagnostic could appear inside a line printed earlier,
  at a position set by the buffer size. Five tests that checked the old
  order now check the new one. (#145)

### New flags and features
- Release binaries include Yices, which is BSD-3-Clause from 2.8.0. The
  release ships Yices' licence and third-party notices (MiniSat, CPython
  timsort). Packaging refuses a z3 that is not the pinned commit and a
  `sea-vc` without the pinned z3 and Yices. The Docker build builds Yices
  by default. (#144)
- `PTR_SEGMENT(k, a)` (`docs/design/PtrSegment.md`, steps 1 and 2): under
  `--seam`, a placed object's address is a pointer term that holds the
  object and its address. It erases at the solver boundary to the constant
  `obj.k`, and the boundary adds `obj.k = a` to the query. The OpSem side
  condition `ADDR(obj) = a` is gone. No rewrite rule looks inside a
  segment, so no rule decides from `a`; `--seam-check` stays parametric.
  Queries are identical to v0.4.0 up to the order of conjuncts. (#145)
- `--x-fold-top-eqs-no-base-skip` (hidden, testing only) turns the object
  skip of `--vc-fold-top-eqs` off and prints the warning
  `vc.fold-top-eqs-no-base-skip`. (#145)

### Performance
- Against v0.4.0 on the new pins, firedancer hard jobs solved on both sides
  take about 23% less wall time and 16% less RSS in total. On vcc, z3 CPU
  falls by 3%. (#144)
- Some QF_BV queries are slower on this z3: its `max-bv-sharing` preamble
  step now runs (it was a no-op in 5.3.3, from a missing initialisation).
  Examples are vcc `hash_table_eq` and firedancer `step-claims-frag` and
  `txn-pf-legacy-*` (1.4 to 2 times slower). The cause is how z3 shapes
  linear sums, and it is being addressed in z3. No `sea-vc` option changes
  it. (#144)

### Docs
- `docs/design/PtrSegment.md`, the plan for object identity in the pointer
  term. (#145)
- Docs drop the GPL text for Yices and the planned `yices-smt2`
  workaround. (#144)

## 0.4.0

A minor release: the default slot placement and the default query fold
change the queries, a `--seam` provenance fix changes verdicts, and new
lines, options and an exit status appear (docs/Release.md, "Version numbers
of the 0.x preview"). Triage verdict differences against v0.3.0. The solver
pins are those of 0.3.0 (z3 5.3.3, Yices 2.7.0, Bitwuzla 0.9.1).

### Verdict changes
- **Verdict change.** Under `--seam`, pointer arithmetic (`ptr.add` /
  `ptr.index`) keeps its operand's provenance whatever the offset mentions;
  an access that reaches another object this way, e.g. `p + (to_int(q) -
  to_int(p))`, was sat and is now unsat under A-SEP, and `--check-inb`
  reports it. A chain of such ops takes the view of its pointer operand, and
  an ite of leaves with one base gives that base. Before, an offset that
  mentioned a base left the access without a view and a memmove's range test
  stayed open. The h2 flow-control ring jobs (`h2fc-blowup-tx-reach-ring`)
  no longer run out of memory: undecided memmove range tests fall from 2052
  to 0 and the encoding to 0.12 M terms. (#140)
- **Verdict change.** A sat model that breaks a promised GEP stride is
  answered at once; before, `sea-vc` reran the query without the stride
  rule. The replay classifies it: exit 0 when the assertion fails with every
  value matching, exit 4 when a GEP promise breaks at an access in the run
  (the broken promise is the answer), exit 2 (`vc.replay-mismatch`,
  `vc.replay-no-failure`) otherwise. Jobs whose rerun did not finish now
  answer. `--reach` keeps its rerun (`vc.ack-stride-reach-retry`). (#128,
  #130)

### Output and command-line changes
- `--place-slots=aligned` is the default: each placed object owns a
  power-of-two slot aligned to its size, so an object's base and its offsets
  occupy disjoint bits. `--place-slots=none` is the old packed placement.
  Addresses in queries, models and replays change. (#135)
- `--vc-fold-top-eqs` is on by default. Top-level equalities `x = numeral`
  and Boolean facts are substituted into the query, and the folded
  definitions go to a model trail, so a sat still replays. The folded query
  is equisatisfiable with the VC, not equivalent. The same folded query is
  written under `--smt2`. Facts from which a pointer address is reachable
  are skipped (`vc.fold-top-eqs-skipped-ptr`); the concat-ite hoist is bound
  to numerals (`--vc-fold-concat-ite`); the status line is
  `vc.fold-top-eqs-status`. The query tools (`query-diff`, `encoding-diff`,
  `cover-diff`, `memops-equiv`) turn the fold off in `utils/seavc_flags.py`,
  and a fold flag given by the user wins. `--vc-fold-top-eqs=false` restores
  the unfolded query. (#138)
- `--seam-congruence` (rule `cmp.congruence`) is on by default;
  `--seam-congruence=false` turns it off. The rule decides two addresses
  apart when their linear difference is `sum(c_k x_k) + d` with 2^t dividing
  every `c_k` but not `d`. It is exact and needs no premise. On
  step-reach-sz `row.store-skip` rises from 269 to 1870. (#126)
- A budget stop has its own exit status 8 and the bench verdict `budget`
  (see `--max-nodes` and `--max-rss` below). With `--check-inb`, status 8
  ranks among the part statuses. (#139)
- `assume.memset-bound` is counted when `sea-expand-memset` records the
  bounds it assumes (`sea.memset_bound = {count, bound}`), not from the
  assumes left after the lowering. A bound that a later fold erased is now
  reported, and a bound that a fold made redundant is still counted. On vcc
  9 jobs report `count=3` instead of `count=1` (the fat-pointer lowering
  turns one memmove into three). Queries are unchanged. (#122)
- An allocation of symbolic size prints the ledger line
  `assume.symbolic-size-bound: allocs=<n> room=<r>`, counted at placement;
  169 of 228 vcc jobs print it. Queries are unchanged. (#124)
- `bv.eq.low-zero` also decides `A + d = B` when both sides have low zero
  bits and `d mod 2^z != 0` (counter `bv.eq.low-zero2`). It is exact and
  needs no premise; vcc queries are byte-identical. (#125)
- The interpreter's `interp.ptr-index-promise` warning is reported at the
  executed access whose address carries the poison of a broken GEP, not
  where the GEP is evaluated, and it says when the pointer went through
  memory. It is gone where no access uses the pointer. (#129)
- The `vc.ack-stride-promise-broken` and `vc.ack-stride-promise-broken-access`
  lines report a stride-broken model; `vc.ackermann-stride-idle`,
  `-exec-unknown` and `-exec-overflow` count the pairs left out of the
  stride-broken judgment. `utils/bench.py` labels these runs
  `promise-broken`. (#128, #130)
- `--check-inb` parts use Yices when the build has it; an explicit
  `--solver` wins, and a z3-only option (`--z3-tactic`, `--z3-logic`, ...)
  keeps z3. z3 parts get `--ro-arrays=uf` unless `--ro-arrays` is given.
  The job lowers for the parts' solver (mul-overflow intrinsics). The line
  `check-inb.solver:` states the choice and why. On the ring job (141 parts)
  Yices takes 324 s and z3 with `--ro-arrays=uf` 2166 s with the same
  verdicts; before, z3 left 101 parts undecided after 1200 s. (#141)
- `--solver=bitwuzla` no longer sets `produce-unsat-assumptions`. A caller
  that reads a core sets `SolverConfig::unsatCores`; `unsat_core()` on a
  solver built without it is the error `bitwuzla.unsat-core`. (#121)

### New flags and features
- `--check-inb`: a job that discharges A-INB (every access stays in its
  object), the premise behind the `--seam` encoding, with a separate check.
  `sea-check-inb` puts a `sea.ptr.dereferenceable` assert before every
  access and drops the program's own asserts; constant offsets into objects
  of constant size are proved statically; `sea-split-asserts` makes one
  program per remaining assert; each part runs with `--seam
  --ackermannize=off`. The line `check-inb.discharges: a-inb a-sep a-null
  [a-nowrap] under=<assumptions>` names what the result rests on. The job
  directory is self-contained and relocatable (`manifest.json`,
  `parts.cfg`); `--check-inb-keep` keeps it, and `--check-inb --vcgen-only`
  writes it without solving. A failed premise part is the job's answer, with
  exit 4. Design: `docs/design/CheckInb.md`. (#123, #124)
- `--symbolic-size=assume|assert|refuse` controls the room of an allocation
  of symbolic size (`--max-symbolic-size`, default 4096). In assert mode the
  bound is an assert marked `sea.premise`; a failure is reported as
  `premise.failed: symbolic-size-bound size=<n> room=<r> at=<alloc>` with
  exit 4. `--check-align` and `--check-ptr-index` keep premise asserts. The
  bench tool labels these runs `premise-failed`. (#124)
- `--ack-cells-plan` counts a cells plan for the eager Ackermann lowering:
  the objects come from ARM (the address-range map), and the plan names the
  reads and cells each object needs. It reports only; no query changes.
  (#136)
- `--ack-arm` drops the Ackermann constraint of two reads whose indices ARM
  places in different objects; the pair is counted. It rests on A-SEP.
  (#137)
- `--mem-trace` writes a heartbeat with live terms, top operators, rules
  fired and the source sites that made the most terms. `--max-nodes` and
  `--max-rss` turn an out-of-memory kill into a stop with a census,
  `result: unknown` and exit status 8. A budget never replaces a verdict.
  ExprFactory watches node creation with one comparison per new node.
  Recipe: `docs/agents/blowups.md`. (#139)
- `seaimp` gets variables and memory. Programs declare and assign
  variables, and `sea-lower-seaimp` lowers them to `sea` through Mem2Reg;
  unused variables are accepted and pointer variables start null. Memory is
  reached only through region ops (`seaimp.alloca`, `seaimp.malloc` and
  `seaimp.global` name the region of their object). `sea-erase-mem-ssa`
  rewrites one memory's SSA to a region and reports refusals as
  `erase-mem-ssa.unsupported`. `sea-vc` lowers `seaimp` one program at a
  time, in module order. `sea.nondet` sites go through
  `NondetSiteOpInterface`. (#131, #132)
- `sea.mem.fill` writes constant cells from an address, with
  `seaimp.mem.fill`, a constant start for regions, `sea-expand-mem-fill`
  and interpreter support. (#132)
- `sea-split-asserts`, `sea-check-inb` and the bench configurations
  `fold-top-eqs` and `ack-arm`. (#123, #138)

### Fixes
- The interpreter binds block arguments as a parallel copy. A back edge
  that swaps two arguments gave both the same value, which missed an
  out-of-bounds access, printed the wrong provenance, or lost a memory
  buffer. It still rejects one memory version passed to two arguments of an
  edge. Only `sea-run` on programs with loops was affected. (#127)
- The interpreter's GEP promise breaks only at an access and follows LLVM
  poison through value ops, structs, `box.all`, `is_modified` and memory;
  marks are kept in a stable order. (#129)
- `strideBroken` counts only pairs whose two reads execute in the model.
  Before, a model could look stride-broken from reads it never executes and
  `sea-vc` reran without the rule (on firedancer txn-mut-programid-zero the
  rerun did not finish). (#128)
- `--vc-fold-top-eqs` fixes: a refused top concat stays memoized, and
  every fold fact derived from a pointer address is skipped. (#138)
- `--check-inb` fixes: objects are sized from the placed segments,
  `assume.memhavoc-null-noop` is reported when a null check folds away and
  counted only for may-be-null pointers, `a-nowrap` is decided from the
  program's objects, boxes are lowered before the instrumentation, the
  failing check is named by assert op, and diagnostic options stay out of
  the parts. (#123, #124)
- `assume.symbolic-size-bound` adds to an existing count at placement, and
  a pruned premise failure is reported. (#124)
- Tests: the Bitwuzla core checks of the factory test are guarded (#121).
  A test that needs an optional part the build lacks (`REQUIRES: yices`,
  `bitwuzla`, `z3`, `sea-git-tree`, or `UNSUPPORTED: yices`) is excluded from
  discovery, so a good run is all passes; disabled tests still show as
  UNSUPPORTED. (#142)
- `utils/output-diff.py` in full mode skips four jobs that do not terminate
  (lambda on `nki-dyn-add-M4H4-good`, `nki-dyn-add-M4H4-nopre` and
  `nki-scatter-K4-int8`, reach on `array_list_swap`; each ran past 1200 s).
  Skipped jobs are reported (`nonterminating-skipped`,
  `skipped-nonterminating`); `--include-nonterminating` runs them. Quick
  mode is unchanged. (#133)

### Performance
- `--solver=bitwuzla` skips the preprocessing passes and the assumption
  per assertion that `produce-unsat-assumptions` forced on. On firedancer
  step-reach-sz, with `--lower --seam --widen-memory=4 --ackermannize=on`,
  a run that timed out at 150 s is sat in 12.9 s; h2fc-settings-proof goes
  from 12.6 s to 9.4 s. (#121)
- The h2 flow-control ring job (`h2fc-blowup-tx-reach-ring`) is sat in 41 s
  instead of running out of memory (#140); its `--check-inb` parts take 324 s
  with Yices (#141).

### Docs
- The blowup recipe (`docs/agents/blowups.md`) with a worked example on an
  h2 flow-control job. (#139)
- `docs/design/CheckInb.md` (#123); the core contract and the reason the
  option is off in `docs/design/Bitwuzla.md` (#121).
- Region ops, linear versions, allocation forms and `sea.mem.fill` of
  `seaimp`. (#131, #132)
- SeaHorn exports global initializers of any size as `sea.mem.fill`, so
  initializers over 16 KiB are no longer listed as an exporter gap in the
  known limitations; pointer and constant-expression initializers are still
  dropped. (#134)

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
