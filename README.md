<p align="center">
  <img src="docs/assets/banner.svg" alt="sea-mlir: assembly for verification" width="640">
</p>

<p align="center">
  <b>A verification toolkit for low-level code, built for AI agents as well as people.</b><br>
  Bit-precise programs in a small MLIR dialect · SMT-backed proofs · every counterexample replayed
</p>

---

sea-mlir checks assertions in programs written in the `sea` dialect: control
flow, bit-vector arithmetic and byte-addressed memory. It answers with a
proof (`unsat`) or with a concrete counterexample (`sat`) that its
interpreter replays and validates before reporting it. Programs come from C
and C++ through the SeaHorn front end, or are written directly in the
dialect.

## 🌊 Assembly for verification

Automated verification often stalls on the encoding, not on the program.
When a proof is slow or fails, someone has to find what makes it hard and
change how it is posed: the memory model, the decomposition, the
assumptions, the solver settings. Source code is too far from the formula,
and SMT has lost the program's structure.

sea-mlir sits between the two. Its programs keep control flow, arithmetic
and memory operations, stay close to the source in size, and stay close to
the solver encoding. Every encoding choice is an explicit option, so an
agent (or a person) can change one, rerun, and compare.

## 🐠 A first look

```mlir
sea.program @main {
  %x = sea.nondet "nondet_u32" [0] : i32
  %c = sea.const 10 : i32
  %lt = sea.icmp ult, %x, %c : i32
  sea.assume %lt
  %y = sea.add %x, %x : i32
  %c18 = sea.const 18 : i32
  %ok = sea.icmp ult, %y, %c18 : i32
  sea.assert %ok
  sea.halt
}
```

```console
$ sea-vc t.mlir | grep -E '^(result|nondet|replay|validate)'
result: sat
nondet: site=0 callee=nondet_u32 width=32 value=9
replay: assert-failed
replay: assert-at: t.mlir:8:9
replay: assert-op: t.mlir:9:3
validate: checked=6 mismatches=0 dontcare=0
```

The solver found `x = 9`; the interpreter ran the program on it, the
assertion failed at line 8, and every value the solver chose matched the
run. With the bound `20` instead of `18` the answer is `result: unsat`.

## 🧰 What is in the box

| tool | what it does |
|---|---|
| `sea-vc` | builds the verification condition, solves it (z3, Yices, Bitwuzla), replays counterexamples |
| `sea-run` | the interpreter: the executable semantics of the dialect |
| `sea-opt` | the passes: lowering, placement of objects, memory widening, … |
| `sea-reduce` | shrinks a program while a symptom persists |
| SeaM | the object-aware memory rewriter behind `sea-vc --seam` |
| SeaHorn exporter | C/C++ → LLVM → `sea` dialect (in SeaHorn, `--horn-bmc-engine=sea-mlir`) |

## 🤖 Built for agents

- every tool has an `--agent` guide, and `--format=jsonl` output;
- exit codes are stable and documented;
- every `sat` is replayed by the interpreter before it is reported;
- statistics name the operations and rules responsible for the solver's work;
- every result states the assumptions it depends on;
- the configuration an agent arrives at is saved as a profile and rerun in CI.

## 🦀 Why trust the results

sea-mlir checks itself in several independent ways:

- 🔁 **replay:** every counterexample runs in the interpreter and is checked
  value by value;
- 🔍 **rewrite checking:** `--seam-check` checks each memory rewrite with
  the solver;
- ⚖️ **differential runs** against SeaHorn, and benchmark gates with
  expected verdicts and counter bounds;
- 🧪 **fuzzing:** random programs and mutants against the interpreter;
- 📐 **Lean:** machine-checked proofs of the foundations of the rewriting
  rules, and an independent review of how they match the code.

Results are conditional on named assumptions, such as A-INB (every access
stays in its object), and the output reports which ones a result used.
See [Why trust it](docs/trust/index.md) and
[Known limitations](docs/reference/limitations.md).

## ⚓ Build

sea-mlir builds out of tree against an installed LLVM/MLIR (the commit in
`LLVM_REVISION`), with z3; Bitwuzla and Yices 2 are optional.

```bash
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DMLIR_DIR=<llvm-install>/lib/cmake/mlir -DZ3_ROOT=<z3-install>
ninja -C build check-sea
```

Details, dependencies, installation and the release gates:
[Build and install](docs/guide/build.md) and [Release notes](docs/Release.md).

## 🗺️ Documentation

- ⛵ [Get started](docs/guide/getting-started.md): a first proof, reading the
  output, counterexamples, proof controls
- 🐟 [Applications](docs/tutorials/index.md): NKI kernels, an inductive
  proof, Stellar, Firedancer HPACK, transaction parsing, HTTP/2 flow control
- 🐚 [Reference](docs/reference/index.md): the dialect, the command line,
  output formats, profiles, SeaM
- 🐙 [For agents](docs/agents/index.md): running the tools, gates, rules

The documentation is also a [MkDocs](https://www.mkdocs.org/) site:
`just docs-serve` previews it locally.

## 🛠️ Development

- [`CLAUDE.md`](CLAUDE.md): the development build, the code layout, the
  semantic decisions and the rules for contributors (people and agents)
- [`docs/design/`](docs/design/): design notes (SeaM, memory, pointers,
  prints, …)
- [`docs/reference/dialect.md`](docs/reference/dialect.md): the `sea`
  dialect, generated from `include/sea/Dialect/Sea/*.td`
- [`test/README.md`](test/README.md): the tests and the protected oracles
- [`docs/Benchmarks.md`](docs/Benchmarks.md): benchmark suites and gates
  (`just bench-connect`, `just bench-gate`)
- `just --list`: the development recipes (docs, benchmarks, the release
  gate)

## 🐢 Status

Version 0.1, a research prototype. The dialect has no text-format stability
guarantee between versions. The default options are conservative, not
tuned; see [Release notes](docs/Release.md) for the supported
configuration and the known issues.

## 🐬 Built with AI agents

sea-mlir was developed with AI coding agents: **Claude** (Claude Code, by
Anthropic) and **Codex** (by OpenAI) wrote most of the code, tests,
documentation and Lean proofs, and reviewed one another's work, under the
direction of [Arie Gurfinkel](https://ece.uwaterloo.ca/~agurfink/)
(University of Waterloo), who designed sea-mlir and leads the project.

## License

Apache License v2.0 with LLVM Exceptions
(SPDX `Apache-2.0 WITH LLVM-exception`), as LLVM and MLIR. See
[`LICENSE`](LICENSE) and [`AUTHORS`](AUTHORS).
