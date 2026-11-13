# Sea-MLIR contributor gates

These gates protect changes to **Sea-MLIR itself**: its semantics,
encodings, performance, release artifacts and repository contents. Run
them from a Sea-MLIR source tree. Each answers a different question, so
passing one does not replace the others.

For a gate that protects application code, follow the
[application verification workflow](index.md). That workflow includes the
property, harness and regeneration from changed source; the contributor
checks below test the verifier and its distribution.

| gate | question | command |
|---|---|---|
| lint | is the change formatted, spelled, licensed and free of private paths? | `just lint` |
| lit | do the regression tests and the oracles pass? | `ninja -C <build> check-sea` |
| release check | does an installed build work? | `just release-check` |
| fuzz | do the query, SeaM and placement agree with the interpreter on random programs? | `utils/fuzz-vc.py`, `utils/fuzz-seam.py`, `utils/fuzz-place.py` |
| benchmarks | do the applications keep their expected verdicts and counter bounds? | `just bench-smoke`, `just bench-canary`, `just bench-gate quick` |
| query diff | does a change that must not change queries keep them? | `utils/query-diff.py` |
| pass validation | does a lowering change keep which check sites can be reached? | `utils/cover-diff.py` |

## Lint

`.pre-commit-config.yaml` defines the checks every change passes; `just
lint` runs them on every file with `prek` or `pre-commit` (`pip install
pre-commit`), and `pre-commit install` runs them on the staged files of
each commit.

- C and C++: clang-format 18.1.8, LLVM style with MLIR's settings
  (`.clang-format`), for the whole tree, the code ported from SeaHorn
  included. The major stays at 18, the toolchain of the build; a newer
  major reformats the tree and is its own `style:` commit.
- Python: `ruff check` and `ruff format` (`ruff.toml`: Python 3.11,
  88 columns).
- Shell scripts: shellcheck. Workflows: actionlint.
- Spelling: typos. A correct word it flags goes into `_typos.toml` with
  the reason.
- Licensing: the SPDX headers (`just spdx`) and `reuse lint`.
- Privacy: `utils/privacy-check.py`, the generic patterns. Project terms
  that must not be published live outside the repository: put them in an
  untracked `.privacy-terms` file (one regex per line), or pass
  `--terms FILE`.
- Whitespace, final newlines, YAML / TOML syntax and merge markers.
  The vendored KaTeX files and the license texts are excluded; the
  generated dialect reference skips the whitespace fixes.

Hooks are pinned by commit. `pre-commit autoupdate --freeze` moves them
to their newest tags; keep clang-format on major 18 and review the
diff.

Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/):
`type(scope): subject`, header at most 72 characters, types `build`,
`chore`, `ci`, `docs`, `feat`, `fix`, `perf`, `refactor`, `revert`,
`style`, `test` (`.commitlintrc.yml`).

## lit

