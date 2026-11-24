# Building on macOS

sea-mlir and SeaHorn dev18 have not been built on macOS before this guide.
It is a first-build checklist: `utils/build-macos.sh` builds everything from
source at the release pins, runs the tests and a smoke run, and writes logs
and timings that say what works. Apple Silicon is the expected machine; the
script is meant to run on x86-64 Macs too; they are untested (section 5).

What gets built, each at the pin the Linux release uses:

| component | pin (single definition) | installed to |
|---|---|---|
| LLVM + MLIR | `LLVM_REVISION` | `$ROOT/install/llvm` |
| z3 for sea-mlir | `Z3_REVISION` in `docker/Dockerfile` | `$ROOT/install/solvers` |
| Bitwuzla | `BITWUZLA_TAG` in `docker/Dockerfile` | `$ROOT/install/solvers` |
| Yices | `YICES_TAG` in `docker/Dockerfile` | `$ROOT/install/solvers` |
| sea-mlir | this checkout | `$ROOT/install/sea-mlir` |
| z3 for SeaHorn | `SEAHORN_Z3_REVISION` in `docker/SeaHorn.Dockerfile` (4.12.0) | `$ROOT/install/seahorn-z3` |
| SeaHorn, sea-dsa, llvm-seahorn, clam, crab | the `*_REVISION` arguments of `docker/SeaHorn.Dockerfile` | `$ROOT/install/seahorn` |
| LLVM 18 for SeaHorn | Homebrew `llvm@18` (not built) | |

`$ROOT` is `~/sea-macos` unless `--root` or `SEA_MACOS_ROOT` says
otherwise. It needs about 30 GB.

## 1. Prerequisites

```bash
xcode-select --install          # Apple clang, ld, SDK
brew install cmake ninja meson pkg-config autoconf gperf \
  boost gmp mpfr coreutils llvm@18
python3 -m venv ~/sea-macos/venv && ~/sea-macos/venv/bin/pip install lit pyyaml
export PATH=~/sea-macos/venv/bin:$PATH
```

- `llvm@18` is SeaHorn's LLVM (keg-only: not on `PATH`). SeaHorn links it,
  and `sea` compiles C with its `clang`. If Homebrew no longer offers
  `llvm@18`, stop and report: SeaHorn then needs LLVM 18.1.8 built from
  source (tag `llvmorg-18.1.8`).
- `coreutils` provides `gtimeout`, which one sea-mlir test needs (it is
  reported unsupported without it).
- The venv's `bin` must be first on `PATH` in every shell that runs the
  script: it provides `lit` and the `python3` with PyYAML that `sea` uses
  (`#!/usr/bin/env python3`). Homebrew's Python refuses `pip install --user`
  (PEP 668), hence the venv.

`utils/build-macos.sh check` verifies all of this and prints the pins.

## 2. Build

```bash
cd sea-mlir
utils/build-macos.sh check
utils/build-macos.sh llvm          # the long one: about 1 h on 8-10 cores
utils/build-macos.sh z3 bitwuzla yices
utils/build-macos.sh sea-mlir      # builds, runs check-sea, installs
utils/build-macos.sh seahorn       # builds, installs, runs SeaHorn's lit
utils/build-macos.sh smoke report
```

Or `utils/build-macos.sh` for all stages in order. Each stage writes
`$ROOT/logs/<stage>.log` and appends its time to `$ROOT/logs/timings.txt`.
A failing stage stops the script; rerun from that stage. On the first
build, failing tests are expected findings: `--keep-going` installs
sea-mlir and SeaHorn anyway, and the failures are listed in
`$ROOT/logs/check-sea-failures.txt` and `$ROOT/logs/seahorn-lit.log`.

SeaHorn's lit is expected to fail on macOS: most `%sea` RUN lines use the
default `-m32`. A full run without `--keep-going` therefore stops at the
`seahorn` stage; run `utils/build-macos.sh smoke report` afterwards.

