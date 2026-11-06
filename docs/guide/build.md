# Build and install

sea-mlir 0.1.0 is a research prototype. The text format of the dialect and
the command-line options can change between releases. Linux on x86-64 is
the only platform that is built and tested.

## Dependencies

| Dependency | Version | Notes |
|---|---|---|
| LLVM and MLIR | the commit in `LLVM_REVISION` (`93f05d753ad3`) | built with `-DLLVM_ENABLE_PROJECTS=mlir -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_INSTALL_UTILS=ON`, then installed; other revisions are not supported |
| z3 | tested with 4.12.0 and 5.3.3 | the supported solver |
| Yices 2 | tested with 2.6.4 and 2.7.0 | optional secondary solver (`-DSEA_ENABLE_YICES=AUTO\|ON\|OFF`); not in release binaries (GPLv3) |
| Bitwuzla | 0.9 (at least 0.5) | optional secondary solver |
| Boost | 1.71 or later, headers only | tested with 1.83 |
| GMP | with `gmpxx` | tested with 6.3 |
| CMake, Ninja, a C++17 compiler | | tested with clang 18 and lld |
| lit, FileCheck | from the LLVM install or `pip install lit` | tests only |
| Python 3, PyYAML | | tests, `sea-reduce-test`, benchmarks |

## Configure, build, test, install

```bash
cmake -S . -B build-release -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 -DLLVM_ENABLE_LLD=ON \
  -DMLIR_DIR=<llvm-install>/lib/cmake/mlir \
  -DZ3_ROOT=<z3-install> -DSEA_Z3=<z3-install>/bin/z3 \
  [-DYICES2_ROOT=<yices-install>] [-DBITWUZLA_ROOT=<bitwuzla-install>] \
  -DCMAKE_INSTALL_PREFIX=<prefix>
ninja -C build-release            # build
ninja -C build-release check-sea  # the regression tests; every test must pass
ninja -C build-release install
```

`-DSEA_Z3` names the z3 binary that the tests use. Without
`-DBITWUZLA_ROOT`, `--solver=bitwuzla` is an error. Yices is used when
found (`-DSEA_ENABLE_YICES=AUTO`, the default); `ON` requires it and `OFF`
leaves it out. Without it, `--solver=yices` and the `--yices-*` options
are errors and the tests that need it are reported as unsupported.

`just docker-build` builds the same in Docker from scratch: LLVM at the
pin, z3 and Bitwuzla at pinned versions, sea-mlir with `check-sea`
(`docker/README.md`).

## Check the build

```console
$ sea-vc --version
sea-vc (sea-mlir 0.1.0 (...))
  LLVM/MLIR 24.0.0git at 93f05d753ad3
  build: Release, assertions on
  z3 4.12.0.0
  yices 2.6.4
  bitwuzla 0.9.1
```

The version line names the sea-mlir commit, the LLVM revision and the
solver libraries that were loaded. A Release build against an LLVM built
with assertions keeps assertions on, and `--version` says so.

## What is installed

| Path | What |
|---|---|
| `bin/sea-vc` | the verifier: VC generation, solving, counterexample replay |
| `bin/sea-run` | the interpreter, the reference semantics of the dialect |
| `bin/sea-opt` | `mlir-opt` with the sea dialect and its passes |
| `bin/sea-reduce`, `bin/sea-reduce-test` | test-case reduction ([The reducer](../reference/reducer.md)) |
| `bin/sea-seam-export` | SeaM terms as JSON, for the Lean conformance checks ([SeaM export](../reference/seam-export.md)) |
| `share/sea-mlir/profiles/` | example profiles ([Profiles](../reference/profiles.md)) |
| `share/doc/sea-mlir/` | `README.md`, `CHANGELOG.md`, `docs/` |

The C++ libraries and headers are not installed; there is no released
API. The scripts in `utils/` (benchmarks, fuzzers, oracles) are
development tools that run from a source tree.

## Runtime requirements

`sea-run`, `sea-opt` and `sea-reduce` need only the C and C++ runtime,
zlib and zstd. LLVM and MLIR are linked statically.

`sea-vc` and `sea-seam-export` link the solvers as shared libraries:
`libz3.so` with `libgmp` and `libgmpxx`; with Bitwuzla,
`libbitwuzla*.so` and `libmpfr`; with Yices, `libyices.so.2.*`. The installed binaries
keep the RUNPATH of the solver directories they were linked against, so an
install runs in place without `LD_LIBRARY_PATH` while those directories
exist. To move an install to another machine, install the same solver
libraries there and set `LD_LIBRARY_PATH`, or configure with
`-DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF` and install the solvers
system-wide. `ldd <prefix>/bin/sea-vc` must not print `not found`.

## Where programs come from

sea-mlir reads programs in the `sea` dialect. Programs from C come from
SeaHorn: its exporter (`--horn-sea-mlir-export`) compiles a harness to
LLVM bitcode, unrolls loops to a bound, and writes a `sea.program`. The
exporter is part of SeaHorn, not of sea-mlir. Other front ends write the
dialect directly; the [NKI harness](../tutorials/nki.md) traces Python
kernels into it. Small programs are also written by hand, as in the
[next page](getting-started.md).
