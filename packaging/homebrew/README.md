# Homebrew tap `seahorn/seahorn`

The formulae of the tap `seahorn/homebrew-seahorn`. The tap repository
does not exist yet; it is created from this directory (`Formula/` at its
root). Design: [BinaryRelease.md](../../docs/design/BinaryRelease.md),
section 3.

| formula | what |
|---|---|
| `sea-mlir-llvm` | LLVM + MLIR at `LLVM_REVISION`, keg-only, a build dependency of `sea-mlir` only |
| `sea-mlir` | the sea-mlir tools; z3 `bbd46c9`, Bitwuzla 0.9.1 (CaDiCaL, SymFPU) and Yices 2.8.0 built from resources into `libexec` |
| `seahorn` | SeaHorn dev18 without clam, z3 4.12.0 in `libexec`, Yices from `sea-mlir`; `sea` runs llvm@18's clang |

The pins are those of `docker/Dockerfile`, `docker/SeaHorn.Dockerfile` and
`LLVM_REVISION`; `docker/test_release.py` checks that they agree.

On Linux every formula compiles with clang 18 and links with lld from
llvm@18, the toolchain of the tested builds, not with Homebrew's GCC and
GNU ld (docs/design/BinaryRelease.md, section 3, "Toolchain").

## Bottles: local build

```bash
python3 docker/release.py OUTPUT --homebrew --cpuset 40-47 [--brew-source REF]
```

builds, in `ghcr.io/homebrew/brew` pinned by digest, each formula from
source (`brew install --build-bottle`), checks `brew linkage --test
--strict`, runs `brew test`, bottles it
(`brew bottle --json`), merges the `bottle do` blocks, runs the license
gate over every bottle (`docker/package-release.py --audit-bottles`) and
installs the bottles in a fresh container with `install-local.sh`. Output
in `OUTPUT/artifacts/homebrew`: `bottles/` (tarballs under their release
names, JSON), the formulae with their bottle blocks (`Formula/` for
`--brew-source formula`, else `Formula-local/`), `license-audit.json`,
`timings.tsv`, `INSTALL-TEST.md`, `install-local.sh`, build logs.

The `root_url` of each bottle is the release `<formula>-<version>` of the
tap repository (`https://github.com/seahorn/homebrew-seahorn/releases/download/<formula>-<version>`),
the layout `brew pr-upload` uses; `install-local.sh` points it at a local
directory instead.

## The sea-mlir source

- Release: `sea-mlir.rb`'s `url` is the tag's tarball,
  `https://github.com/seahorn/sea-mlir/archive/refs/tags/v<version>.tar.gz`,
  with its `sha256` (the committed one is a placeholder until the tag
  exists). `--brew-source formula` builds it as written; it is the only
  build whose formulae may go to the tap (`artifacts/homebrew/Formula/`:
  the committed formulae with only the bottle blocks merged in).
- Local (default): `--brew-source REF` (default `HEAD`) builds a
  `git archive` of REF. The build context's copy of `sea-mlir.rb` gets
  `url "file:///home/linuxbrew/src/sea-mlir-<version>.tar.gz"`, its
  `sha256` and `version "<project version>-local.<commit>"` (the project
  version when REF is the tag `v<project version>`). The formula passes
  `-DSEA_GIT_DESCRIBE=v<version>`, so a local bottle never claims a tag.
  Its formulae land in `artifacts/homebrew/Formula-local/`, each headed
  `# NOT FOR PUBLISHING`: for `install-local.sh` tests only.

## Release

From a `--brew-source formula` build only: upload each bottle
(`bottles/*.bottle.tar.gz`, already under its `filename`) with
`gh release upload` to the release `<formula>-<version>` of the tap
repository (`brew pr-upload` cannot consume this directory), and commit
`Formula/` with the `bottle do` blocks. Bottling uses `--no-rebuild`:
re-bottling a published version bumps the formula's `revision`. macOS bottles are built on a Mac with the same
commands (`brew install --build-bottle`, `brew test`, `brew bottle --json`,
`brew bottle --merge --write`); their blocks merge into the same formulae.