Every test must pass. The oracles among them are protected: they are not
weakened to make a change pass ([Sea-MLIR contributor rules](rules.md#tests-and-oracles)).
lit also runs small fuzz campaigns.

## The release check

`just release-check` builds Release in `build-rc`, runs lit, installs to a
temporary prefix, and runs the installed tools: a `sat` program with
replay and prints, and an `unsat` program with `--seam` through the
installed profile. If a benchmark checkout is connected, it also runs the
benchmark smoke gate.

## Fuzzing

A release candidate runs longer campaigns than lit:

```bash
python3 utils/fuzz-vc.py --sea-vc <bin>/sea-vc --sea-run <bin>/sea-run \
    --sea-opt <bin>/sea-opt --seed <s> -n 500
python3 utils/fuzz-seam.py --sea-vc <bin>/sea-vc --seed <s> -n 200 \
    --accesses=<bases|any|wide|cells|circular|ptrmem|addr-forms>
```

`fuzz-vc` generates programs in BMC form and checks that z3 and Yices
agree, that every `sat` replays with every value equal to the model's, and
that every `unsat` survives random interpreter runs. `fuzz-seam` compares
SeaM with the plain encoding. Its `addr-forms` mode checks completeness:
the SeaM counters must not depend on how an address is written. A failure
is a bug; reduce it ([Minimizing a problem](minimizing.md)).

## Benchmarks

The benchmark suites live in their own repository and are connected to a
checkout on demand ([Verifier benchmarks and gates](../reference/benchmarks.md)):

```bash
just bench-connect        # clone or link the suites into bench/work/
just bench-list           # families, jobs per tier and expected verdict
just bench-smoke          # every smoke job (under 5 s each)
just bench-canary         # a few jobs per family, with counter bounds
just bench-gate quick     # smoke and quick jobs (under 60 s each)
```

A gate fails on a verdict other than the expected one, on a replay
mismatch, when two configurations disagree on `sat` and `unsat`, and when
a counter leaves its bound. A small gate over six NKI jobs:

```console
$ python3 utils/bench.py gate smoke --suite public/nki --jobs 'gather-hw-N4D3K2' --no-record
gate: smoke configs=default jobs=6 runs=6 par=4
...
verdicts: sat=2 unsat=4
failures: 0
counter-bounds: 12
counter-failures: 0
slow: 0
wall: 6
gate: pass
```

Without `--no-record`, each gate run appends a record to
`$SEA_BENCH_HOME/gates/<date>-<commit>.yaml` (default
`~/.cache/sea-mlir-bench`); logs and results go to `$SEA_BENCH_HOME/results`.
The gates run at most 4 jobs in parallel by default, with a 16 GB memory
cap per job.

## Query diff

A change that must not change the queries (a refactoring, a new
representation) runs `utils/query-diff.py`: two `sea-vc` binaries over a
corpus, with the same arguments.

```bash
python3 utils/query-diff.py --old <base>/sea-vc --new <head>/sea-vc \
    bench/work/public/vcc --args="--lower --widen-memory=8 --seam" --par 4
```

Each job gets one class. Symbols of the two queries are matched by name.
A symbol that only one query declares is a free input, unless it is named
`obj.<name>` (the address of a placed object) and a top-level conjunct of
that query (through `and` and the bodies of `let`) defines it as a numeral
(`(= x #x..)`): then it is replaced by the numeral before any check
(`subst=<n>` on the job's line). The replacement is exact, since
$\exists x.\, x = c \wedge \varphi \equiv \varphi[c/x]$. The other
lowering may have erased the object. A program input defined on one side
only (`nd.<site>`) stays free.

| class | what it proves |
|---|---|
| `identical` | the queries are byte-identical |
| `order` | they are equal up to the order of commutative arguments and of the top-level forms |
| `equiv-sat` | both are sat, and z3 proves that the same assignments satisfy both. With `reason=derived`, one sat check was unknown and its result follows from the other's and the equivalence |
| `both-unsat` | z3 proves each query unsat by its own check, or (`reason=derived`) one by its own check and the other, whose check was unknown, through a proof that the two are equivalent: the verdict is kept, nothing more (any two unsat queries are `both-unsat`) |
| `DIFF` | one is sat and the other unsat, or z3 finds an assignment on which they differ |
| `unknown` | a z3 check ran out of its resource limit or time, or z3 gave up: no proof either way |
| `error` | `sea-vc` or z3 failed, or the queries cannot be compared (a symbol with two sorts) |

On unsat queries only `identical` and `order` show that the change kept
the query. `both-unsat` is a pass of the gate, but no evidence about the
change beyond the verdict. The line after the totals counts the jobs by
what they prove:

```text
evidence: equiv-sat=0 identical+order=2 both-unsat=225 (verdict only)
```

`--strict` makes `both-unsat` a failure: when some job is `both-unsat`,
the run prints `strict: fail both-unsat=<n> ...` and exits with 3.

`--project-blocks` compares the queries up to their block guards
(`^bbN_k`): `(exists g. OLD) <=> (exists g'. NEW)`. A change that only
renumbers blocks is then `equiv-sat by=exists`. The check by names runs
first; the quantified check runs only when it finds a difference.

Skolem hints replace the quantified check when both queries carry block
ids (`; sea.block-ids` comments: `sea-vc --smt2` on a program lowered
under `--reach`, such as R_T; [Block ids](../reference/output-formats.md#block-ids)).
Each guard of one query gets a term over the guards of the other: the
conjunction of the guards whose blocks hold one of its ids; `false` when
no block holds one and all are input ids (the other lowering erased the
block); for an edge block whose edge the other side did not split, the
conjunction of the guards of its two ends (not exact); otherwise no term,
and the guard is expanded into its cases (at most 6 per check; more: no
hints). Two quantifier-free checks follow, `OLD(x, g) and not NEW(x, t(g))`
and `NEW(x, g') and not OLD(x, s(g'))`; both unsat is `equiv-sat
by=hints`. Why the terms fit: in a model of a BMC-form query the true
guards are the blocks of one path from the entry to the halt (an edge is
`pred && succ`, a `cond_br` block has at most one successor whose
condition holds, the halt block's guard is asserted), and the blocks that
share an id are on a path together. The hints are checked, not trusted:
a wrong one makes its check sat or unknown, and the quantified check runs
(`by=exists hints=<old-new|new-old>:<answer>`); it never makes a pass.
`hints=incomplete:<n>` and `hints=none:<why>` (`cases:<n>`: a guard
without a hint has more than 6 cases, an untuned bound;
`guard-without-ids`) say why no hint check ran.
`--no-hints` turns them off.

Every z3 check has a resource limit (`--rlimit`, deterministic) and a
time limit (`--timeout`, a backstop). The sat checks solve the job's own
query, so the default limit (200 million, about two minutes of z3) is
sized for the quick tier of the vcc suite; it is not tuned. An `unknown`
names the limit it hit (`old=unknown:rlimit`). z3 runs single-threaded
by default, so `--par N` uses at most N cores for z3. The exit status is
0 when every job is `identical`, `order`, `equiv-sat` or `both-unsat`,
1 on a `DIFF` or `error`, 2 on an `unknown`, and 3 with `--strict` on a
`both-unsat`, checked in this order.

### Encoding diff

`utils/query-diff.py` compares two binaries on one corpus: use it when the
tool changes and the programs do not. `utils/encoding-diff.py` is the
complement: one `sea-vc` on two programs, when the program changes and the
claim must not (a hand-skolemized claim against `sea.box.all` with
`--box-all-expand=0`, a rewritten loop, reordered conjuncts).

```bash
python3 utils/encoding-diff.py --sea-vc build/bin/sea-vc \
    --left-flags=--box-all-expand=0 --right-flags=--box-all-expand=0 \
    --alias k=box.all.eq.0 left.mlir right.mlir
```

Both sides run with `--lower --seam --smt2 --ackermannize=off` (no symbol
is named after a read's position). Nondets and objects are renamed from
their sites to the names the programs give them, so two programs that
number their sites differently compare by name. A symbol declared on one
side only is projected (`exists`). The classes are `identical`, `same-dag`
(the same terms up to let names and conjunct order), `equiv-parts` (z3
proves the differing top-level conjuncts equivalent on their own), `equiv`
(both sat, z3 proves the whole queries equivalent), `both-unsat` (verdict
only, not equivalence), `DIFF`, `unknown`, `error`. Each pair prints
`result: <class>`; the exit status is 0, 1 on `DIFF` or `error`, 2 on
`unknown`, 3 with `--strict` on `both-unsat`. Every z3 check has an
`--rlimit` (deterministic) and a `--timeout` backstop. `--agent` prints
the guide.

## Pass validation

A change of the lowering (a fold, a pass, an optimization switched on)
runs `utils/cover-diff.py`: one `sea-vc`, two configurations A and B of
the lowering of the same input, compared by reach coverage
(`sea-vc --reach`, [Reach](../reference/output-formats.md#reach)).

```bash
python3 utils/cover-diff.py --sea-vc <build>/bin/sea-vc \
    --args="--lower --widen-memory=8 --seam" --a=--lower-opt=false --b= \
    bench/work/public/vcc --par 4
```

`--a` and `--b` are the arguments of each side (`--lower-opt=false`,
`--lower-disable=sccp|cse|canonicalize`), `--args` those of both.

What is compared, per input:

- Keys. `sea-vc --reach` tags every assume and assert of the input with its
  position (`sea.reach_site`) before any pass. A key is a site and a role:
  `assume` (the top of input assume k, or of the `sea.unreachable` its
  folding to false left), `assert` (the top of input assert k), `assume`
  of an assert (the assume `sea-merge-asserts` makes of assert k: reached
  and holds), and `fail` (assert k reached with its condition false;
  `sea-vc --reach-failures`). Checks the lowering made have no site and
  are not compared (`untagged=`).
- Both sides run with `sea-vc --reach-keep-sites`: canonicalize replaces a
  tagged check of the constant true by a reach-site marker (an inert
  `sea.print`) instead of erasing it, so the check stays a key on both
  sides.
- Level 0 (the default): the reach answer of every key on each side,
  concrete runs first (`--fuzz`, 1000 trials per side by default; each
  side draws its own inputs), the solver for the rest; and the two safety
  verdicts (`sea-vc` without `--reach`; `--no-verdict` leaves them out).
- Level 1 (`--level=1`): for every key reachable on both sides, the
  query-diff classifier with `--project-blocks` on its two R_T programs:
  `equiv-sat` means the same inputs reach the key (for a `fail` key: the
  same inputs make the assert fail). The block guards are matched through
  the block ids of the two R_T (Skolem hints, above); each `level1:` line
  says how its pair was proved, `by=identical` (the same query up to
  order), `by=names`, `by=hints` or `by=exists` (the quantified check),
  and the totals count each (`--no-hints`: no hints).

| class | meaning |
|---|---|
| `equal` | both sides reachable, or both unreachable |
| `DIFF` | reachable on one side, unreachable on the other |
| `removed-ok` | on one side only, and unreachable there: dead code the other lowering erased |
| `removed-true` | (`--no-keep-sites` only) on one side only, a check of the constant true there, and the next target of its block, which is reached iff it is, is equal |
| `removed-but-reachable` | on one side only, and reachable there: a `DIFF` |
| `unknown` | an unknown answer decides the class; also a removed check of true with no next target that can stand for it (`--unresolved-true=pass` counts it `removed-true`) |

Both sides run on one placement of the objects. The writer side lowers
with `sea-vc --place-out=<file>`, the other with `--place-in=<file>`
([External placement](../reference/output-formats.md#placement-files)), so
the objects of both sides are at the same addresses. The writer should be
the side with more objects: A writes first. If B has an object that A's
file does not name (or one of another kind, size or alignment: `place.in-unplaced` or
`place.in-mismatch`; other `place.in-*` errors are errors), B writes and A
reads, once. The `layout:` line names the writer.
If both fail, there is no shared placement, and the job is `unknown`
(`reason=layout`), not a pass. The gate does not check all placements: both
lowerings are compared on one placement, the writer's; the reader takes it
as an allocator would (checked for alignment, size, kind, overlap, null
and width). The reach
runs, the safety verdicts, the R_T programs and the witness replays all
use it. Each side's own placement need not agree with the other's on the
common objects: sccp and cse can run before placement on one side only.

A job is `DIFF` if some key is, or the verdicts are sat and unsat, or
level 1 finds a `DIFF`. It is `unknown`, never a pass, when no key is
tagged on either side (`reason=no-targets`: an input without checks, a
`sea-vc` without `site=`), and at level 1 when a key has copies on a side
(`reason=copies`: R_T of one copy is not the target). A `DIFF` with a
reachable side replays that side's witness on the other side's lowering
(`replay:` line). The exit status is 1 on a `DIFF`, 3 on an error, 2 on an
`unknown`, checked in this order, and 0 on a pass.

### QA corpus

`test/QA` holds the gate's corpus: small programs in
`test/QA/Inputs/<corpus>/`, and lit tests (part of `check-sea`) named by
what they validate: `lower-opt-<corpus>.test` runs `cover-diff.py
--level=1` with `--a=--lower-opt=false --b=` on one corpus and expects
`status: pass`, which validates the canonicalize and cse folds of the
lowering on those programs. The corpora are `fold` (the fold tests and
probes), `drop-fresh-memhavoc`, `replay-original` and
`guard-null-memhavoc` (programs of those passes' tests and probes).
`sea-drop-fresh-memhavoc` and `sea-guard-null-memhavoc` run on both sides
and are not validated by these tests: they decide semantics, and there is
no A/B switch for them. `test-*` programs come from the lit tests
(positive and edge cases), `probe-*` from reviewers' adversarial probes. A
new fold adds its positive, edge and probe programs to a corpus; a program
goes in only if it passes (a program without checks is `unknown`, one the
interpreter cannot run is an error).

### QA coverage

`just qa-coverage` (`utils/qa-coverage.py`) measures how much of each
validated pass the corpus drives: a clang source-coverage build
(`-fprofile-instr-generate -fcoverage-mapping`, `build-cov`; configured
from the arguments after `--` when it has no `CMakeCache.txt`, e.g. the
CMake line of the project's local notes) runs `test/QA` and then all of
`check-sea`, and the script prints, per pass source, line and branch
coverage under both, the branches the corpus never takes one way (and
whether `check-sea` takes them), and per canonicalization pattern how
often it ran and fired (`canon.fold` events). The numbers are a guide to
where the corpus is thin, not a target.

## Continuous integration and attestation

GitHub Actions (`.github/workflows/ci.yml`) runs on every pull request
and every push to `main`:

| job | checks |
|---|---|
| `lint` | `.pre-commit-config.yaml` on every file |
| `commits` | the commit messages (commitlint, `.commitlintrc.yml`) |
| `docs` | `mkdocs build --strict` |
| `utils-tests` | the Python tests that need no build (with Ubuntu's z3 for `query-diff`) |

The build, lit and the benchmark gates need LLVM, the solvers and the
benchmark suites, which the hosted runners do not have. They run on a
developer machine, and `utils/attest.py` (`just attest`) posts each
result as a commit status on the SHA it tested:

| context | gate | description |
|---|---|---|
| `sea-mlir/check-sea` | build, then `ninja check-sea` | `398/398 passed, 1 unsupported (1m30s)` |
| `sea-mlir/bench-vcc` | `bench.py gate quick --suite public/vcc --par 4 --no-record` | jobs, failures, counter failures, wall time |
| `sea-mlir/bench-perf` | CPU time of the vcc gate against the base commit | total change, jobs slower, base, runs |

```bash
just bench-connect                       # once: the benchmark suites
just attest --dry-run                    # run the gates, print the statuses
just attest --build build-release        # run them and post the statuses
just attest --only check-sea --build build-release
```

- HEAD must be clean (untracked files count): the statuses name a
  commit, so they must describe exactly that commit. The tree is checked
  again before posting.
- A status belongs to one SHA. A new push (or a rebase) needs a new
  attest.
- `--build DIR` reuses a build of this source tree; without
  `CMakeCache.txt` it configures one (Release) with the CMake arguments
  after `--`. The build is brought up to date first.
- bench-perf compares the CPU time per job of one run per side
  (`--perf-runs N`: N runs, medians) of the default configuration, which
  uses z3, and fails when the total over the jobs both sides solve grows
  by more than 10 % (`--perf-threshold`). z3 times vary little; after a
  failure, rerun with `--perf-runs 3` before looking for a cause.
  Yices times vary with its random seed and need
  several seeds per job; they are not compared. The base is `--base`
  (default: the merge base with `origin/main`). Each vcc gate run is
  recorded per SHA in `$SEA_BENCH_HOME/attest/`; without a record for
  the base, pass `--base-sea-vc` (a sea-vc built at the base) and the
  base and HEAD runs alternate. A record entry whose results
  directory is gone is dropped (`stale-record:` line) and counts as
  never recorded.
- Without the benchmark suites (`just bench-connect`, or
  `SEA_BENCH_SUITES`), attest refuses with exit code 2 before running
  any bench gate and posts nothing; `--only check-sea` needs no suites.
- Without `--dry-run`, posting needs `gh` logged in with write access to
  the repository.

The ruleset of `main`, the default branch (`.github/rulesets/main.json`,
applied with `gh api repos/seahorn/sea-mlir/rulesets --input .github/rulesets/main.json`)
requires a pull request approved by one reviewer other than its author
(an approval counts only if it comes after the last push), the `lint`,
`commits` and `docs` jobs, and the
`sea-mlir/check-sea` and `sea-mlir/bench-vcc` statuses on the branch's
latest commit, up to date with `main`. It blocks force pushes and
deleting the branch, and allows only merge commits: they keep the
pull request's commits, with the SHAs attest posted statuses on, and the
merge's tree is the attested head (the branch must be up to date). A
rebase merge rewrites every commit, so nothing on `main` would carry a
status; a squash merge also takes the pull request's title as the commit
message, which the `commits` job does not check. `git log --first-parent
main` shows one line per pull request. Organization admins may bypass the
rules only through a pull request (GitHub shows them a "merge without
waiting for requirements" option): the maintainer uses it, case by case,
for pull requests that have no second reviewer. Agents never merge.

## Which gates for which change

- Operation semantics, the interpreter or a folder: lit (the semantics
  oracle) and fuzz.
- The query or SeaM: lit, `fuzz-vc` and `fuzz-seam`, the benchmark canary
  and smoke gates.
- Performance: the benchmark gate at the tier where the effect shows, with
  counters, and an explanation for any slowdown.
- Every pull request: lint, and `just attest` on its last commit.
- A release: all of them.

## Publishing

Development happens in the private `sea-mlir-dev` repository. Start a
publication branch from public `main`, then copy the changes intended for
release onto it. Publish that branch only after reviewing its **whole
history**: deleting a private reference in a later commit does not remove
it from the commits a push exposes.

Configure a remote named `public` for `seahorn/sea-mlir` and keep the
private regex file outside the checkout. Install
[gitleaks](https://github.com/gitleaks/gitleaks) (the CI version is 8.30.0),
then run:

```bash
export SEA_PRIVACY_TERMS=<external-regex-file>
just install-hooks
just publish <branch> --strict --dry-run
just publish <branch> --strict
```

`publish.py` fetches `main` from the remote's actual push URL and checks every commit reachable from
the selected branch but not from that main. A different fetch URL cannot
change the baseline; multiple push URLs are refused. Shallow repositories
are refused until their history is fetched in full. For each commit it scans the
complete added or changed files, filenames, and commit message with the
privacy matcher. The published branch name is checked too. Merge commits
include changes against each parent.
Symlink targets are scanned as text; links are not followed. The normal
public-credit exceptions in `privacy-check.py` still apply to file
contents, but never to commit messages. Gitleaks scans patches across the
same history with its built-in rules; repository ignore files, scanner
configuration, and inline suppressions cannot disable that scan.

A finding refuses publication. A missing gitleaks installation emits a
warning unless `--strict` (or `SEA_PUBLISH_STRICT=1`) makes it an error.
An unreadable terms file, invalid regex, or scanner failure is always an
error. Terms are optional: `--terms`, else `SEA_PRIVACY_TERMS`, else an
untracked `.privacy-terms` in the worktree or the main worktree (shared by
linked worktrees); without any of them, only the generic privacy patterns
run. The tool prints locations and pattern
categories, never matching content. It also redacts terms from filenames
and suppresses raw scanner diagnostics.

On success, the tool pushes the exact SHA it checked, without force or
following tags, and prints a `gh pr create` command. It does not create
the pull request. Review, attest, and merge through the public repository's
normal gates; bring public `main` back into the private development
repository afterward.

The installed pre-push hook applies the same checks to branch updates
sent to the canonical HTTPS or SSH URLs of `seahorn/sea-mlir`. Existing
branches are checked from their advertised old tip; new branches are
checked from freshly fetched public `main`. Deletions need no scan.
Other remotes pass through even in older worktrees without the checker;
a public push from such a worktree is refused. Tags require a separate release review and
are refused by this hook. Installation refuses to replace a different
hook; integrate existing hooks manually. Hooks are local guards against
accidents, not enforcement against a developer who bypasses them.

The `privacy` job in `.github/workflows/privacy.yml` scans pull-request
commits with the `PRIVACY_TERMS` Actions secret when present and requires
the pinned gitleaks binary. It uses `pull_request_target` so the workflow
and checker come from the trusted base. **PR files are never checked out
or executed in that job**: it fetches objects and reads blobs as data.
This also permits private-term checks on fork PRs. No token is persisted,
permissions are read-only, and raw scanner reports are discarded. Keep
that boundary when editing the workflow. The ordinary CI jobs still run
without these private terms on `pull_request`.

CI runs after a public push, so it cannot prevent the first disclosure.
The local scan is required before publishing. The workflow becomes active
only once it is on the default branch; bootstrap its installation with a
reviewed local strict scan. Initial repository creation and release tags
remain separate maintainer operations. Repository settings, including the
secret and any required-check configuration, are not changed by these
scripts.
