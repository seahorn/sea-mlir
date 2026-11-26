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
re-bottling a published version bumps the formula's `revision`. macOS
bottles are built on a Mac ("Bottles: macOS" below); their lines merge into
the same `bottle do` blocks.

## Bottles: macOS

Built by hand on an Apple Silicon Mac, from the tap's formulae (the
release's `url` and `sha256`); from sea-mlir 0.6.1 on (0.6.0 does not
build on macOS). A bottle installs on the macOS release it
was built on and on newer ones (its tag: `arm64_tahoe` = macOS 26,
`arm64_golden_gate` = macOS 27): build on the oldest macOS to support.
Only `sea-mlir` (and later `seahorn`) is bottled: `sea-mlir-llvm` is
built from source on the Mac as a build dependency and is never needed to
install a bottle.

Needs: the Xcode command-line tools (`xcode-select --install`), Homebrew
7.0 or newer at its default prefix (`/opt/homebrew`), and about 20 GB of
free disk (the LLVM build tree, the `sea-mlir-llvm` keg of 3.1 GB, the
build dependencies of about 1 GB). Times on a 14-core M-series Mac:
`sea-mlir-llvm` 8 min, `sea-mlir` 4 min (solvers and `check-sea`
included).

```bash
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1 HOMEBREW_NO_ENV_HINTS=1 \
       HOMEBREW_NO_INSTALL_CLEANUP=1 HOMEBREW_NO_AUTOREMOVE=1
# the tap at the commit whose formulae carry the release (url, sha256)
brew tap seahorn/seahorn
git -C "$(brew --repository seahorn/seahorn)" checkout <tap commit or branch>
brew trust seahorn/seahorn          # Homebrew 7 loads other taps' formulae only once trusted
mkdir -p ~/bottles && cd ~/bottles
version=$(brew ruby -e 'puts Formula["seahorn/seahorn/sea-mlir"].version')
brew install --build-bottle seahorn/seahorn/sea-mlir    # builds sea-mlir-llvm first; runs check-sea
brew linkage --test --strict seahorn/seahorn/sea-mlir  # only declared dependencies (gmp, mpfr)
brew test seahorn/seahorn/sea-mlir
brew bottle --json --no-rebuild \
  --root-url="https://github.com/seahorn/homebrew-seahorn/releases/download/sea-mlir-$version" \
  seahorn/seahorn/sea-mlir
```

`brew bottle` writes `sea-mlir--<version>.<tag>.bottle.tar.gz` (two
dashes) and its `.json`; the download name is the JSON's `filename`
(`sea-mlir-<version>.<tag>.bottle.tar.gz`, one dash): rename the tarball
to it before uploading, as `docker/homebrew-bottle.sh` does.

Before uploading, install the bottle as a user would, from the bottle only:
copy the tap's formulae into `DIR/Formula`, add the `bottle do` line from
the JSON, put the renamed tarball into `DIR/bottles`, remove the kegs built
from source and run `install-local.sh` (it fails unless every keg was
poured from a bottle):

```bash
HOMEBREW_NO_AUTOREMOVE=1 brew uninstall seahorn/seahorn/sea-mlir seahorn/seahorn/sea-mlir-llvm
brew untap --force seahorn/seahorn
bash install-local.sh DIR sea-mlir && brew test seahorn/seahorn/sea-mlir
```

Release: upload the renamed tarball to the tap release
`sea-mlir-<version>` (`gh release upload sea-mlir-<version> <file> --repo
seahorn/homebrew-seahorn`) and merge its line into the tap's formula,
next to `x86_64_linux` (`brew bottle --merge --write --no-commit
*.bottle.json` in the tap checkout; not yet tried on this tap).

Traps:
- `HOMEBREW_NO_AUTOREMOVE=1`: without it `brew uninstall` also removes
  dependencies nothing else in Homebrew uses (for example a `python@3.x`
  that virtualenvs outside Homebrew still use).
- The build runs in macOS's sandbox: a formula's `install` cannot fetch
  (a resource's `fetch` takes a lock under the Homebrew prefix that the
  sandbox denies; `sea-mlir.rb` uses `cached_download`), and its `PATH`
  holds only the declared dependencies' `bin` directories and `/usr/bin`.
- `check-sea` and `brew test` crash some processes on purpose (death tests,
  `test/Tools/Inputs/fake-sea-vc.py`); macOS writes crash reports for them.