Options: `--jobs N` (default: all cores), `--seahorn-z3 shared` (SeaHorn
against the sea-mlir z3 instead of 4.12.0; on Linux this breaks Spacer,
see [design/BinaryRelease.md](design/BinaryRelease.md)), `--seahorn-yices`
(needs a SeaHorn revision that builds with Yices 2.8).

## 3. What the script does differently from Linux

- Apple's linker: no `LLVM_ENABLE_LLD` / `SEA_ENABLE_LLD`.
- Install RPATH `@loader_path/../lib` instead of `$ORIGIN/../lib`.
- Solver dylibs: each gets the install name `@rpath/<name>`, its references
  to sibling solver dylibs are rewritten to `@rpath/...`, it gets an
  `@loader_path` rpath, and it is signed again ad hoc (`codesign -f -s -`).
  An arm64 binary edited by `install_name_tool` without a new signature is
  killed by the kernel at load time.
- LLVM: host target only, zlib from the SDK, no zstd and no libxml2, so
  the sea-mlir tools take nothing from Homebrew through LLVM.
- Boost, GMP and MPFR come from Homebrew (`CMAKE_PREFIX_PATH`).
- SeaHorn is compiled by Apple clang against Homebrew's `llvm@18`.
- The smoke export uses `-m64`: SeaHorn's default `-m32` needs 32-bit
  headers, which macOS does not have.

## 4. What to check and report

1. `$ROOT/logs/timings.txt`: the time of each stage and the core count.
2. `check-sea`: the failing tests. Known macOS-specific suspects:
   - `--mem-limit` tests: macOS does not enforce `RLIMIT_AS` (sea-vc prints
     `vc.mem-limit-unenforced`); a test that expects `mem-limit: hit` from
     the process limit fails.
   - `--max-rss` needs `/proc` and is an error on macOS
     (`vc.rss-budget-unsupported`).
   - `sea-reduce-test` memory caps (Python `resource.setrlimit(RLIMIT_AS)`)
     may raise on macOS.
   - tests whose expected text contains a Linux path, `.so`, or
     GNU-specific tool output (`sed`, `wc` padding, `stat`).
   - floating differences in solver behavior: z3 / Yices / Bitwuzla built
     by Apple clang may take other search paths; verdicts must not change,
     statistics may.
3. SeaHorn's lit (`$ROOT/logs/seahorn-lit.log`): expected on Linux at the
   pin is 419 tests, 11 expected failures, 1 unsupported, the rest passing.
   Tests that compile C use `-m32` by default and may fail on macOS for
   that reason alone; note which.
4. The smoke stage: C exported on an arm64 host has an AArch64 data
   layout and triple. sea-vc should not care; if it refuses the
   program, that is a finding.
5. `$ROOT/logs/report.txt` (the `report` stage): the dylibs each binary
   loads. The summary lists the Homebrew dylibs (expected: gmp, gmpxx,
   mpfr, `llvm@18`'s libLLVM for SeaHorn) and absolute rpaths (expected:
   none in `install/sea-mlir`). These are the runtime dependencies a
   Homebrew formula declares.
6. `sea-vc --version` names the solver versions that were loaded.

Send back `timings.txt`, `report.txt`, the failure lists, the
`$ROOT/logs/<stage>.log` of any failed stage and the
`--version` output; they decide the Homebrew formula's dependencies and
which tests need a macOS variant.

## 5. Not covered

- Intel Macs and macOS releases before 14 were not considered.
- No notarization: binaries built here are ad-hoc signed and run on this
  machine. A downloaded tarball would also carry the quarantine attribute
  (`xattr -d -r com.apple.quarantine <dir>`). Homebrew bottles avoid both.
- `DYLD_LIBRARY_PATH` is stripped when a protected binary such as
  `/bin/sh` starts a process, so nothing here relies on it; the rpaths
  must be right.
