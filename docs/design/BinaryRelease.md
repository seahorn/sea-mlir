# Binary releases: Linux tarball, Homebrew, macOS

Status: plan for the release after v0.5.0. The Linux pipeline exists (`docker/`,
`docker/README.md`) and builds both artifacts; nothing is published. The
Homebrew tap and the macOS build are proposals; macOS has not been built.

This note covers three deliverables for the next release of sea-mlir
together with SeaHorn dev18:

- (a) a relocatable tarball for Ubuntu 24.04 x86-64 with sea-mlir, the
  SeaHorn frontend and the solvers;
- (b) Homebrew packages with bottles for Linux and macOS, in a tap;
- (c) a first macOS build ([BuildMacOS.md](../BuildMacOS.md),
  `utils/build-macos.sh`).

## 1. Components and pins

Two LLVMs, two z3s. Neither LLVM is shipped as a shared library: sea-mlir
links LLVM/MLIR statically, and SeaHorn links LLVM 18 statically (the
apt.llvm.org `llvm-18-dev` static libraries).

| component | pin | where the pin is defined | license |
|---|---|---|---|
| LLVM + MLIR (sea-mlir) | `302c842d6601` (LLVM 24 `main`) | `LLVM_REVISION` | Apache-2.0 WITH LLVM-exception |
| z3 (sea-mlir) | master `bbd46c9` (reports 5.4.2) | `Z3_REVISION`, `docker/Dockerfile` | MIT |
| Bitwuzla | 0.9.1 (with CaDiCaL, SymFPU from its wraps) | `BITWUZLA_TAG` | MIT; SymFPU BSD-3-Clause |
| Yices | 2.8.0, without MCSAT | `YICES_TAG` | BSD-3-Clause |
| SeaHorn | dev18 `67474bf` | `SEAHORN_REVISION`, `docker/SeaHorn.Dockerfile` | Apache-2.0 WITH LLVM-exception |
| sea-dsa | dev18 `1e33f16` | `SEADSA_REVISION` | Apache-2.0 WITH LLVM-exception |
| llvm-seahorn | dev18 `9225814` | `LLVM_SEAHORN_REVISION` | Apache-2.0 WITH LLVM-exception |
| clam (crab-llvm) | dev18 `3eaf016` | `CLAM_REVISION` | Apache-2.0 |
| crab | dev `49773d0` | `CRAB_REVISION` | Apache-2.0, and the IKOS license for code derived from IKOS |
| LLVM 18 (SeaHorn) | 18.1.8, apt.llvm.org `llvm-toolchain-noble-18` | `LLVM18_VERSION` | Apache-2.0 WITH LLVM-exception |
| z3 (SeaHorn) | 4.12.0 `feda706` | `SEAHORN_Z3_REVISION` | MIT |
| GMP, MPFR | Ubuntu 24.04 packages, copied into `lib/` | | LGPL-3.0+ (dynamically linked, replaceable) |

### One z3 or two

SeaHorn dev18 configures with any z3 >= 4.8.9 (`cmake/FindZ3.cmake`); its
CI uses 4.8.9 and local builds 4.12.0. Built against the sea-mlir pin (z3
`bbd46c9`, Yices 2.8.0; clam on), SeaHorn compiles, and its lit suite gave
406 passed, 11 expected failures, 1 unsupported and 1 failure out of 419:

- `test/tutorial/add_by_one.c` (`sea pf`, Spacer): the `-O0` run did not
  finish in 120 s; with z3 4.12.0 it takes 0.5 s. A Spacer regression in
  z3 master, or a parameter SeaHorn sets that changed meaning; not
  investigated.
- `units_z3` (`expr.neg`) compares a simplified term with the exact form
  4.12's simplifier produces; master produces an equivalent but different
  term.

So the release ships two z3 libraries side by side. Their sonames differ
(`libz3.so.4.12`, `libz3.so.5.4`), each tool has its own RUNPATH to `lib/`,
and no process loads both: `seahorn` / `seapp` / `seaopt` load 4.12, `sea-vc`
loads 5.4. Only one `z3` executable is installed, the sea-mlir pin
(`bin/z3`); SeaHorn's driver does not run a z3 binary. One z3 for both is
possible when SeaHorn's Spacer works with the sea-mlir pin; the test above
is the check.

### Yices in SeaHorn

