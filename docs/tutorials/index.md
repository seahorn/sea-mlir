# Applications

A kernel changes its tiling, a parser advances through a packet, or two
transaction clusters merge their updates. In each case, the implementation
has many details to get right while the requirement can be small: preserve
the selected values, stay inside the packet, or keep the larger TTL.

These studies show how to turn such requirements into checks on real code.
Start with the system closest to yours. Each tutorial explains the domain
problem and the scope of the check before discussing verification machinery.

| Tutorial | Code under test | Engineering question |
|---|---|---|
| [NKI kernels](nki.md) | `nki-library` gather, dynamic add and scatter-add | Do tiled transfers and runtime updates preserve the intended tensor values? |
| [QUIC fragment reassembly](inductive-proof.md) | Firedancer's fragment reassembly | Can a fragment sequence cause a copy outside its buffer? |
| [Stellar: lifetime extensions](stellar-ttl.md) | stellar-core's TTL merge, ported to C | Can committing one cluster lose another cluster's TTL extension? |
| [Stellar: queue accounting](stellar-txqueue.md) | stellar-core's C++ queue methods with modeled surroundings | Do fee reservations follow the right payer through replacement and eviction? |
| [HPACK decoding](firedancer.md) | nghttp2's Huffman decoder in Firedancer | Does the decoder respect its input and output boundaries? |
| [Transaction parsing](txn-parse.md) | Firedancer's `fd_txn_parse_core` | Do parsing steps preserve the bounds needed for subsequent reads? |
| [HTTP/2 flow control](h2-flow-control.md) | Firedancer's h2 client | Do window updates and data transmission preserve the protocol's accounting? |

## Reproducing the experiments

The runnable examples use exported programs from the benchmark repository
[seahorn/sea-mlir-bench](https://github.com/seahorn/sea-mlir-bench),
under Apache-2.0 and the licenses of the code under test. Each family has
its exported `.mlir` jobs, a `manifest.yaml` with the expected verdict and
tier of each job, and a `PROVENANCE.md` with the source commits and the
export commands. The C families also have their harness sources under
`harness/`. The queue-accounting story describes a separate source-extraction
study, with code examples and recorded results. Its harness and generated
programs are not distributed with the tutorial.

A benchmark job runs with its family's default arguments plus its own.
For Firedancer and Stellar TTL benchmark jobs, the defaults are
`--lower --seam --widen-memory=4`; for NKI they are `--lower --seam`.
The benchmark commands run from the root of a benchmark checkout:

```bash
sea-vc firedancer/hpack-huff-proof.mlir --lower --seam --widen-memory=4
```

The benchmark jobs and transcripts use commit `f14be40`. The queue study
records its own toolchain snapshot on its page. Times shown are from single
runs and vary between machines and runs. Transcripts illustrate the recorded
snapshots; replay counts and source locations can differ with the build
and solver model.

## How a job is made

A harness is a C or C++ program that calls the code under test on
nondeterministic inputs, constrains the inputs with `assume`, and states
the claims with `sassert`. clang compiles it to LLVM bitcode, and SeaHorn
exports the `sea` program. A typical export uses an unroll bound of 8:

```bash
sea yama -y sea-mlir-export --bound=8 --horn-sea-mlir-out=job.mlir h.bc
```

Some harnesses replace loops with explicit invariant obligations; the
unroll bound alone does not describe their proof scope. Each family's
`PROVENANCE.md` in the benchmark records the source
commits, compiler flags and export settings of its jobs. The NKI jobs are
made differently: a Python harness traces the kernels into the `sea`
dialect directly. The pages describe each harness in prose; the exported
`.mlir` file is what `sea-vc` checks.

## Firedancer source and extraction

The jobs use Firedancer commit `37c658197`. Their C harnesses and extracted
programs are in the [benchmark repository](https://github.com/seahorn/sea-mlir-bench/tree/main/firedancer).
An agent writes the contract, chooses the environment assumptions and
constructs deliberate regressions. Clang and SeaHorn compile the harness
and selected source into Sea-MLIR; Sea-MLIR checks the resulting program
and replays violating inputs. The export settings and source transformations
are recorded in the family's
[provenance](https://github.com/seahorn/sea-mlir-bench/blob/main/firedancer/PROVENANCE.md).

For gates on changed application code, follow the
[regeneration and controls workflow](../agents/index.md#5-regenerate-and-check-in-ci).
Fixed exports check the verifier against the same input.

## Three kinds of job

- **proof** (expected `unsat`): the claim holds, under the assumptions the
  run reports.
- **reach** (expected `sat`): a vacuity check. A proof means little if its
  assumptions exclude the interesting executions. A reach job asserts the
  negation of one condition. A replayed counterexample whose `assert-at`
  is the reach assertion shows that the condition can occur; a
  counterexample that stops at another check says nothing about it.
- **mutant** (expected `sat`, unless the mutant is equivalent at the
  bound): the code with a planted bug, or with a historical fix reverted.
  A proof that misses its mutants proves too little.

Every proof on these pages has mutants. The C proofs also have reach
jobs; the NKI proofs have jobs with the precondition dropped instead.
[Proof controls](../guide/proof-controls.md) describes these jobs, and the
vacuity twins that check that the end of a proof is reachable at all.
