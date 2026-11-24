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
just docker-build        # image sea-mlir:dev (z3, Bitwuzla, Yices)
just docker-check        # build stage with check-sea, smoke runs of sea-mlir:dev
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
| `solvers` | z3, Bitwuzla and Yices built from source into `/opt/solvers`; `--build-arg YICES=0` leaves Yices out |
| `sea-mlir` | configure against `/opt/llvm` and `/opt/solvers`, build, `check-sea`, install to `/opt/sea-mlir`; the build tree stays in `/build` |
| `runtime` (default) | Ubuntu 24.04 with the runtime libraries, `/opt/sea-mlir` and `/opt/solvers`; `sea-vc` on `PATH` |

## Pins

The build arguments at the top of `docker/Dockerfile` hold the pins; keep
this table in sync.

| what | pin | license |
|---|---|---|
| base image | `ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852` | |
| LLVM + MLIR | the `commit:` line of `LLVM_REVISION` | Apache-2.0 WITH LLVM-exception |
| z3 | master commit `bbd46c9` (`Z3_REVISION`; it reports 5.4.2), not a release; fetched by commit | MIT |
| Bitwuzla | tag `0.9.1` (`BITWUZLA_TAG`) | MIT |
| Yices | tag `yices-2.8.0` (`YICES_TAG`), without MCSAT; left out with `YICES=0` | BSD 3-Clause (GPLv3 before 2.8.0) |
| CaDiCaL | tag from Bitwuzla's `subprojects/cadical.wrap` (currently `rel-2.1.2`); not set by hand | MIT |
| SymFPU | revision from Bitwuzla's `subprojects/symfpu.wrap` (currently `40bdec0`); not set by hand | GPL-3.0 or BSD 3-Clause, at the user's choice; the release uses BSD 3-Clause |
| SeaHorn | `SEAHORN_REVISION`, `docker/SeaHorn.Dockerfile` (dev18 `67474bf`), plus `docker/seahorn-yices.patch` (the Yices >= 2.7 fix, until it is in dev18) | Apache-2.0 WITH LLVM-exception |
| sea-dsa | `SEADSA_REVISION` (dev18 `1e33f16`) | Apache-2.0 WITH LLVM-exception |
| llvm-seahorn | `LLVM_SEAHORN_REVISION` (dev18 `9225814`) | Apache-2.0 WITH LLVM-exception |
| clam, crab | not built (`WITH_CLAM=OFF`) | |
| LLVM 18 for SeaHorn | `LLVM18_VERSION`: apt.llvm.org `llvm-toolchain-noble-18` 18.1.8, key checked by fingerprint | Apache-2.0 WITH LLVM-exception |
| SeaHorn's z3 | `SEAHORN_Z3_REVISION`: z3 4.12.0 (`feda706`); SeaHorn does not work with the sea-mlir pin (`docs/design/BinaryRelease.md`) | MIT |

The release packaging takes the solver pins from the `Z3_REVISION`,
`BITWUZLA_TAG` and `YICES_TAG` arguments (`docker/release.py` reads them
from `docker/Dockerfile` and passes them on). It refuses a z3 whose
`--version` names a commit other than `Z3_REVISION` (when it is a full
commit hash), and a
`sea-vc` whose `--version` does not report that z3 version and the Yices
version of `YICES_TAG`. The `solvers` stage saves Bitwuzla's
two wrap files in `/opt/solvers/share/pins`, and `docker/package-release.py`
reads the CaDiCaL and SymFPU pins from them for the license downloads and the
notices. The artifact version is the `project(sea-mlir VERSION ...)` of
`CMakeLists.txt` unless `--version` is given.

The Ubuntu packages are not pinned: `apt-get` installs the versions
current at build time on top of the pinned base image.

## Yices