SeaHorn dev18 does not compile against Yices >= 2.7 (renamed status
constants). The fix is on the SeaHorn branch `feat/binary-release`
(`fix(smt): build the Yices backend with Yices >= 2.7`). Until it is in
dev18, the bundle builds SeaHorn without Yices, as before. Afterwards
SeaHorn can link the same `libyices.so.2.8` as sea-vc (one library, no
second Yices).

### Clam / Crab

`WITH_CLAM` defaults to ON in SeaHorn, so the bundle builds it (CI builds
SeaHorn with `WITH_CLAM=OFF`). Crab is built without Apron, Elina, LDD and
PPLite, so it adds no shared library; it is linked statically into the
SeaHorn tools. crab carries `IKOS_LICENSE.pdf` for code derived from IKOS:
the bundle includes it, and its terms are an open decision (section 6).

## 2. (a) The Linux tarball

`just binary-release OUT --bundle` (`docker/release.py`) builds, in a local
Docker, on `ubuntu:24.04` pinned by digest:

1. `llvm`: LLVM + MLIR at the pin (or `--llvm-image IMAGE` to reuse one);
2. `solvers`: z3, Bitwuzla, Yices at their pins;
3. `sea-mlir`: build, `check-sea`, install;
4. `package`: `docker/package-release.py` copies the non-system shared
   libraries into `lib/`, sets every ELF RUNPATH to `$ORIGIN`-relative,
   refuses a dependency that resolves outside the prefix or a copied
   library without a license, writes `THIRD-PARTY-NOTICES`,
   `RELEASE-MANIFEST.json` (pins, packages, ELF dependencies, hashes) and a
   reproducible tarball (fixed mtimes, owner, order);
5. `standalone-smoke`: the tarball unpacked in a fresh `ubuntu:24.04`
   container, `sea-vc` sat (with replay, z3 and Yices) and unsat;
6. with `--bundle`: `seahorn` (`docker/SeaHorn.Dockerfile`),
   `bundle-package` (the sea-mlir prefix plus the SeaHorn prefix), and
   `bundle-smoke`: in a fresh container with `apt-get install clang-18
   python3`, `sea yama -y sea-mlir-export` on `docker/smoke/{sat,unsat}.c`,
   then `sea-vc` must report the expected verdict (sat with replay).

Artifacts: `sea-mlir-<version>-linux-x86_64.tar.gz` and the bundle
`sea-mlir-bundle-<version>-linux-x86_64-held.tar.gz` (held: see section 6),
each with `.sha256` and the manifest.

### Relocatability

Installed binaries get `$ORIGIN/../lib` (`patchelf` in packaging). A plain
CMake install can produce the same without patchelf:
`-DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF '-DCMAKE_INSTALL_RPATH=$ORIGIN/../lib'`;
this needed the fix of `lib/Expr/CMakeLists.txt`, which passed Bitwuzla's
directory as an explicit `-rpath` that survived into installs (fixed on
this branch).

### Runtime requirements on a clean Ubuntu 24.04

- Standalone sea-mlir: nothing beyond the base image. glibc, libstdc++ and
  libgcc come from Ubuntu; GMP, MPFR, zlib, zstd and the solvers are in
  `lib/`. Python 3 only for `sea-reduce-test`.
- Bundle: `apt-get install clang-18 python3`. SeaHorn's `sea` compiles C
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

### Build time

See "Timings" in the report of the build; the LLVM stage is the long one
(about an hour at 8 jobs) and is cached by Docker until `LLVM_REVISION`
moves.

## 3. (b) Homebrew

### Layout

A tap is a GitHub repository named `homebrew-<name>`. Proposal:
`seahorn/homebrew-tap` (`brew tap seahorn/tap`), formulae:

| formula | contents | dependencies |
|---|---|---|
| `sea-mlir-llvm` | LLVM + MLIR at the pin, keg-only, versioned by the pin (`24.0.0-git.302c842d6601`) | build-only for `sea-mlir` |
| `sea-mlir` | sea-vc, sea-run, sea-opt, sea-reduce, sea-seam-export, sea-reduce-test, profiles; the pinned z3, Bitwuzla and Yices as resources built into `libexec` | `gmp`, `mpfr`; build: `cmake`, `ninja`, `meson`, `boost`, `sea-mlir-llvm` |
| `seahorn` | `sea`, seahorn, seapp, seaopt, seainspect, clam tools; z3 4.12.0 as a resource in `libexec` | `llvm@18`, `gmp`, `mpfr`, `python@3`, `sea-mlir` (for the export flow); build: `cmake`, `ninja`, `boost` |

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
  `depends_on "seahorn/tap/sea-mlir-llvm" => :build` and checks
  `LLVM_REVISION` against it.
