# Docker build

`docker/Dockerfile` builds sea-mlir from source on a clean Ubuntu 24.04:
LLVM and MLIR at the commit in `LLVM_REVISION`, the solvers at pinned
versions, sea-mlir with its regression tests (`check-sea`), and a runtime
image with the installed tools. It is the "does it build on a clean
machine" test. Later it is the base of CI (with a prebuilt LLVM once LLVM
24 is released) and of the binary release.

## Build

From the root of a sea-mlir checkout (the build context is the source
tree; `.dockerignore` leaves out build trees and benchmark checkouts):

```bash
just docker-build        # image sea-mlir:dev (z3 + Bitwuzla)
just docker-check        # Yices build test with check-sea, smoke runs of sea-mlir:dev
docker run --rm -v "$PWD:/work" sea-mlir:dev sea-vc p.mlir --lower
```

The build uses 8 jobs (`SEA_DOCKER_JOBS`, the `JOBS` build argument). At
8 jobs the LLVM stage takes about 45 minutes and the rest about 15
(on a shared 48-core machine). The LLVM stage copies only
`LLVM_REVISION`, so Docker's layer cache keeps it until the pin moves, and
a change to sea-mlir rebuilds only the sea-mlir and runtime stages.

## Stages

| stage | what |
|---|---|
| `base` | Ubuntu 24.04 with clang 18, lld, CMake, Ninja, Python 3, Boost, GMP, MPFR and the tools the solver builds need |
| `llvm` | a shallow fetch of the commit in `LLVM_REVISION`; LLVM + MLIR, Release with assertions, X86 only, `LLVM_INSTALL_UTILS=ON` (FileCheck, not, count), installed in `/opt/llvm` with lit (`/opt/llvm/share/lit/lit.py`) |
| `solvers` | z3 and Bitwuzla built from source into `/opt/solvers`; Yices only with `--build-arg YICES=1` |
| `sea-mlir` | configure against `/opt/llvm` and `/opt/solvers`, build, `check-sea`, install to `/opt/sea-mlir`; the build tree stays in `/build` |
| `runtime` (default) | Ubuntu 24.04 with the runtime libraries, `/opt/sea-mlir` and `/opt/solvers`; `sea-vc` on `PATH` |

## Pins

The build arguments at the top of `docker/Dockerfile` hold the pins; keep
this table in sync.

| what | pin | license |
|---|---|---|
| base image | `ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852` | |
| LLVM + MLIR | the `commit:` line of `LLVM_REVISION` | Apache-2.0 WITH LLVM-exception |
| z3 | tag `z3-5.3.3` (`Z3_TAG`) | MIT |
| Bitwuzla | tag `0.9.1` (`BITWUZLA_TAG`) | MIT |
| Yices | tag `yices-2.7.0` (`YICES_TAG`), only with `YICES=1` | GPLv3 |

The Ubuntu packages are not pinned: `apt-get` installs the versions
current at build time on top of the pinned base image.

## Yices

The runtime image contains z3 and Bitwuzla only. Yices is GPLv3, and a
binary that links it would have to be distributed under the GPLv3, so
release binaries and images omit it (docs/Release.md, "Runtime
requirements"). `--build-arg YICES=1` builds Yices into the `solvers`
stage and sea-mlir with it, for build tests and solver comparisons:

```bash
docker build -f docker/Dockerfile --build-arg YICES=1 --target sea-mlir \
  -t sea-mlir-test:yices .
```

The `runtime` stage fails when Yices is present, so such a build is never
a runtime image.
