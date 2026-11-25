# Installing the sea-mlir bottles on a clean Linux machine

These bottles were built by `docker/release.py --homebrew` (Linux x86_64,
Homebrew's default Linux prefix, `.linuxbrew` in the home of the
`linuxbrew` user). They are not
published. This page tests them on a machine that has never built them.

## What you need

- Linux x86_64 with glibc 2.38 or newer and the libstdc++ of GCC 13 or
  newer (Ubuntu 24.04 or later; the bottles were built on Ubuntu 24.04), `git`, `curl`, and a C library
  development package (`build-essential` on Ubuntu). `sea` compiles C with
  llvm@18's clang, which uses the system's C headers.
- Homebrew (its default prefix is the tested one; the bottles are
  `cellar: :any`, relocatable to another prefix). Install it with
  https://brew.sh's one-line installer and run the `brew shellenv` line it
  prints.
- This directory (`artifacts/homebrew` of the build output): `Formula/`
  (the formulae with their `bottle do` blocks), `bottles/` (the bottle
  tarballs under their release file names), `install-local.sh`.

## A. From local bottles (before upload)

```bash
scp -r <build-host>:<output>/artifacts/homebrew ~/sea-mlir-bottles
bash ~/sea-mlir-bottles/install-local.sh ~/sea-mlir-bottles
```

`install-local.sh` taps a copy of `Formula/` as `seahorn/seahorn` with
`root_url` set to `file://<this directory>/bottles`, runs `brew trust
seahorn/seahorn` (Homebrew loads formulae of other taps only when they are
trusted), `brew install seahorn/seahorn/seahorn` (and with it
`seahorn/seahorn/sea-mlir`, llvm@18, gmp, mpfr, python@3.15 and their
dependencies from homebrew-core), and fails unless every installed formula
was poured from a bottle. Pass more formulae after the directory, e.g.
`... ~/sea-mlir-bottles seahorn sea-mlir-llvm`; `sea-mlir-llvm` is needed
only to build sea-mlir from source.

## B. From uploaded bottles (the tap on GitHub)

When the tap repository exists with these formulae and each bottle is
attached to the release `<formula>-<version>` of the tap repository (the
`root_url` of each `bottle do` block):

```bash
brew tap seahorn/seahorn
brew trust seahorn/seahorn
brew install seahorn/seahorn/seahorn
```

## Check

```bash
brew test seahorn/seahorn/sea-mlir seahorn/seahorn/seahorn
sea-vc --version                  # z3 5.4.2.0, yices 2.8.0, bitwuzla 0.9.1
sea-vc --agent | head -3          # DOCS: <brew prefix>/Cellar/sea-mlir/<version>/share/doc/sea-mlir
cat > sat.c <<'EOF'
extern unsigned nd(void);
#include "seahorn/seahorn.h"
int main(void) {
  unsigned value = nd();
  sassert(value == 0);
  return 0;
}
EOF
sea yama -y sea-mlir-export -m64 --horn-sea-mlir-out=sat.mlir sat.c
sea-vc sat.mlir --lower           # result: sat, replay: assert-failed, mismatches=0
```

`sea`'s default `-m32` needs 32-bit C headers (`apt-get install
libc6-dev-i386` on Ubuntu); `-m64` does not.

## Remove

```bash
brew uninstall seahorn/seahorn/seahorn seahorn/seahorn/sea-mlir
brew uninstall seahorn/seahorn/sea-mlir-llvm   # if installed
brew untap seahorn/seahorn
brew autoremove
```
