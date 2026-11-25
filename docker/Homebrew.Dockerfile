# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Homebrew bottles for Linux x86_64: the tap's formulae (packaging/homebrew)
# built from source with `brew install --build-bottle`, tested, and bottled
# in dependency order. docker/release.py --homebrew prepares the build
# context (the formulae, sea-mlir's url pointing at a local `git archive`
# tarball) and runs it; one layer per formula, so a sea-mlir change keeps
# the LLVM layer. Bottles, their JSON and the formulae with the merged
# `bottle do` blocks end in /artifacts/homebrew.
ARG BREW_IMAGE=ghcr.io/homebrew/brew:7.0.16@sha256:af4248b6688e954119fc2f61fbd8a26ea85d309df91ac065913d423c57a04549
# The root URL of the tap's GitHub releases: brew bottle --root-url gets
# <root>/<formula>-<version>, the tag `brew pr-upload` would create. Global:
# each stage redeclares it.
ARG ROOT_URL=https://github.com/seahorn/homebrew-seahorn/releases/download
FROM ${BREW_IMAGE} AS homebrew-base
ARG JOBS=8
ENV HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1 HOMEBREW_NO_ENV_HINTS=1 \
    HOMEBREW_NO_INSTALL_CLEANUP=1 HOMEBREW_MAKE_JOBS=${JOBS} \
    GIT_AUTHOR_NAME=bottle GIT_AUTHOR_EMAIL=bottle@localhost \
    GIT_COMMITTER_NAME=bottle GIT_COMMITTER_EMAIL=bottle@localhost
# the image works in the linuxbrew user's home (WORKDIR, $HOME)
ENV TAP=${HOMEBREW_PREFIX}/Homebrew/Library/Taps/seahorn/homebrew-seahorn
COPY --chown=linuxbrew:linuxbrew docker/homebrew-bottle.sh homebrew-bottle.sh
RUN set -eux; \
    test "$JOBS" -ge 1 && test "$JOBS" -le 8; \
    mkdir -p "$TAP/Formula" "$HOME/artifacts/homebrew/bottles"; \
    git -C "$TAP" init -q; \
    git -C "$TAP" commit -q --allow-empty -m tap; \
    brew trust seahorn/seahorn; \
    brew --version > "$HOME/artifacts/homebrew/brew-version.txt"

FROM homebrew-base AS homebrew-llvm
ARG ROOT_URL
COPY --chown=linuxbrew:linuxbrew tap/Formula/sea-mlir-llvm.rb tap/
RUN bash homebrew-bottle.sh sea-mlir-llvm "$ROOT_URL"

FROM homebrew-llvm AS homebrew-sea-mlir
ARG ROOT_URL
COPY --chown=linuxbrew:linuxbrew src/ src/
COPY --chown=linuxbrew:linuxbrew tap/Formula/sea-mlir.rb tap/
RUN bash homebrew-bottle.sh sea-mlir "$ROOT_URL"

FROM homebrew-sea-mlir AS homebrew-seahorn
ARG ROOT_URL
COPY --chown=linuxbrew:linuxbrew tap/Formula/seahorn.rb tap/
RUN bash homebrew-bottle.sh seahorn "$ROOT_URL"

# the formulae with their bottle blocks; the image's /artifacts is extracted
FROM homebrew-seahorn AS homebrew-bottle
RUN set -eux; \
    cd "$HOME/artifacts/homebrew/bottles"; \
    brew bottle --merge --write --no-commit ./*.bottle.json; \
    mkdir -p ../Formula; \
    cp "$TAP"/Formula/*.rb ../Formula/; \
    git -C "$TAP" diff --stat
USER root
RUN mv "$(getent passwd linuxbrew | cut -d: -f6)/artifacts" /artifacts
USER linuxbrew
