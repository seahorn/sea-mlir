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


## Local binary releases

The release job produces a relocatable Linux x86-64 tarball and a runtime
image, then tests the extracted tarball in a clean Ubuntu 24.04 container.
The builds take hours on a shared machine, so the driver can stop at a
given local time (`--until HH:MM`), and can refuse to start before another
(`--not-before HH:MM`, a window across midnight when it is later than
`--until`). `--timezone NAME` sets the zone of both (default: the system
zone). Without `--until` there is no time limit:

```bash
just binary-release ../release-output --plan
just binary-release ../release-output
just binary-release ../release-output --not-before 22:00 --until 07:00
# Build the standalone artifact first, then the held SeaHorn bundle:
just binary-release ../release-output --bundle
```

The output directory must be outside the source tree. Each run records its
source revision, whether the checkout was dirty, image IDs, stage times,
logs and artifact hashes in `release-run.json`. Tarballs, SHA-256 files and
package manifests go in `artifacts/`. Use a clean, committed checkout for
artifacts intended for review. The job never publishes them.

Builds run sequentially with eight compile jobs and four regression-test
workers. At the `--until` time the driver interrupts the active command and
stops its own remaining build or smoke containers (exit 124). Rerun the
command later to reuse Docker's cached layers. The driver works with Docker's
legacy builder; BuildKit cancellation has not been separately validated.
It creates only `sea-mlir-release:*` images and does not prune the cache.
Both release builders disable Yices.

To reuse an existing LLVM image, pass `--llvm-image IMAGE`. The image must
contain the LLVM installation at `/opt/llvm` and the build dependencies of
the `base` stage. The job checks the revision file, Release mode, assertions,
MLIR and lit before using it. Without this option it builds LLVM from the
pinned source. Changing the LLVM revision invalidates reuse.

The standalone tarball includes the installed tools, their non-system
shared libraries, licenses and notices. Every ELF file uses a relative
`$ORIGIN` RPATH. The package retains Ubuntu's glibc, C++ runtime and GCC
runtime as system dependencies; Ubuntu 24.04 x86-64 is the tested target.
The manifest records the packaged files and resolved ELF dependencies.
The smoke test checks the version, SAT with counterexample replay, and
UNSAT after relocating the tarball into a fresh container.

### SeaHorn bundle

`--bundle` builds SeaHorn separately against Ubuntu's LLVM 18, with the
source and dependency pins in `docker/SeaHorn.Dockerfile`. SeaHorn uses
Z3 4.12; sea-mlir uses Z3 5.3.3. The bundle preserves their distinct shared
library names. The builders exchange programs through the exported MLIR
text format and do not share an LLVM installation.

The bundle adds the `sea` Python driver and SeaHorn executables, but no C
compiler. Its Ubuntu runtime installs `clang-18` and Python 3. The clean
container smoke compiles C fixtures with `sea yama -y sea-mlir-export`,
then checks both SAT/replay and UNSAT with `sea-vc`. For a C input, use
SeaHorn's assertion header and the profile's export option:

```bash
sea yama -y sea-mlir-export -m64 --horn-sea-mlir-out=program.mlir program.c
sea-vc program.mlir --lower
```

`-m64` selects the installed compiler's native headers; 32-bit C inputs
that include system headers need an additional multilib toolchain.
The smoke fixtures retain symbolic inputs through export, so both verdicts
exercise the backend.

The SeaHorn artifact and bundle are **held pending SeaHorn relicensing**.
Their notices and marker files record that restriction. A successful build
or smoke test does not authorize distribution. The standalone sea-mlir
artifact can proceed to release review independently.
