# Gates

A gate is a check that a change must pass before it is trusted. The gates
answer different questions, and none replaces another. All of them run
from a sea-mlir source tree.

| gate | question | command |
|---|---|---|
| lint | is the change formatted, spelled, licensed and free of private paths? | `just lint` |
| lit | do the regression tests and the oracles pass? | `ninja -C <build> check-sea` |
| release check | does an installed build work? | `just release-check` |
| fuzz | do the query, SeaM and placement agree with the interpreter on random programs? | `utils/fuzz-vc.py`, `utils/fuzz-seam.py`, `utils/fuzz-place.py` |
| benchmarks | do the applications keep their expected verdicts and counter bounds? | `just bench-smoke`, `just bench-canary`, `just bench-gate quick` |

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
weakened to make a change pass ([Rules for contributors](rules.md#tests-and-oracles)).
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
checkout on demand ([Benchmarks and gates](../reference/benchmarks.md)):

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
`bench/gates/<date>-<commit>.yaml`; commit it with the change it gates.
The gates run at most 4 jobs in parallel by default, with a 16 GB memory
cap per job.

## Continuous integration and attestation

GitHub Actions (`.github/workflows/ci.yml`) runs on every pull request
and every push to `seam`:

| job | checks |
|---|---|
| `lint` | `.pre-commit-config.yaml` on every file |
| `commits` | the commit messages (commitlint, `.commitlintrc.yml`) |
| `docs` | `mkdocs build --strict` |
| `utils-tests` | the Python tests that need no build |

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
- bench-perf compares the median CPU time per job over 3 runs
  (`--perf-runs`) of the default configuration, which uses z3, and fails
  when the total over the jobs both sides solve grows by more than 10 %
  (`--perf-threshold`). Yices times vary with its random seed and need
  several seeds per job; they are not compared. The base is `--base`
  (default: the merge base with `origin/seam`). Each vcc gate run is
  recorded per SHA in `$SEA_BENCH_HOME/attest/`; without a record for
  the base, pass `--base-sea-vc` (a sea-vc built at the base) and the
  base and HEAD runs alternate.
- Without `--dry-run`, posting needs `gh` logged in with write access to
  the repository.

The ruleset of `seam` (`.github/rulesets/seam.json`, applied with
`gh api repos/seahorn/sea-mlir/rulesets --input .github/rulesets/seam.json`)
requires a pull request approved by one reviewer other than its author
(an approval counts only if it comes after the last push), the `lint`,
`commits` and `docs` jobs, and the
`sea-mlir/check-sea` and `sea-mlir/bench-vcc` statuses on the branch's
latest commit, up to date with `seam`. It blocks force pushes and
deleting the branch, and allows merge commits and rebase merges: a squash
merge would take the pull request's title as the commit message, which
the `commits` job does not check. Organization admins may bypass the
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
