# sea-mlir 0.1.0: the first release

This release freezes the functionality of sea-mlir so that applications can
build on a fixed version instead of the development branch. It is a
research prototype. Expect bugs. There is no promise that the text format of
the dialect or the options stay compatible across releases. Linux (x86-64)
is the only platform built and tested.

Every tool prints its version with `--version`. `sea-vc` also prints it as
the first line of every run (`tool.version:`), so every bug report should
include it (see [Reporting a bug](#reporting-a-bug)).

## What is in it

- **The `sea` dialect** (`include/sea/Dialect/Sea/*.td`): bit-vector
  scalars, pointers and abstract objects (`sea.malloc`, `sea.alloca`,
  globals), Mem-SSA memories (`!sea.mem<...>`, exact / aligned /
  unaligned / keyed layouts), memset / memmove / memhavoc, structs,
  intrinsics, `sea.ptr.index` (GEPs with their no-wrap flags), nondet
  sources, assume / assert, and print ops (`sea.print`, `sea.print.mem`,
  scopes; [design/Print.md](design/Print.md)).
- **The interpreter** (`sea-run`): the reference semantics of the dialect.
  It runs a program on given nondet values. `sea-vc` uses it to replay
  every sat model and to check each computed value against the model.
- **The VC generator** (`sea-vc`): SeaHorn-style BMC verification
  conditions, solved with z3 (default), Yices 2 or Bitwuzla
  (`--solver`). z3 is the supported solver of this release: the gates
  (lit, fuzz, vcc, benchmarks) run with it. Yices 2 and Bitwuzla are
  secondary: they work and are smoke-tested, the best solver differs per
  workload (Yices wins on some, times out on others), and both are
  optional at build time. Release binaries do not include Yices (see
  [Runtime requirements](#runtime-requirements)). It includes:
  - the VC lowering (`--lower`): placement, tracking, fat pointers,
    struct scalarization and the other passes of `lib/Transforms`;
  - SeaM (`--seam`), the object-aware rewriting of loads and stores
    ([design/SeaM.md](design/SeaM.md));
  - the pointer sort ([design/PtrSort.md](design/PtrSort.md));
  - Ackermann lowering of read-only arrays (`--ackermannize`);
  - counterexample replay and the program's print ops.
- **Profiles** (`sea-vc --config=<name|file>`): named option sets, in
  LLVM config-file syntax ([Profiles.md](Profiles.md)). The shipped
  `seam` and `seam-wide` profiles are examples of the format. They are not
  recommended defaults.
- **The reducer** (`sea-reduce`, `sea-reduce-test`): test-case reduction
  for sea programs ([Reducer.md](Reducer.md)).
- **Machine-readable output**: `--format=jsonl` (`sea-vc`, `sea-run`) and
  `--agent` (a terse guide to the options, output lines and exit codes of
  each tool).
- `sea-opt`: an `mlir-opt` driver with the sea dialect and its passes.

The exporter from LLVM bitcode to the sea dialect (`--horn-sea-mlir-export`)
is part of SeaHorn, not of this release.

## Building

### Dependencies

| Dependency | Version | Notes |
|---|---|---|
| LLVM + MLIR | the commit in `LLVM_REVISION` (`93f05d753ad3`, LLVM 24 `main`) | built with `-DLLVM_ENABLE_PROJECTS=mlir -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_INSTALL_UTILS=ON`, then installed. Other revisions are not supported. |
| z3 | tested with 4.12.0 and 5.3.3 | `-DZ3_ROOT=<install>`, and `-DSEA_Z3=<z3 binary>` for the tests |
| Yices 2 | tested with 2.6.4 (Ubuntu `libyices2-dev`) and 2.7.0 | optional: `-DSEA_ENABLE_YICES=AUTO` (default: used when found), `ON` (required) or `OFF`; `-DYICES2_ROOT=<install>` if it is not installed system-wide |
| Bitwuzla | 0.9 (>= 0.5, the C++ `TermManager` API) | optional: `-DBITWUZLA_ROOT=<install>` enables `--solver=bitwuzla` |
| Boost | >= 1.71 (headers only; tested with 1.83) | |
| GMP | with `gmpxx` (tested with 6.3) | |
| CMake, Ninja, a C++17 compiler | tested with clang 18 and lld | |
| lit, FileCheck | from the LLVM install (`LLVM_INSTALL_UTILS`) or `pip install lit` | tests only |
| Python 3 (+ PyYAML for `utils/bench.py`) | | tests, `sea-reduce-test`, benchmarks |

### The LLVM pin and version numbers

sea-mlir builds against one LLVM commit, recorded in `LLVM_REVISION`
(first line: the short hash; then the full commit and its date). From
LLVM 24 on, the major version of sea-mlir is the LLVM major it builds on:
`v24.0.0` is pinned to `llvmorg-24.1.0`, minor versions add features
within one LLVM major, and patch versions fix bugs or move to a later
24.1.x. The text format of the dialect can change with each major. This
release, `v0.1`, is a preview pinned to a commit of LLVM `main`. Between
releases the pin moves to a newer LLVM commit about once a month.
`just llvm-drift` prints the pin's age (with an llvm-project checkout,
`--llvm <dir>`, also the commits it is behind `main`, in total and in the
MLIR parts sea-mlir uses). `just release-check` warns when the pin is
older than 45 days and fails when it is older than 90 days.

### Configure, build, test, install

```bash
cmake -S . -B build-release -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 -DLLVM_ENABLE_LLD=ON \
  -DMLIR_DIR=<llvm-install>/lib/cmake/mlir \
  -DZ3_ROOT=<z3-install> -DSEA_Z3=<z3-install>/bin/z3 \
  [-DYICES2_ROOT=<yices-install>] [-DBITWUZLA_ROOT=<bitwuzla-install>] \
  -DCMAKE_INSTALL_PREFIX=<prefix>
ninja -C build-release            # build
ninja -C build-release check-sea  # lit: every test must pass
ninja -C build-release install
```

`just release-check` (`utils/release-check.sh`) runs the release gate:
- a Release build in `build-rc`, which takes its settings from
  `build-release/CMakeCache.txt` when no `-D` arguments are given;
- lit;
- an install to a temporary prefix;
- smoke runs of the installed tools: one sat program with replay and
  prints, and one unsat program with `--seam` through the installed
  profile;
- `bench.py gate smoke` when a benchmark checkout is connected
  ([Benchmarks.md](Benchmarks.md)).

### Release gates

A release candidate passes every gate below. Each one runs from a
sea-mlir checkout. `just release-check` runs lit, the installed smoke test
and, when a benchmark checkout is connected, the benchmark smoke gate. The
other gates are run by hand.

| gate | what it tests | how to run |
|---|---|---|
| lit | the regression suite (includes small fuzz runs) | `ninja -C <build> check-sea`; in `release-check` |
| installed smoke | the install: tools, profiles, shared libraries, replay | `just release-check` |
| fuzz | low-level code: VC vs interpreter, SeaM vs plain, placement | longer runs than lit's: `python3 utils/fuzz-vc.py --sea-vc <bin>/sea-vc --sea-run <bin>/sea-run --sea-opt <bin>/sea-opt --seed <s> -n 500`; `python3 utils/fuzz-seam.py --sea-vc <bin>/sea-vc --seed <s> -n 200 --accesses=<bases\|any\|wide\|cells\|circular\|ptrmem\|addr-forms>` (addr-forms: completeness, the SeaM counters must not depend on the address form); `utils/fuzz-place.py` (each `--agent` / `--help`) |
| vcc gate | the aws-c-common (vcc) suite: expected verdicts, replay | `python3 utils/bench.py gate quick --suite public/vcc` (needs `just bench-connect`) |
| benchmark canary | a few jobs per family (tag `canary`): verdicts and completeness counter bounds (Ackermann constraints / reads) | `just bench-canary` ([Benchmarks.md](Benchmarks.md), counter expectations) |
| benchmark smoke / quick | every connected suite (vcc, firedancer, stellar, nki, crafted) | smoke: `just bench-smoke`, which also runs in `release-check` when `bench/work/` is connected (with `--no-record`); quick: `just bench-gate quick` ([Benchmarks.md](Benchmarks.md)) |

### What is installed

| Path | What |
|---|---|
| `bin/sea-vc`, `bin/sea-run`, `bin/sea-opt`, `bin/sea-reduce` | the tools |
| `bin/sea-seam-export` | exports SeaM terms and rule firings for the Lean conformance checks ([SeamExport.md](SeamExport.md)) |
| `bin/sea-reduce-test` | `utils/reduce-test.py`, the interestingness tests and reduction loop for `sea-reduce`. It finds the tools next to itself. |
| `share/sea-mlir/profiles/` | the example profiles (`sea-vc --config=seam`) |
| `share/doc/sea-mlir/` | `README.md`, `CHANGELOG.md`, `docs/` (this file, the design notes) |

The following are not installed:
- `sea-expr-eval` and the `unittests/` drivers: they are test utilities.
- The C++ libraries and headers: there is no released API.
- The other `utils/` scripts. They are development tools that work in a
  source tree: `bench.py` (benchmarks), `query-diff.py`, the fuzzers and
  oracles (`check-semantics.py`, `fuzz-*.py`), `seam-trail.py` (reads
  `--seam-trail` output) and `run-seahorn-tests.py`.

### Runtime requirements

`sea-run`, `sea-opt` and `sea-reduce` need only the C/C++ runtime, zlib
and zstd. LLVM and MLIR are linked statically.

`sea-vc` and `sea-seam-export` (which links the VC library but runs no
solver) link the solvers as shared libraries:
- `libz3.so`, with `libgmp` and `libgmpxx`;
- with Bitwuzla, `libbitwuzla*.so` and `libmpfr`;
- with Yices, `libyices.so.2.*` (and `libpoly` if Yices was built with
  MCSAT).

Release binaries omit Yices for licensing reasons. z3 and Bitwuzla are
MIT-licensed; Yices is GPLv3, and a binary that links `libyices` would
have to be distributed under the terms of the GPLv3. Yices is available in
builds that link it: build from source with Yices installed
(`-DSEA_ENABLE_YICES=AUTO` or `ON`). Without it, `--solver=yices` and the
`--yices-*` options are errors, and `sea-vc --version` prints
`yices: not built`. A backend that runs the `yices-smt2` executable over
SMT-LIB2 (no linking) is planned before the first major release; until
then this is a known limitation of the release binaries.

The installed binaries keep the RUNPATH of the solver directories they were
linked against (`Z3_ROOT`, `BITWUZLA_ROOT`, a non-system `YICES2_ROOT`). An
install therefore runs in place without `LD_LIBRARY_PATH` as long as those
directories exist. To move an install to another machine, either install
the same solver libraries there and set `LD_LIBRARY_PATH`, or configure
with `-DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF` and install the solvers
system-wide. To check, run `ldd <prefix>/bin/sea-vc`: no line may say
`not found`. `sea-vc --version` prints the solver versions it loaded.

Release builds against an LLVM built with assertions have assertions on.
`--version` says so (`build: Release, assertions on`).

## Running

```bash
# verify: sat (counterexample, replayed, with the program's prints) or unsat
sea-vc p.mlir --lower
sea-vc p.mlir --config=seam                 # = --lower --seam (profiles/seam.cfg)
sea-vc p.mlir --lower --format=jsonl | jq -c 'select(.kind=="result")'
sea-vc --agent                              # options, output lines, exit codes

# run the interpreter on given nondet values (site=value)
sea-run p.mlir --input=0=5,1=-1
sea-run p.mlir --missing=zero --print=tree

# run passes (what sea-vc --lower runs; the module on stdout)
sea-opt p.mlir --sea-vc-lower

# reduce a program while a symptom persists (docs/Reducer.md)
sea-reduce-test reduce --mode verdict-differs \
    --a="--lower" --b="--lower --seam" in.mlir -o small.mlir
```

`sea-vc` reads a program in BMC form: loop-free, one `sea.assert` before
one `sea.halt`, and no critical edges. `--lower` establishes the rest of the
form that the VC needs.

### Output and exit codes

`sea-vc` prints `key: value` lines on stdout. The first line is
`tool.version: sea-mlir <x.y.z> (<git describe>)`. It goes to stderr with
`--smt2`, and under `--format=jsonl` it is a `stat` record with key
`tool.version`. Diagnostics go to stderr. `sea-vc --agent` describes every
output line, the jsonl records and the exit codes. In jsonl an object has
one id in every record (`stack.1`, as printed; the VC constant is the
`object` record's `symbol`, `obj.stack.1`), and addresses are numbers. In
short:

| exit | sea-vc |
|---|---|
| 0 | unsat, or sat confirmed by replay: the replayed run fails an assertion, or with `--ptr-companion=check` a split pointer load fails its A-PTRINT guard (`seam.ptrint-violated` / `seam.ptrint-unstored`) |
| 1 | error (bad input, not in BMC form, unsupported op, replay impossible) |
| 2 | sat, but the replay disagrees with the model (a value differs, or the run does not fail the property: `vc.replay-no-failure`): a bug (please report) |
| 3 | unknown (including `--timeout`) |
| 4 | `--seam`: sat that does not replay because the program leaves an object with the model's inputs. A-INB does not hold, the answer is inconclusive, and the program has a memory-safety error. |
| 5 | `--seam-check`: an invalid rewrite. A bug, so do not trust the verdict. |

Where a counterexample fails (`sea-vc` after `replay: assert-failed`,
`sea-run` after `status: assert-failed`):

- `assert-op`: the failing `sea.assert`'s own location. When
  `sea-merge-asserts` (part of `--lower`) merged several asserts into one,
  this is the original assert that failed. `at:` shows the same op with
  its whole MLIR location.
- `assert-at`: where the failing condition is computed. If the condition
  is computed by an op, this is that op's location. If the condition is a
  chain of `sea.and`, it is the first false conjunct's location. If the
  condition is a constant or a block argument (SeaHorn merges all its
  checks into one `assert false` reached by branches), it is the last
  executed op before the assert with a file location: usually the branch
  of the check that failed. Source locations win over named locations,
  which win over bare positions in the .mlir file, and print ops never
  count. Only the program can name where an `assert false` is reached
  (a reach target, a vacuity point). In a SeaHorn export, the branch into
  the error block carries the failing check's location, and with a single
  failure site the error block takes that location too, so assert-at
  names the check or the reach. The location can still be lost when the
  program has several failure sites and optimization removes all but one
  and folds its branch: assert-at is then the last located op before the
  assert, possibly inside an inlined callee (`verify.c:12:3
  inlined-at=main.c:31:7` for a reach on main.c:33). Harnesses that route
  every failure through one check macro keep a single failure site.
- `assert-path`: up to five recent branches, most recent first. In a
  program with source locations, the path lists only source locations:
  a `sea.cond_br` without one (the exporter left it unlocated, so its
  location is a position in the .mlir file) is shown at its condition's
  definition, else at the last op executed before it in its block, if
  either has a source location; otherwise it is left out, as is an
  unlocated `sea.br` (which decides nothing). A program without source
  locations (a handwritten .mlir) keeps its .mlir positions.

A location shows as
`<file>:<line>:<col> [inlined-at=<file:line:col>[,...]] [name=<name>]`.
The position is first: the source location if the op has one, else the
op's position in the input .mlir file. For an op inlined from a callee
(`loc(callsite(<callee> at <caller>))`, nested for deeper chains), the
position is the innermost one, in the callee, and ` inlined-at=` lists
the call sites, innermost first and the entry function's call last:
`callsite("f.c":3:5 at callsite("g.c":9:2 at "main.c":20:7))` shows as
`f.c:3:5 inlined-at=g.c:9:2,main.c:20:7`. ` name=<name>` follows when the
location has a name, and it is the last field.
For example, an assert with `loc("claim.no-oob")` on line 154 shows as
`p.mlir:154:3 name=claim.no-oob`, and one with
`loc("claim.no-oob"("gather.py":61:4))` shows as
`gather.py:61:4 name=claim.no-oob`. In jsonl, the position, the call
sites and the name are separate fields (`at` / `at_inlined` / `at_name`,
`op` / `op_inlined` / `op_name`; `*_inlined` is an array of
`file:line:col` strings, and the assert path's call sites are in
`path_inlined`, parallel to `path`). Print lines use
`at=<pos> [inlined-at=..] at_name=<name>`. The link that SeaHorn's
exporter can add to every location (`fused<"llvm @f %bb #i">[...]`, its
debugging mode) is never shown as a position or a name; `at:` shows it,
as part of the whole MLIR location.

`sea-run`: 0 when the run ends (with any status but error), and 1 on an
error. `sea-opt`: 0 ok, 1 on a parse, verify or pass failure.
`sea-seam-export`: 0 exported, 1 error (including a construct the format
cannot express), 3 exported with unexpressible rule firings.

## What a verdict rests on

A verdict is conditional. `unsat` means that the assertion holds for every
execution of the program that satisfies the assumptions below. `sea-vc`
reports the assumptions it used. It discharges none of them unless one of
the check jobs below is run.

- **A-INB** (`--seam`, on by default under `--seam` as `--seam-inb=true`):
  every access stays inside the object its pointer came from. Each run
  reports it as `seam.inb: assumed accesses=<n> conditions=<m>`, and
  `seam.rule.premise.a-inb` (`--seam-stats`) counts the rewrites that
  depend on it. **A-INB is not discharged in this release.** That needs a
  separate memory-safety (bounds) check of the program, which sea-mlir
  does not provide yet. A sat that needs an out-of-bounds access does not
  replay, and `sea-vc` exits 4. `--seam-inb=0` assumes nothing (it is
  slower). `--seam-check-inb-premise` (off) is a partial check that SeaM's
  decisions used only recorded A-INB conditions.
- **Front-end annotations are trusted.** They are named premises of the
  rewrites, not assumptions about the program:
  - ALIGN (`PremAlign`): the `align` of loads, stores, memset and memmove.
    The optional check is `--check-align`, which asserts every promise.
  - GEP promises (`PremPtrIndex`): the inbounds / nusw / nuw flags of
    `sea.ptr.index`, counted as `premise.ptr-index`. The optional check is
    `--check-ptr-index`, a paranoid job that asserts every promise used.
  - LAYOUT (`PremLayout`): the placement of objects (`obj.<k> = <address>`),
    which is a fact of the VC. Placement refuses objects that wrap the
    address space.
- **Bounded memset**: a symbolic-length memset is expanded up to a bound,
  with an explicit assume. It is reported as `assume.memset-bound`.
- **`--ptr-companion=assume`** (off by default) adds the A-PTRINT
  assumption, which is reported.
- `--seam-check` checks each SeaM rewrite against its stated premises with
  the solver. It finds rewriting bugs. It does not discharge A-INB.

### Programs exported by SeaHorn: known issues

The following concern only programs that reach sea-mlir through SeaHorn's
front end and exporter. Programs built directly in the sea dialect are not
affected.
- **sea-dsa**: sea-dsa can miss pointer equalities that are only assumed
  at a havoc cut. The memory regions of the exported program can then be
  wrong. This is a soundness hazard of the export path, outside sea-mlir.
- **Unified assumes**: export with `--horn-unify-assumes=false`. Unified
  assumes merge assumptions into the assert, and the check jobs
  (`--check-ptr-index`, `--check-align`) then report spurious failures.
- **Exporter gaps**: some exporters drop or mishandle constructs: large
  initializers, small constant tables, `llvm.cttz` / `llvm.ctlz` and
  `llvm.expect`. Fixes are in SeaHorn. Check the export's warnings.
- **TBAA is trusted**: SeaHorn's shadow-memory analysis uses the C/C++
  type-based alias annotations, so code that writes an object through one
  type and reads it through an unrelated one (a strict-aliasing violation,
  e.g. a mock that mirrors a library type with its own struct) can read a
  stale memory value and make a property vacuously true. The export warns
  `TBAA moved N shadow-memory use(s) past stores`; re-export with
  `--horn-shadow-mem-use-tbaa=false` to check whether a result depends on
  it. Write mocks against the real types.
- **One failure site per harness**: with several `__VERIFIER_error` calls,
  optimization can drop the location of the one that fails, and
  `assert-at` then names the last located operation before it. Route
  every failure through the harness's own check macro.

### Other known issues

- `--timeout` bounds the solver only. VC generation (the lowering,
  encoding and SeaM rewriting) is not bounded by it. Use an external
  `timeout`, and set `--mem-limit=<GB>`: there is no memory cap by default,
  and large exports have reached tens of GB.
- When the static A-INB check fails (`vc.seam-inb-violated`), `sea-vc`
  warns, names the accesses and falls back to `seam.inb: off`. The access
  it names can lie on an infeasible (unrolled) path.
- `--ptr-mode=address` can be much slower on code that aligns pointers by
  integer arithmetic.
- Release binaries do not include Yices (GPLv3; see
  [Runtime requirements](#runtime-requirements)): `--solver=yices` needs a
  build from source that links it.

## Defaults are not tuned

The defaults of `sea-vc` are the conservative ones. They are not a tuned
configuration:
- `--lower` and `--seam` are off. Every application so far passes both.
- These decisions are open:
  - **`--seam-wide`** (off): whether a wide load or store stays one term.
  - **`--widen-memory`** (0): the C applications use 4.
  - **Fat pointers, shadow memory and tracking**: placed today as MLIR
    passes in `--lower`, with the alternative of the memory model.

Each of these needs measurements before it becomes a default. Pass the
options you rely on explicitly, or in a profile of your own, and record
them with your results.

### Experimental options

These options are off (or 0) by default. They are modes kept for
measurement and are not part of the supported configuration:

| option | what |
|---|---|
| `--seam-wide` | wide accesses as one width term (open decision above) |
| `--seam-words=auto\|all`, `--words-check` | initial byte memories re-encoded as 8-byte words |
| `--seam-pull-index` | pull a PTR_INDEX node through its first ite |
| `--seam-resolve-ite-nodes=<N>` | resolve a loaded pointer through an ite of loaded values |
| `--seam-pull-concat=<N>` | resolve a split pointer load through a shared ite |
| `--seam-congruence` | decide addresses apart by divisibility |
| `--ptr-companion=exact\|check\|assume` | the pointer companion memory (PtrSort S6) |
| `--seam-stride` | R-STRIDE in Store-Map |
| `--seam-check-inb-premise` | the partial A-INB premise check (above) |
| `--mem=lambda` (+ `--lambda-*`) | SeaHorn's lambda memory encoding (a baseline) |
| `--sink-assumes` | one assume of the path conjunction before the assert (SeaHorn's unified assumes, done exactly in sea-vc); a solver tradeoff: faster or slower depending on solver and seed |
| `--yices-param=k=v`, `--yices-config=k=v`, `--yices-logic=L` | Yices search parameters and context; e.g. `--yices-param=random-seed=N`. Yices times vary widely with the seed: compare medians over several seeds |
| `--x-read-table*` (hidden) | a prototype guarded read table |
| `--eq-audit`, `--ack-dump-pairs`, `--seam-trail`, `--seam-trace`, `--dag-stats` | diagnostics of the encoding |

The paranoid and check jobs (`--check-align`, `--check-ptr-index`,
`--seam-check`) are off by default because they verify a different
property. Run them as separate jobs.

## Formal foundations

The SeaM pointer semantics and its erasure to bit-vectors are formalized
in Lean in the seam-lean repository
([github.com/agurfinkel/seam-lean](https://github.com/agurfinkel/seam-lean)).
The formalization is not part of this release. The `sea-seam-export` tool
exports SeaM terms for its conformance checks, which are run from seam-lean
(see its documentation): the terms of each register before any rewrite and
in each `sea-vc` configuration (lanes), the same terms after the solver
boundary, and every rule firing with its premise as a witness for the
proved rules. Format and lanes: [SeamExport.md](SeamExport.md).

## Reporting a bug

1. Include the version: the `tool.version:` line of the run, or
   `sea-vc --version` (it also gives the LLVM revision and the solver
   versions).
2. Include the exact command line, including every `--config` (the run
   prints `vc.config: <path>` for each), and the exit code.
3. Minimize the input with `sea-reduce-test reduce`
   ([Reducer.md](Reducer.md)). Tie the interestingness test to the
   symptom, for example two configurations that disagree, a diagnostic
   tag, or a result with its exit status. Attach the reduced program.
4. Exit 2 (replay disagrees with the model) and exit 5 (an invalid
   rewrite) are always bugs. So is a crash, or `<pass>.postcondition`.
