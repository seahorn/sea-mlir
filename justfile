# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# sea-mlir recipes. Benchmarks live outside this repo (docs/Benchmarks.md):
# `just bench-connect` links / clones them into bench/work/<name>.

bench := "python3 utils/bench.py"

# list the recipes
default:
    @just --list

# connect the benchmark repos of bench/sources.yaml (+ bench/sources.local.yaml, $SEA_BENCH_SOURCE_<NAME>) into bench/work/
bench-connect:
    {{bench}} connect

# list the connected suites and families (jobs per tier and expected verdict)
bench-list *sel:
    {{bench}} suites {{sel}}

# gate a tier (smoke | quick | full | hard) with a configuration list (default, default+CFG, CFG; comma-separated); records $SEA_BENCH_HOME/gates/<date>-<commit>.yaml
bench-gate tier="smoke" config="default" *args="":
    {{bench}} gate {{tier}} --config {{config}} {{args}}

# the smoke gate (CI candidate)
bench-smoke *args="":
    {{bench}} gate smoke {{args}}

# the canary gate: a few jobs per family (tag canary), verdicts and counter bounds (docs/Benchmarks.md)
bench-canary *args="":
    {{bench}} gate quick --tag canary {{args}}

# the release gate (docs/Release.md): Release build in BUILD, lit, install to a temp prefix, smoke-run the installed tools, bench smoke gate if connected
release-check build="build-rc" *cmake_args="":
    utils/release-check.sh {{build}} {{ if cmake_args == "" { "" } else { "-- " + cmake_args } }}

# the documentation site (mkdocs.yml; needs `pip install -r docs/requirements.txt`): build site/ with warnings as errors
docs:
    mkdocs build --strict

# serve the documentation site with live reload on http://127.0.0.1:8000
docs-serve addr="127.0.0.1:8000":
    mkdocs serve --dev-addr {{addr}}

# check the SPDX header of every code and test file (utils/spdx.py; REUSE.toml lists the files without one)
spdx:
    python3 utils/spdx.py --check

# add or repair missing / wrong SPDX headers
spdx-fix:
    python3 utils/spdx.py --fix

# run the checks of .pre-commit-config.yaml (formatting, lint, typos, SPDX, REUSE, privacy) with prek or pre-commit; default: every file
lint *args="--all-files":
    if command -v prek >/dev/null; then prek run {{args}}; else pre-commit run {{args}}; fi

# run the local gates (check-sea, the vcc gate, vcc performance) on the clean HEAD and post them as GitHub commit statuses (utils/attest.py --agent; --dry-run prints them)
attest *args="":
    python3 utils/attest.py {{args}}

# regenerate the committed dialect reference docs/reference/dialect.md from the ODS (needs mlir-tblgen: argument, $MLIR_TBLGEN, the build's LLVM, or PATH)
docs-dialect *tblgen="":
    utils/gen-dialect-doc.sh {{tblgen}}

# how far the LLVM pin (LLVM_REVISION) is behind main: age, and commits behind with --llvm DIR / $LLVM_PROJECT (--fetch, --list) or --network
llvm-drift *args="":
    python3 utils/llvm-drift.py {{args}}

# build the Docker image sea-mlir:dev (docker/README.md): LLVM at the pin, z3 + Bitwuzla, sea-mlir built and tested (check-sea); -j$SEA_DOCKER_JOBS (8)
docker-build *args="":
    docker build -f docker/Dockerfile --build-arg JOBS=${SEA_DOCKER_JOBS:-8} \
      --build-arg SEA_GIT_DESCRIBE="$(git describe --tags --always --dirty --abbrev=12)" \
      -t sea-mlir:dev {{args}} .

# the Docker build test with Yices (sea-mlir-test:yices, never a runtime image): check-sea inside it, then a sat and an unsat smoke run of sea-mlir:dev
docker-check:
    docker build -f docker/Dockerfile --build-arg JOBS=${SEA_DOCKER_JOBS:-8} \
      --build-arg SEA_GIT_DESCRIBE="$(git describe --tags --always --dirty --abbrev=12)" \
      --build-arg YICES=1 --target sea-mlir -t sea-mlir-test:yices .
    docker run --rm sea-mlir-test:yices sh -c 'ninja -C /build check-sea && /build/bin/sea-vc --version'
    docker run --rm -v "$PWD/test:/test:ro" sea-mlir:dev sea-vc /test/VC/print/replay.mlir --lower | grep -E '^(tool.version|result|replay):'
    docker run --rm -v "$PWD/test:/test:ro" sea-mlir:dev sea-vc /test/VC/seam/inb-ok.mlir --config=seam | grep -E '^(result|vc.config):'

# Check all unpublished commits, then push the checked branch (docs/agents/gates.md).
publish branch *args:
    python3 utils/publish.py {{quote(branch)}} {{args}}

# Install the public-remote pre-push guard; refuses to overwrite a different hook.
install-hooks:
    python3 utils/publish.py --install-hook

# Build local release artifacts in the overnight window (docker/README.md).
binary-release output *args:
    python3 docker/release.py {{quote(output)}} {{args}}
