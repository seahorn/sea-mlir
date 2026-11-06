# Applications

These pages walk through proofs of real code: AWS Neuron NKI kernels, the
Firedancer Solana validator, and stellar-core. Each page states the
question, names the exported jobs, runs `sea-vc`, reads the verdict and
the counterexample, and lists what the result rests on.

| page | code under test | shows |
|---|---|---|
| [NKI kernels](nki.md) | the `nki-library` gather kernel | SeaM resolving all data movement before the solver |
| [Inductive proof: FD-FRAG](inductive-proof.md) | Firedancer's QUIC fragment reassembly | why a bounded run failed, and a one-step inductive proof |
| [Stellar](stellar.md) | stellar-core's TTL merge, ported to C | a historical bug, and a counterexample that is not a bug of the system |
| [Firedancer: HPACK](firedancer.md) | nghttp2's Huffman decoder in Firedancer | a loop cut, a mutant, the JSON view of a counterexample |
| [Firedancer: transaction parsing](txn-parse.md) | `fd_txn_parse_core` | loop invariants, and a size constant the proof explained |
| [Firedancer: HTTP/2 flow control](h2-flow-control.md) | Firedancer's h2 client | ghost state from an RFC, and reach jobs against vacuity |

## The jobs

All programs are in the benchmark repository
[seahorn/sea-mlir-bench](https://github.com/seahorn/sea-mlir-bench),
under Apache-2.0 and the licenses of the code under test. Each family has
its exported `.mlir` jobs, a `manifest.yaml` with the expected verdict and
tier of each job, and a `PROVENANCE.md` with the source commits and the
export commands. The C families also have their harness sources under
`harness/`.

A job runs with its family's default arguments plus its own. For
Firedancer and Stellar the defaults are `--lower --seam --widen-memory=4`;
for NKI they are `--lower --seam`. The commands on these pages run from
the root of a benchmark checkout:

```bash
sea-vc firedancer/hpack-huff-proof.mlir --lower --seam --widen-memory=4
```

The jobs and the sample output on these pages are from the benchmark at
commit `f14be40`. The times shown are from single runs and vary between
machines and runs.

## How a job is made

A harness is a C program that calls the code under test on
nondeterministic inputs, constrains the inputs with `assume`, and states
the claims with `sassert`. clang compiles it to LLVM bitcode, and SeaHorn
exports the `sea` program, with loops unrolled to the bound 8:

```bash
sea yama -y sea-mlir-export --bound=8 --horn-sea-mlir-out=job.mlir h.bc
```

Each family's `PROVENANCE.md` in the benchmark records the source
commits, compiler flags and export settings of its jobs. The NKI jobs are
made differently: a Python harness traces the kernels into the `sea`
dialect directly. The pages describe each harness in prose; the exported
`.mlir` file is what `sea-vc` checks.

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