The runtime image and the release binaries include Yices. Yices is
BSD-3-Clause from 2.8.0 (earlier releases are GPLv3). It is built without
MCSAT, so it links GMP and not LibPoly (docs/Release.md, "Runtime
requirements"). The SeaHorn bundle's SeaHorn links the same
`libyices.so.2.8`. `--build-arg YICES=0` builds
the solvers and sea-mlir without it:

```bash
docker build -f docker/Dockerfile --build-arg YICES=0 -t sea-mlir:no-yices .
```


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
# Build the standalone artifact first, then the SeaHorn bundle:
just binary-release ../release-output --bundle
```

The output directory must be outside the source tree. Each run records its
source revision, whether the checkout was dirty, image IDs, stage times,
logs and artifact hashes in `release-run.json`. Tarballs, SHA-256 files and
package manifests go in `artifacts/`. A dirty checkout is marked
`-dirty` in the version string of the built tools. Use a clean, committed checkout for
artifacts intended for review. The job never publishes them.

Builds run sequentially with eight compile jobs and four regression-test
workers. At the `--until` time the driver interrupts the active command and
stops its own remaining build or smoke containers (exit 124). Rerun the
command later to reuse Docker's cached layers. The driver works with Docker's
legacy builder; BuildKit cancellation has not been separately validated.
It creates only `sea-mlir-release:*` images (`--image-prefix NAME` for
`NAME:*`) and does not prune the cache. `--cpuset 40-47` pins every
`docker build` to those CPUs.
The release build includes Yices (`YICES=1`).

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
Packaging fails when a copied shared library has neither an owning Ubuntu
package (whose copyright file is included) nor a known upstream license, or
when a dependency would resolve outside the package.

No GPL-family code (GPL, LGPL, AGPL) is shipped. GMP, GMPXX and MPFR (LGPL)
stay Ubuntu packages: `libgmp10` is in the base image, and the runtime
needs `apt-get install libgmpxx4ldbl libmpfr6`. The license gate of
`docker/package-release.py` fails the build unless every shipped file
matches a rule of `FILE_RULES` whose component license (`COMPONENTS`) has an
alternative made only of `ALLOWED_LICENSES`. The licenses of sea-mlir,
SeaHorn, sea-dsa and llvm-seahorn are their REUSE `LICENSES/` directories,
so a GPL file added to one of them fails the gate. The manifest's
`license_audit` lists each component, its license and its file count;
`docker/test_package_release.py` tests the gate.

The smoke test checks the version (with Yices), SAT with counterexample
replay (with z3 and with Yices), and UNSAT after relocating the tarball
into a fresh container.

### SeaHorn bundle

`--bundle` builds SeaHorn dev18 without clam/crab (`WITH_CLAM=OFF`) and
with Yices separately against LLVM 18.1.8 from apt.llvm.org, with the
source and dependency pins in `docker/SeaHorn.Dockerfile`. It takes the
Yices headers and `libyices.so.2.8` from the `solvers` image
(`SOLVERS_IMAGE`), so SeaHorn and sea-vc share one Yices 2.8.0 library;
the build refuses a Yices other than `YICES_TAG` or older than 2.8.0.
SeaHorn's test corpus (`share/seahorn/test`) is left out: it contains
inputs derived from Linux kernel sources (GPL-2.0). SeaHorn uses
Z3 4.12; sea-mlir uses a z3 master commit (5.4.2). The bundle preserves their distinct shared
library names. The builders exchange programs through the exported MLIR
text format and do not share an LLVM installation.

The bundle adds the `sea` Python driver and SeaHorn executables, but no C
compiler. Its Ubuntu runtime installs `clang-18` and Python 3. The clean
container smoke compiles C fixtures with `sea yama -y sea-mlir-export`,
then checks both SAT/replay and UNSAT with `sea-vc`, and runs SeaHorn's
BMC with its Yices backend (`sea bpf -m64 --bmc=mono
--horn-bmc-solver=smt-y2`) on both. For a C input, use
SeaHorn's assertion header and the profile's export option:

```bash
sea yama -y sea-mlir-export -m64 --horn-sea-mlir-out=program.mlir program.c
sea-vc program.mlir --lower
```

`-m64` selects the installed compiler's native headers; 32-bit C inputs
that include system headers need an additional multilib toolchain.
The smoke fixtures retain symbolic inputs through export, so both verdicts
exercise the backend.

The bundle is `artifacts/sea-mlir-bundle-VERSION-linux-x86_64.tar.gz`
(with its `.sha256` and manifest). VERSION is the sea-mlir version: the
bundle is a sea-mlir release with SeaHorn as a dependency, and the SeaHorn
commits are in the manifest (`"seahorn"`).
