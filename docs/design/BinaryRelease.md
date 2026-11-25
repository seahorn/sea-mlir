# Binary releases: Linux tarball, Homebrew, macOS

Status: plan for the release after v0.5.0. The Linux pipeline exists (`docker/`,
`docker/README.md`) and builds both artifacts, the standalone tarball and
the SeaHorn bundle; both may be distributed, and nothing is published yet.
The Homebrew tap and the macOS build are proposals; macOS has not been built.

This note covers three deliverables for the next release of sea-mlir
together with SeaHorn dev18:

- (a) a relocatable tarball for Ubuntu 24.04 x86-64 with sea-mlir, the
  SeaHorn frontend and the solvers;
- (b) Homebrew packages with bottles for Linux and macOS, in a tap;
- (c) a first macOS build ([BuildMacOS.md](../BuildMacOS.md),
  `utils/build-macos.sh`).

## 1. Components and pins

Two LLVMs, two z3s, one Yices. Neither LLVM is shipped as a shared library:
sea-mlir links LLVM/MLIR statically, and SeaHorn links LLVM 18 statically
(the apt.llvm.org `llvm-18-dev` static libraries).

| component | pin | where the pin is defined | license |
|---|---|---|---|
| LLVM + MLIR (sea-mlir) | `302c842d6601` (LLVM 24 `main`) | `LLVM_REVISION` | Apache-2.0 WITH LLVM-exception |
| z3 (sea-mlir) | master `bbd46c9` (reports 5.4.2) | `Z3_REVISION`, `docker/Dockerfile` | MIT |
| Bitwuzla | 0.9.1 (with CaDiCaL, SymFPU from its wraps) | `BITWUZLA_TAG` | MIT; SymFPU GPL-3.0 or BSD-3-Clause, used under BSD-3-Clause |
| Yices | 2.8.0, without MCSAT; linked by sea-vc and SeaHorn | `YICES_TAG` | BSD-3-Clause |
| SeaHorn | dev18 `67474bf` and `docker/seahorn-yices.patch` | `SEAHORN_REVISION`, `docker/SeaHorn.Dockerfile` | Apache-2.0 WITH LLVM-exception |
| sea-dsa | dev18 `1e33f16` | `SEADSA_REVISION` | Apache-2.0 WITH LLVM-exception |
| llvm-seahorn | dev18 `9225814` | `LLVM_SEAHORN_REVISION` | Apache-2.0 WITH LLVM-exception |
| clam, crab | not built (`WITH_CLAM=OFF`) | | |
| LLVM 18 (SeaHorn) | 18.1.8, apt.llvm.org `llvm-toolchain-noble-18` | `LLVM18_VERSION` | Apache-2.0 WITH LLVM-exception |
| z3 (SeaHorn) | 4.12.0 `feda706` | `SEAHORN_Z3_REVISION` | MIT |
| GMP, GMPXX, MPFR | not shipped: Ubuntu 24.04 packages at run time | | LGPL-3.0+ |

The SeaHorn pins are dev18 commits that passed SeaHorn's CI; a SeaHorn
release can replace them later. `docker/seahorn-yices.patch` is SeaHorn's
Yices >= 2.7 fix (branch `feat/binary-release`, pending in dev18); when it
is merged, `SEAHORN_REVISION` moves to the merge and the patch is deleted.

### One z3 or two

SeaHorn dev18 configures with any z3 >= 4.8.9 (`cmake/FindZ3.cmake`); its
CI uses 4.8.9 and local builds 4.12.0. Built against the sea-mlir pin (z3
`bbd46c9`, Yices 2.8.0; clam on), SeaHorn compiles, and its lit suite gave
lit: 406 passed, 11 expected failures, 1 unsupported, 1 failure
(`add_by_one.c`) out of 419; the unit test `units_z3` also fails
(`expr.neg`):

- `test/tutorial/add_by_one.c` (`sea pf`, Spacer): the `-O0` run did not
  finish in 120 s; with z3 4.12.0 it takes 0.5 s. A Spacer regression in
  z3 master, or a parameter SeaHorn sets that changed meaning; not
  investigated.
