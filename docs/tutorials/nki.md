# NKI kernels

AWS Neuron kernels, written in NKI, move tensors between memories with
explicit tile offsets, strides and DMA transfers. An indexing error reads
the wrong row, or reads outside a tensor. This page checks the gather
kernel of [nki-library](https://github.com/aws-neuron/nki-library)
(commit `ad2f1b9`): for every index vector that satisfies the kernel's
precondition, every DMA stays inside its tensor, and the output holds
exactly the selected rows.

## The question

The kernel computes `out[i, :] = input[index[i], :]` for an input of $N$
rows and $D$ columns and $K$ indices. If every index lies in $[0, N)$, does
every transfer stay inside the input, and does every output element equal
the input element it selects?

## The harness

There is no C here and no SeaHorn. The harness is a stand-in `nki`
Python package that loads the kernel source unchanged and traces it into a
`sea.program`. Shapes and tile constants are concrete, because they drive
Python control flow. Tensor contents and indices are symbolic: they are
read from one nondeterministic initial memory. Kernel mutants are text
substitutions in the kernel source. The tracing harness is not part of
the benchmark; the traced `.mlir` programs are.

The traced program for $N = 4$, $D = 3$, $K = 2$ starts with the
precondition. Each index is read from the initial memory `mem0` and
assumed to lie in $[0, 4)$:

```mlir
  %v1 = sea.mem.nondet "region.0" [0] : !m loc("mem0")
  %v4 = sea.alloca %v3 align 8 [1] : i64 -> !sea.ptr<64> loc("input")
  %v6 = sea.alloca %v5 align 8 [2] : i64 -> !sea.ptr<64> loc("index")
  %v56 = sea.ptr.add %v6, %v10 : !sea.ptr<64>, i64
  %v57 = sea.load %v1[%v56] align 4 : !m -> i32 loc("pre.index0")
  %v58 = sea.sext %v57 : i32 to i64
  %v59 = sea.icmp sge, %v58, %v10 : i64
  %v60 = sea.icmp slt, %v58, %v13 : i64
  %v61 = sea.and %v59, %v60 : i1
  sea.assume %v61
```

The kernel's DMA transfers become `sea.memmove` operations between
objects, with the transfer's name as location:

```mlir
  %v35 = sea.memmove %v17[%v19], %v17[%v34], %v23 inplace : !m loc("dma.input->sbuf.t2.row0")
```

Before each transfer from the input, the program checks that the source
row lies inside the input. At the end, it compares every output element
(`spec.actual`) with the input element its index selects
(`spec.expect`). The assertion is the conjunction of all these checks.

## Running it

```console
$ sea-vc nki/gather-hw-N4D3K2-all-good.mlir --lower --seam
tool.version: sea-mlir 0.1.0 (...)
...
program: @nki_gather
...
vc.ackermann-constraints: 0
seam.inb: assumed accesses=40 conditions=8
solve-time: 0.024
result: unsat
```

The mutant `src-shift` adds 1 to the source offset of each row copy:

```console
$ sea-vc nki/gather-hw-N4D3K2-all-mut-src-shift.mlir --lower --seam
...
result: sat
...
replay: assert-failed
replay: assert-at: nki/gather-hw-N4D3K2-all-mut-src-shift.mlir:132:11
replay: assert-op: nki/gather-hw-N4D3K2-all-mut-src-shift.mlir:169:3
validate: checked=137 mismatches=0 dontcare=0
```

Without C debug information, `assert-at` points into the MLIR file. The
assertion is a chain of `sea.and`, and `assert-at` is its first false
conjunct: on line 132, an output element differs from the input element
it should hold.
Dropping the precondition (`gather-hw-N4D3K2-all-nopre`) also gives a
replayed counterexample. Three other mutants (`idx-k0`, `out-col`,
`src-col`) are equivalent to the kernel at this small shape; the harness
expects `unsat` for them, and that is the answer.

## Scaling

At the hardware tile sizes, with $N = 64$, $D = 16$, $K = 32$:

```console
$ sea-vc nki/gather-hw-N64D16K32-all-good.mlir --lower --seam
...
vc-time: 0.800
...
vc.ackermann-constraints: 0
seam.inb: assumed accesses=1472 conditions=544
solve-time: 0.065
result: unsat
```

SeaM resolves every load of the gather before the solver runs: the query
has no memory constraints left (`vc.ackermann-constraints: 0`). The same
program without SeaM does not finish within a two-minute solver limit:

```console
$ sea-vc nki/gather-hw-N64D16K32-all-good.mlir --lower --timeout=120 --mem-limit=8
...
vc-time: 0.181
...
solve-time: 120.099
result: unknown
solve-timeout: true
```

In the benchmark manifest, the $N = 256$ proof is `unsat` in the `full`
tier (134 s when the expectations were established). The $N = 512$ proof
runs out of the 16 GB memory cap and is tracked in the `hard` tier.

## What the proof rests on

- The in-bounds assumption for the 1472 accesses that the run reports.
  The program's own checks cover the DMA source rows.
- The harness's model of the NKI operations that the kernel uses. The
  proof is about the traced program, not about the hardware.
- The concrete shapes. Each shape is a separate proof.

## A regression that changed no verdict

The gather proofs have a counter bound in the benchmark manifest:
`vc.ackermann-constraints` must be 0. The bound exists because of a
regression. A change to SeaM's address comparison made it miss some equal
addresses. Every verdict stayed the same, but this proof left 11,280
memory constraints to the solver instead of 0. The bound makes the
benchmark gate fail on such a change ([Benchmarks and gates](../reference/benchmarks.md)).
