# Contributing

Start with the [build instructions](README.md#-build) and
[contributor rules](CLAUDE.md). Use the LLVM revision in `LLVM_REVISION`
and an out-of-tree build. Run `ninja -C <build> check-sea` and `just lint`
before submitting a change. The [test rules](test/README.md) identify
protected semantic oracles; do not weaken them to make a test pass.

Use [Conventional Commits](https://www.conventionalcommits.org/), with a
header of at most 72 characters. Keep changes focused and explain the
behavior and validation in the pull request.

On a clean committed branch, run `python3 utils/attest.py` before requesting
review. It runs the required gates and posts their commit statuses; see
[the gate instructions](docs/agents/gates.md) for configuration and results.
The main branch accepts pull requests through merge commits only.

Report security vulnerabilities privately as described in
[SECURITY.md](SECURITY.md).

<!-- SPDX-FileCopyrightText: 2026 The sea-mlir Authors -->
<!-- SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception -->