- `units_z3` (`expr.neg`) compares a simplified term with the exact form
  4.12's simplifier produces; master produces an equivalent but different
  term.

The release ships two z3 libraries side by side. Their sonames differ
(`libz3.so.4.12`, `libz3.so.5.4`), each tool has its own RUNPATH to `lib/`,
and no process loads both: `seahorn` / `seapp` load 4.12 (seaopt links no z3), `sea-vc`
loads 5.4. No `z3` executable is installed in the bundle
(`build-seahorn-release.sh` removes SeaHorn's); SeaHorn's driver does not
run one. One z3 for both is
possible when SeaHorn's Spacer works with the sea-mlir pin; the test above
is the check.

### Yices in SeaHorn

SeaHorn links Yices, the same `libyices.so.2.8` as sea-vc: one library in
the bundle. SeaHorn dev18 at the pin does not compile against Yices >= 2.7
(renamed status constants); `docker/seahorn-yices.patch` is the fix. The
`seahorn` stage takes the Yices headers, `libyices.so.2.8.0` and
`yices-smt2` (SeaHorn's CMake requires it; not shipped) from the `solvers`
image, refuses a Yices other than `YICES_TAG` or older than 2.8.0, keeps
the library byte-identical to the solvers stage's (packaging fails on two
different copies), and checks that `seahorn` links it. The bundle smoke
runs SeaHorn's BMC with the Yices backend (`sea bpf -m64 --bmc=mono
--horn-bmc-solver=smt-y2`) on the sat and unsat fixtures.

### Clam / Crab

The bundle builds SeaHorn with `WITH_CLAM=OFF`, as SeaHorn's CI does: no
clam or crab code, headers or license files are in it, and `sea` options
that need Crab are unavailable. The Docker build has no clam / crab pins;
the packaging refuses `include/clam` or `include/crab`. Enabling clam later
needs pins (the last ones used were clam dev18 `3eaf016` and crab dev
`49773d0`) and a decision on crab's `IKOS_LICENSE.pdf` (NASA Open Source
Agreement) for the code crab derives from IKOS.

## 2. (a) The Linux tarball

`just binary-release OUT --bundle` (`docker/release.py`) builds, in a local
Docker, on `ubuntu:24.04` pinned by digest:

1. `llvm`: LLVM + MLIR at the pin (or `--llvm-image IMAGE` to reuse one);
2. `solvers`: z3, Bitwuzla, Yices at their pins;
3. `sea-mlir`: build, `check-sea`, install;
4. `package`: `docker/package-release.py` copies the non-system shared
   libraries into `lib/` (not GMP, GMPXX, MPFR: section "License audit"),
   sets every ELF RUNPATH to `$ORIGIN`-relative, refuses a dependency that
   resolves outside the prefix or a copied library without a license, runs
   the license gate, writes `THIRD-PARTY-NOTICES`, `RELEASE-MANIFEST.json`
   (pins, SeaHorn commits, packages, license audit, ELF dependencies,
   hashes) and a reproducible tarball (fixed mtimes, owner, order);
5. `standalone-smoke`: the tarball unpacked in a fresh `ubuntu:24.04`
   container, `sea-vc` sat (with replay, z3 and Yices) and unsat;
6. with `--bundle`: `seahorn` (`docker/SeaHorn.Dockerfile`),
   `bundle-package` (the sea-mlir prefix plus the SeaHorn prefix), and
   `bundle-smoke`: in a fresh container with `apt-get install clang-18
   python3` (and the GMP / MPFR packages), `sea yama -y sea-mlir-export`
   on `docker/smoke/{sat,unsat}.c`, then `sea-vc` must report the expected
   verdict (sat with replay), and SeaHorn's BMC with Yices must report it
   too.

Artifacts: `sea-mlir-<version>-linux-x86_64.tar.gz` and the bundle
`sea-mlir-bundle-<version>-linux-x86_64.tar.gz`, each with `.sha256` and
the manifest. Both may be distributed; the build is not a publication
(section 5).

The version is the sea-mlir version (`project(sea-mlir VERSION ...)`, or
`--version`): the bundle is a sea-mlir release with SeaHorn as a
dependency. The SeaHorn commits (seahorn, sea-dsa, llvm-seahorn), the hash
of `docker/seahorn-yices.patch`, SeaHorn's z3 and Yices versions and LLVM
18's version are under `"seahorn"` in the bundle's `RELEASE-MANIFEST.json`
(from `share/seahorn/build-pins.txt`).

### Relocatability

Installed binaries get `$ORIGIN/../lib` (`patchelf` in packaging). A plain
CMake install can produce the same without patchelf:
`-DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF '-DCMAKE_INSTALL_RPATH=$ORIGIN/../lib'`;
this needed the fix of `lib/Expr/CMakeLists.txt`, which passed Bitwuzla's
directory as an explicit `-rpath` that survived into installs (fixed on
this branch).

### Runtime requirements on a clean Ubuntu 24.04

- Standalone sea-mlir: `apt-get install libgmpxx4ldbl libmpfr6`. glibc,
  libstdc++, libgcc and GMP (`libgmp10`) come with the base image; GMPXX
  and MPFR are LGPL and not in the tarball. zlib, zstd and the solvers are
  in `lib/`. Python 3 only for `sea-reduce-test`.
- Bundle: `apt-get install libgmpxx4ldbl libmpfr6 clang-18 python3`.
  SeaHorn's `sea` compiles C
  with `clang-18` (it looks for `clang-mp-18`, `clang-18`, then `clang` on
  `PATH`) and is a Python 3 program; PyYAML is bundled (pure Python).
  Ubuntu's clang-18 is 18.1.3 and SeaHorn's LLVM is 18.1.8: bitcode is
  compatible within LLVM 18, and the bundle smoke test compiles with the
  Ubuntu clang. `sea`'s default is `-m32`, which needs 32-bit C headers
  (`apt-get install libc6-dev-i386`); `-m64` needs nothing more. Linking
  several input files uses `llvm-link-18` (`apt-get install llvm-18`).

Decision: clang is an apt dependency, not bundled. A bundled clang would
add the compiler, its resource headers and libLLVM/libclang-cpp (several
hundred MB), and it would still need the system's C headers
(`libc6-dev`), which apt's `clang-18` pulls in. The cost is one apt line
and that the C compiler is Ubuntu's 18.1.3, not 18.1.8.

### License audit

Rule: a release distributes no GPL-family code (GPL, LGPL, AGPL).

Decision (LGPL): LGPL libraries are runtime dependencies of the package,
not part of it: `apt-get` packages for the tarball, `depends_on` for
Homebrew (Homebrew's own practice for `gmp` and `mpfr`). Every binary links
them dynamically (checked: no GMP or MPFR symbols are defined in a shipped
file). Bundling them later is allowed, as unmodified shared libraries with
the LGPL-3.0 and GPL-3.0 texts and the URLs of their source tarballs, under
a gate rule naming those files only; LGPL stays refused everywhere else.

The license gate in `docker/package-release.py` enforces it at packaging time:
every shipped file must match a rule of `FILE_RULES`, which names its
component; the component's license (`COMPONENTS`) must have an SPDX
alternative made only of `ALLOWED_LICENSES` (0BSD, Apache-2.0,
BSD-2-Clause, BSD-3-Clause, BSL-1.0, ISC, MIT, NCSA, PSF-2.0, Zlib, public
domain; `X WITH LLVM-exception` counts as X). A file without a rule, a
component outside the list, or a library copied from an Ubuntu package
without an entry in `PACKAGE_LICENSES` fails the build. The licenses of
sea-mlir, SeaHorn, sea-dsa and llvm-seahorn are read from their REUSE
`LICENSES/` directories (shipped), so a GPL file added to one of those
repositories fails the gate (SeaHorn's `LicenseRef-NoLicense` is excepted:
it covers `play/neha-examples` only, which is not installed). The manifest
records the result under `license_audit`. `docker/test_package_release.py`
tests the gate (GPL / LGPL libraries, a file without a rule, clam headers,
a GPL license in a repository, an unknown Ubuntu package).

Audit of both tarballs (S: standalone, B: bundle). Licenses are SPDX ids;
the allowed alternative is the one the release uses.

| component | shipped files | in | license |
|---|---|---|---|
| sea-mlir | `bin/sea-reduce-test`, `share/sea-mlir/`, `share/doc/sea-mlir/`, `RELEASE-MANIFEST.json`, `LLVM_REVISION` | S, B | Apache-2.0 WITH LLVM-exception, MIT (REUSE) |
| sea-mlir tools | `bin/sea-vc`, `sea-opt`, `sea-run`, `sea-reduce`, `sea-seam-export` | S, B | sea-mlir, LLVM + MLIR (Apache-2.0 WITH LLVM-exception) and Boost headers (BSL-1.0), linked statically |
| z3 5.4.2 (`bbd46c9`) | `lib/libz3.so.5.4` | S, B | MIT |
| z3 4.12.0 | `lib/libz3.so.4.12` | B | MIT |
| Bitwuzla 0.9.1 | `lib/libbitwuzla{,bb,bv,ls}.so*` | S, B | MIT; CaDiCaL MIT; SymFPU GPL-3.0 OR BSD-3-Clause, used under BSD-3-Clause. No Kissat or CryptoMiniSat linked |
| Yices 2.8.0 | `lib/libyices.so.2.8` | S, B | BSD-3-Clause; MiniSat-derived code MIT, lookup3 public domain, timsort PSF-2.0 (Yices' `etc/NOTICES` section 1). Without MCSAT: no LibPoly (LGPL), no CUDD; no third-party SAT backend linked |
| zlib (`zlib1g`) | `lib/libz.so.1` | S, B | Zlib |
| Zstandard (`libzstd1`) | `lib/libzstd.so.1` | S, B | BSD-3-Clause OR GPL-2.0-only, used under BSD-3-Clause |
| SeaHorn | `bin/sea`, `bin/stats.py`, `lib/seapy/{sea,term}/`, `include/seahorn/`, `share/seahorn/`, `README.md` | B | Apache-2.0 WITH LLVM-exception, BSD-2-Clause, BSL-1.0, MIT, NCSA (REUSE) |
| sea-dsa | `include/seadsa/`, `lib/libc-64.spec.bc`, `lib/sea_dsa.ll` | B | Apache-2.0 WITH LLVM-exception, BSL-1.0, MIT, NCSA (REUSE) |
| SeaHorn tools | `bin/seahorn`, `seapp`, `seaopt`, `seainspect`, `seadsa` | B | SeaHorn, sea-dsa, llvm-seahorn (Apache-2.0 WITH LLVM-exception, NCSA), LLVM 18.1.8 (Apache-2.0 WITH LLVM-exception) and Boost (BSL-1.0), linked statically |
| PyYAML | `lib/seapy/yaml/` (pure Python) | B | MIT |
| notices | `LICENSE`, `NOTICE`, `THIRD-PARTY-NOTICES`, `share/licenses/`, `share/release-licenses/`, `share/doc/*/LICENSE*` | S, B | license texts, not code |

Not in either tarball:

| component | why | license |
|---|---|---|
| GMP 6.3.0, GMPXX, MPFR 4.2.1 | Ubuntu runtime packages (`libgmp10` in the base image; `libgmpxx4ldbl`, `libmpfr6` from apt); linked by sea-vc, Yices, Bitwuzla and SeaHorn, so not removable | LGPL-3.0-or-later OR GPL-2.0-or-later (GMP); LGPL-3.0-or-later (MPFR) |
| SeaHorn's test corpus (`share/seahorn/test`) | dropped: inputs derived from Linux kernel sources, e.g. `test/simple/ldv_bounce_sat.c`, `test/solve/01_unsat.c`; SeaHorn's REUSE.toml labels them Apache-2.0 WITH LLVM-exception | GPL-2.0 (Linux) |
| clam, crab | not built (`WITH_CLAM=OFF`) | Apache-2.0; crab also the IKOS license |
| SeaHorn's `z3` and `yices-smt2` executables, any compiler | removed or not installed | |
| glibc, libstdc++, libgcc_s, the dynamic loader | Ubuntu system libraries | |

Every C++ binary also contains code compiled from libstdc++'s headers and
GCC's startup objects (GPL-3.0-or-later WITH GCC-exception-3.1) and from
glibc's headers (LGPL-2.1-or-later), as any program built on Linux; the GCC
Runtime Library Exception allows distributing the result under any terms.
Replacing them means libc++ and compiler-rt for both LLVMs and every
solver.

### LLVM license notices

Both LLVMs ship the upstream notice: `LLVM-LICENSE.txt` is
`llvm/LICENSE.TXT` at `LLVM_REVISION` (sea-mlir's LLVM + MLIR), and the
bundle adds `LLVM-18-LICENSE.txt`, `llvm/LICENSE.TXT` at `llvmorg-18.1.8`
(SeaHorn's LLVM 18). The bundle keeps Debian's copyright file of
apt.llvm.org's `llvm-18` (`share/release-licenses/llvm-18.txt`).

### Build time and size

Measured on a shared 48-core Linux host, every `docker build` pinned to 8
CPUs (`--cpuset`), 8 compile jobs:

| stage | wall time |
|---|---|
| `llvm` (LLVM + MLIR at the pin, from source) | 54 min (its last 23 min shared the 8 CPUs with an earlier `seahorn` build, with clam) |
| `seahorn` (SeaHorn without clam, with Yices, unit tests; z3 4.12.0 cached) | 4.5 min |
| `solvers` (z3, Bitwuzla, Yices) | 12 min |
| `sea-mlir` (build, `check-sea`: 669 tests pass, install) | 8 min |
| `package`, `bundle-package`, both runtimes, both smokes | 3 min |

With the `llvm`, `solvers` and `seahorn` images cached, `just
binary-release OUT --bundle --llvm-image IMAGE` took 11 minutes. Docker
keeps the `llvm` layer until `LLVM_REVISION` moves. Artifacts: the
standalone tarball is 34 MB, the bundle 108 MB (gzip).

Checked in a fresh `ubuntu:24.04` with the bundle and `clang-18 python3`
from apt: `-m64` export and `sea-vc` give sat (replayed) and unsat; the
default `-m32` fails without `libc6-dev-i386` (`bits/libc-header-start.h`
not found) and works with it; two C inputs fail with `llvm-link not
found` until `llvm-18` is installed.

## 3. (b) Homebrew

### Layout

A tap is a GitHub repository named `homebrew-<name>`. The tap is
`seahorn/homebrew-seahorn` (`brew tap seahorn/seahorn`), formulae:

| formula | contents | dependencies |
|---|---|---|
| `sea-mlir-llvm` | LLVM + MLIR at the pin, keg-only, versioned by the pin (`24.0.0-git.302c842d6601`) | build-only for `sea-mlir` |
| `sea-mlir` | sea-vc, sea-run, sea-opt, sea-reduce, sea-seam-export, sea-reduce-test, profiles; the pinned z3, Bitwuzla and Yices as resources built into `libexec` | `gmp`, `mpfr`; build: `cmake`, `ninja`, `meson`, `boost`, `sea-mlir-llvm` |
| `seahorn` | `sea`, seahorn, seapp, seaopt, seainspect (without clam); z3 4.12.0 as a resource in `libexec`; Yices from `sea-mlir`'s `libexec` | `llvm@18`, `gmp`, `mpfr`, `python@3`, `sea-mlir` (for the export flow); build: `cmake`, `ninja`, `boost` |

Formula, not cask: these are command-line tools built from source, with
dependencies; casks are for prebuilt GUI apps and installers. Bottles are
the binary releases.

- **Solvers are bundled resources, not Homebrew's.** Homebrew's `z3` is a
  release, not the master commit pinned; verdict reproducibility rests on
  the pins; Yices is not in homebrew-core; two z3 versions cannot both be
  Homebrew's `z3`. Each formula builds its solvers from `resource` blocks
  (URL of the pinned commit's tarball + sha256) into its `libexec`, and
  links them with an rpath to `libexec/lib`. Homebrew's own relocation
  (`@@HOMEBREW_PREFIX@@` placeholders on Linux, install-name rewriting on
  macOS) then keeps bottles relocatable to other prefixes.
- **The MLIR LLVM** is its own keg-only formula, used only to build
  `sea-mlir` (static linking: no runtime dependency). It is bottled once
  per pin move, so a sea-mlir bottle build does not rebuild LLVM. Its
  version string carries the pin; `sea-mlir` declares
  `depends_on "seahorn/seahorn/sea-mlir-llvm" => :build` and checks
  `LLVM_REVISION` against it.
- **SeaHorn's LLVM 18** is Homebrew's `llvm@18`: on macOS SeaHorn is expected to link
  its `libLLVM.dylib` (a hypothesis that the `build-macos.sh report` stage
  checks), so it would be a runtime dependency, and `sea` needs its
  `clang`. The formula installs `sea` through `write_env_script` with
  `PATH` = `llvm@18`'s `bin` first. Risk: Homebrew deprecates old versioned
  LLVMs; if `llvm@18` is disabled, the tap needs its own `llvm@18`
  (18.1.8), bottled once.
- **Linux bottles** use Homebrew's own glibc and GCC runtime, independent
  of the Ubuntu tarball.

### Installed paths

Homebrew installs each formula into its own keg and links `bin/` and
`share/` into the prefix (`/opt/homebrew` on macOS arm64,
the `linuxbrew` user's home on Linux); `libexec/` is never linked.

| what | tarball (`<root>` = unpacked directory) | Homebrew (`$(brew --prefix)`, keg `$(brew --prefix sea-mlir)`) |
|---|---|---|
| tools | `<root>/bin/sea-vc` ... | `$(brew --prefix)/bin/sea-vc` ... (links into the keg) |
| SeaHorn | `<root>/bin/sea` | `$(brew --prefix)/bin/sea` (env script: `llvm@18`'s `bin` first on `PATH`) |
| solvers | `<root>/lib/libz3.so.*`, `libyices`, `libbitwuzla*` | `$(brew --prefix sea-mlir)/libexec/lib`; SeaHorn's z3 4.12 in `$(brew --prefix seahorn)/libexec/lib` |
| profiles | `<root>/share/sea-mlir/profiles` | `$(brew --prefix)/share/sea-mlir/profiles` |
| docs | `<root>/share/doc/sea-mlir/docs` | `$(brew --prefix)/share/doc/sea-mlir/docs` |
| SeaHorn data | `<root>/share/seahorn`, `<root>/lib/seapy` | in the `seahorn` keg (`share/seahorn`, `libexec`) |
| pins, hashes | `<root>/RELEASE-MANIFEST.json` | `$(brew --prefix sea-mlir)/RELEASE-MANIFEST.json` |

In both, the tools find their libraries relative to the binary
(`$ORIGIN/../lib`, `@loader_path`), so neither needs `LD_LIBRARY_PATH`;
the tarball user adds `<root>/bin` to `PATH`.

### Docs for agents

Each tool's `--agent` is the first stop; it cites the long pages as
`docs/agents/slow-vc.md`, `docs/design/Ackermann.md` and so on, relative to
the docs root. Both packages install that root, at the version of the
binary, but nothing yet tells an agent where it is. Proposal:

- every `--agent` guide starts with a `DOCS:` line computed at run time:
  `<exe>/../share/doc/sea-mlir/docs` when it exists (an install), else the
  source tree's `docs/` (a build directory), and the web site
  (https://seahorn.github.io/sea-mlir/, which follows `main`, not the
  release); a test checks that every `docs/...` path a guide cites
  exists;
- the tarball's top-level `README.md` is a sea-mlir release README (today
  the bundle's is SeaHorn's): the paths of this table, the apt line, and
  `share/doc/sea-mlir/docs/agents/index.md` as the agents' entry page;
- the formula's `caveats` print the docs path.

### Bottles

The tap gets the workflows `brew tap-new` generates: `tests.yml` runs
`brew test-bot` on each pull request (build from source, `brew test`,
bottle) on the runners of each target (macOS 14 / 15 arm64, Ubuntu x86-64),
and `publish.yml` (`brew pr-pull`, on the `pr-pull` label) uploads the
bottles to a GitHub release of the tap repository and commits the `bottle
do` block. Constraints:

- GitHub-hosted runners have 3-4 cores and a 6 h job limit. LLVM + MLIR
  fits on macOS arm64 runners (estimated 1.5-3 h), and it is built only
  when `sea-mlir-llvm` changes. If it does not fit, bottle that formula
  from a self-hosted runner or the release machine and upload with
  `brew bottle --json` + `brew pr-upload`.
- `brew test` blocks: `sea-vc --version` names the pinned solvers;
  sat / unsat runs on the smoke programs (`docker/smoke`); for `seahorn`,
  `sea yama -y sea-mlir-export -m64` on `docker/smoke/sat.c` then `sea-vc`.

### Versions

`sea-mlir` uses the sea-mlir tag. `seahorn` has no releases: version it by
date and commit, e.g. `18.0.0-dev.<yyyymmdd>` with the pinned dev18
commit in the `url` (`revision:`), until SeaHorn makes a release.

## 4. (c) macOS

[BuildMacOS.md](../BuildMacOS.md) is the guide; `utils/build-macos.sh`
the script. Review of Linux-only assumptions:

Fixed on this branch (separate commits):
- `lib/Expr/CMakeLists.txt`: the explicit Bitwuzla `-rpath` (a fixed
  absolute path in installs; on macOS an `LC_RPATH` to the build machine).
- `sea-vc --mem-limit`: macOS accepts `RLIMIT_AS` but does not enforce it;
  sea-vc now warns `vc.mem-limit-unenforced` there (not run on macOS yet).
- Tests: `sed -i '...'` (BSD sed needs a suffix) in three tests;
  coreutils `timeout` in `test/VC/seam/nki-blowup-regression.test` (now
  `%timeout`: `timeout` or `gtimeout`, `REQUIRES: timeout`).
- `utils/reduce-test.py` (installed as `sea-reduce-test`) and `bench.py`:
  `ru_maxrss` is bytes on macOS, KiB on Linux.
- `utils/release-check.sh`: no `ldd` on macOS.

Already portable: `lib/Support/ProcMem.cpp` (`/proc` under `__linux__`, the
peak from `getrusage` in bytes on macOS; `--max-rss` is an error where
`/proc` is missing); SeaHorn's CMake sets `@loader_path/../lib` on Darwin;
the CMake RPATH handling is generic.

Left for the macOS build to settle (in [BuildMacOS.md](../BuildMacOS.md),
"What to check"):
- `-DLLVM_ENABLE_LLD=ON` (sea-mlir docs) and `SEA_ENABLE_LLD=ON` (SeaHorn)
  are Linux settings; on macOS they would select `ld64.lld`. The script
  leaves both off.
- Solver install names: z3, Bitwuzla (Meson) and Yices (autoconf) produce
  dylibs whose install names and sibling references the script rewrites
  to `@rpath`, then re-signs. A formula gets this from Homebrew.
- `docker/smoke/check.sh` and `release-check.sh` read ELF magic and use
  `ldd`; a macOS smoke uses `otool -L` (`build-macos.sh report`).
- `utils/bench.py` reads `/proc/meminfo` for the memory guard (no guard on
  macOS: it returns infinity) and sets `RLIMIT_AS` (not enforced).
- SeaHorn: `sea` searches `clang-mp-18`, `clang-18`, `clang` on `PATH`;
  on macOS that `clang` may be Apple's (newer bitcode that LLVM 18 cannot
  read). Homebrew's `llvm@18/bin` must come first; the formula's wrapper
  does that. SeaHorn's default `-m32` is unavailable on macOS.
- Exported programs from an arm64 host carry an AArch64 triple and data
  layout; not tried.
- Tests that compare text containing `.so` names, Linux paths or
  GNU-specific tool output; only a macOS `check-sea` run finds them.

### Signing and notarization

No notarization: it needs a paid Apple Developer ID. macOS ships as
Homebrew bottles only.

- Bottles need no notarization. `brew` downloads them with `curl`, which
  sets no `com.apple.quarantine` attribute, so Gatekeeper does not check
  them. On arm64 every binary needs a valid signature; an ad-hoc one
  (`codesign -s -`) is enough. Homebrew re-signs ad hoc after it rewrites
  install names when it pours a bottle; `utils/build-macos.sh` does the
  same after its own `install_name_tool` edits (`codesign -f -s -` on each
  solver dylib and on `bin/z3`).
- A plain macOS tarball is not built. Downloaded by a browser, it gets the
  quarantine attribute and Gatekeeper blocks its unsigned, unnotarized
  binaries. It would work when downloaded with `curl`, or after
  `xattr -d -r com.apple.quarantine <dir>`; both are workarounds to
  document, so bottles are the recommended macOS channel.

## 5. Timeline of a release with these artifacts

1. Tag sea-mlir; SeaHorn pins move to the dev18 commits that passed CI.
2. `just binary-release OUT --bundle --image-prefix sea-mlir-release
   --cpuset <8 cpus>` on a clean checkout of the tag.
3. Review the manifest (`license_audit`, `"seahorn"`) and
   `THIRD-PARTY-NOTICES`; upload both tarballs to the sea-mlir GitHub
   release.
4. Tap: bump `sea-mlir` (`url`/`sha256` of the tag, resource pins), and
   `seahorn`; open the tap PR; `brew test-bot` builds the bottles; label
   `pr-pull` to publish.

## 6. Open decisions

- **LLD per platform**: the docs should say per platform which linker
  settings apply (`-DLLVM_ENABLE_LLD=ON` is Linux-only).
- **SeaHorn headers in the bundle**: the bundle ships the SeaHorn and
  sea-dsa headers (`include/`); keep or drop.
- **One z3**: the release ships two z3s (section 1). One z3 for both needs
  the Spacer regression in z3 master found and fixed.
- **Clam / crab**: off (section 1); revisit with pins and crab's IKOS
  terms.
- **macOS tarball**: bottles only (section 4); a tarball only on request.
- **Docs for agents**: the `DOCS:` line, the release README and the
  caveats (section 3, "Docs for agents").

## 7. Release-day checklist

- [ ] `LLVM_REVISION` and the solver pins in `docker/Dockerfile` match
      `docs/Release.md`; `just release-check` passes on the tag.
- [ ] SeaHorn pins in `docker/SeaHorn.Dockerfile` moved to the chosen
      dev18 commits; SeaHorn CI green at those commits.
- [ ] `just binary-release OUT --bundle` from a clean checkout of the tag:
      `release-run.json` status `complete`, `source_dirty: false`.
- [ ] Both smoke stages pass (fresh `ubuntu:24.04`).
- [ ] `sea-vc --version` in the tarball names z3 5.4.2 (`bbd46c9`), Yices
      2.8.0, Bitwuzla 0.9.1.
- [ ] Manifest: every ELF dependency inside the prefix or a system library;
      no absolute RUNPATH.
- [ ] License gate passed in both packaging stages; `license_audit` in
      both manifests, `THIRD-PARTY-NOTICES` and `share/licenses/release/`
      reviewed.
- [ ] `docker/seahorn-yices.patch` gone once SeaHorn's fix is in the
      pinned dev18 commit.
- [ ] Tarballs and `.sha256` attached to the GitHub release (on the
      maintainer's word).
- [ ] Tap PR: `sea-mlir` and `seahorn` formulae bumped; bottles built by
      `brew test-bot` for macOS arm64 and Linux x86-64; `pr-pull`.
- [ ] `brew install seahorn/seahorn/seahorn` on a clean macOS and Linux
      machine; the export smoke runs.