- **SeaHorn's LLVM 18** is Homebrew's `llvm@18`: on macOS SeaHorn links
  its `libLLVM.dylib`, so it is a runtime dependency, and `sea` needs its
  `clang`. The formula installs `sea` through `write_env_script` with
  `PATH` = `llvm@18`'s `bin` first. Risk: Homebrew deprecates old versioned
  LLVMs; if `llvm@18` is disabled, the tap needs its own `llvm@18`
  (18.1.8), bottled once.
- **Linux bottles** use Homebrew's own glibc and GCC runtime, independent
  of the Ubuntu tarball.

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
date and commit, e.g. `18.0.0-dev.20261124` with the dev18 commit in the
`url` (`revision:`); see section 6.

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
  leaves both off; the docs should say so per platform.
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

## 5. Timeline of a release with these artifacts

1. Tag sea-mlir; SeaHorn pins move to the dev18 commits that passed CI.
2. `just binary-release OUT --bundle --image-prefix sea-mlir-release
   --cpuset <8 cpus>` on a clean checkout of the tag.
3. Review the manifest and `THIRD-PARTY-NOTICES`; upload the tarballs to
   the sea-mlir GitHub release.
4. Tap: bump `sea-mlir` (`url`/`sha256` of the tag, resource pins), and
   `seahorn`; open the tap PR; `brew test-bot` builds the bottles; label
   `pr-pull` to publish.

## 6. Open decisions

- **Tap repository**: `seahorn/homebrew-tap` (proposal) or a
  sea-mlir-specific `seahorn/homebrew-sea-mlir`.
- **Distribution of the SeaHorn bundle**: the hold
  (`NOT-FOR-DISTRIBUTION`, `-held` file names) predates the relicensing of
  seahorn, sea-dsa and llvm-seahorn to Apache-2.0 WITH LLVM-exception.
  Lifting it is the maintainer's call; it is one flag in
  `docker/package-release.py` and the file names in `release.py` / the smoke.
- **crab's IKOS license** (`IKOS_LICENSE.pdf`, NASA Open Source
  Agreement): whether binaries with crab may ship under it, or SeaHorn
  ships with `WITH_CLAM=OFF` (as its CI builds).
- **License notices**: the tarball already carries LLVM's, the solvers'
  (with Yices' third-party notices, CaDiCaL, SymFPU), the Ubuntu package
  copyrights of every copied library and the SeaHorn repositories' files.
  Open: whether LLVM 18's notice for the statically linked SeaHorn tools
  should be the upstream `LICENSE.TXT` at `llvmorg-18.1.8` in addition to
  the Debian copyright file.
- **Bundle version**: `sea-mlir-bundle-<sea-mlir version>` with the
  SeaHorn commits in the manifest (current), or a combined scheme (for
  example `<sea-mlir version>+seahorn.<dev18 short sha>`).
- **SeaHorn pins**: the dev18 heads when this note was written (table above). Which
  commits a release pins: the dev18 head that passed CI on release day, or
  a SeaHorn tag made for the release.
- **Yices in SeaHorn**: after the SeaHorn fix merges, enable it (shared
  `libyices.so.2.8`) or keep SeaHorn without Yices.
- **Two z3s**: accept, or chase the Spacer regression in z3 master so
  one z3 serves both.
- **macOS tarball**: only Homebrew bottles are planned; a standalone
  macOS tarball needs notarization to run without the quarantine dance.

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
- [ ] `THIRD-PARTY-NOTICES` and `share/licenses/release/` reviewed; hold
      decision for the bundle recorded.
- [ ] Tarballs and `.sha256` attached to the GitHub release (on the
      maintainer's word).
- [ ] Tap PR: `sea-mlir` and `seahorn` formulae bumped; bottles built by
      `brew test-bot` for macOS arm64 and Linux x86-64; `pr-pull`.
- [ ] `brew install seahorn/tap/seahorn` on a clean macOS and Linux
      machine; the export smoke runs.
