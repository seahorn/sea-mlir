# NKI kernels: verifying data movement

A tensor kernel can stay within every buffer and still return the wrong
answer: a tile offset can select the wrong rows, or a loop can overwrite
one tile and leave another untouched. These errors are easy to miss with
example inputs, especially when the indices and loop counts come from
runtime data.

This case study checks three kernels from AWS's
[nki-library](https://github.com/aws-neuron/nki-library/tree/ad2f1b921d8e5949fa3b9a186e5f9130269353dc):
gather, dynamic elementwise add, and scatter-add. An agent-built Python
harness extracts their memory operations into Sea-MLIR and states both
bounds and functional properties. Sea-MLIR checks those properties for
symbolic inputs at concrete shapes. The results give us repeatable gates
for data movement and a useful distinction: some dynamic kernels simplify
almost completely, while others leave substantial alias reasoning to SMT.

## What an NKI kernel does

The Neuron Kernel Interface (NKI) is a Python-embedded language for kernels
on AWS Trainium and Inferentia accelerators. A kernel explicitly moves
tensor tiles between device memory (HBM) and on-chip scratchpad memory
(SBUF), then operates on those tiles. Its access patterns specify offsets,
strides and element counts. DMA instructions perform the transfers.

Python describes much of the kernel's structure: tensor shapes, tile
sizes and statically enumerable loops. Other decisions depend on device
data. A gather reads its row indices from a tensor; a dynamic loop reads
its trip count at runtime. Verification must preserve those choices rather
than trace one particular input.

## The three kernels and their claims

All three come from nki-library commit `ad2f1b9`. The source links below
pin that revision. Each job fixes the tensor shapes and tile constants;
the tensor contents remain symbolic.

| Kernel | Runtime choices | Claim under the input contract |
|---|---|---|
| [gather](https://github.com/aws-neuron/nki-library/blob/ad2f1b921d8e5949fa3b9a186e5f9130269353dc/src/nkilib_src/nkilib/experimental/misc/gather.py) | An index vector selects input rows. | For input shape `N × D` and `K` indices in `[0, N)`, every output element equals `input[index[i], j]`, and modeled transfers stay within their tensors. |
| [dynamic_elementwise_add](https://github.com/aws-neuron/nki-library/blob/ad2f1b921d8e5949fa3b9a186e5f9130269353dc/src/nkilib_src/nkilib/experimental/dynamic_shapes/dynamic_elementwise_add.py) | A runtime count `n` controls how many row tiles execute. An offset in SBUF advances after each iteration. | With tile height `P` and `0 ≤ n ≤ M/P`, the first `n*P` rows contain the elementwise combination of the inputs. Remaining rows retain their initial contents, and modeled accesses stay in bounds. |
| [scatter_add](https://github.com/aws-neuron/nki-library/blob/ad2f1b921d8e5949fa3b9a186e5f9130269353dc/src/nkilib_src/nkilib/experimental/misc/scatter_add.py) | Indices select rows to read, update and write back. Later tiles can update the same rows. | On the `dim=0`, `unique_indices=True` path, each output cell contains its initial value plus the contributions addressed to it, using the payload model below. Indices must be in range and distinct within each row tile. |

Scatter permits the same index in *different* tiles: tiles run
sequentially, so later updates must preserve earlier contributions.
The alternative `unique_indices=False` implementation is outside this
study. For dynamic add, untouched output rows are unconstrained initial
memory, not implicitly zero.

### What happens to floating-point operations?

Gather copies bits, so its functional claim does not depend on
floating-point arithmetic. The other two kernels combine payload values.
The checked-in harness replaces `nl.add` on payloads with **bit-vector
addition on the element bits**, and uses the same operation in the spec.
It does not encode IEEE floating-point addition. Integer operations used
to compute indices and offsets retain their bit-vector semantics.

This surrogate lets us check which values a kernel reads, combines and
writes without introducing floating-point reasoning. It can expose a
missing contribution or an update to the wrong cell. It does not establish
numerical correctness, rounding behavior or equivalence for arbitrary
payload operations. Its algebra can also hide mistakes: swapping operands
of a commutative addition will pass. The functional claim is therefore
about the kernel under this explicit payload model.

## From Python to Sea-MLIR

The agent supplies a stand-in `nki` Python package and loads the kernel
source through it. Shapes and tile constants are concrete, so ordinary
Python evaluates the structural parts of the kernel. The stand-in package
records supported NKI operations with symbolic operands and emits a
`sea.program` directly. There is no C translation or SeaHorn frontend in
this route.

The extraction has four parts:

1. Allocate separate tensor objects backed by one nondeterministic initial
   memory. Inputs, index tensors and fresh output contents are symbolic.
2. Enumerate static loops. For a runtime `fori_loop`, emit guarded
   iterations up to the harness bound and a check that the trip count
   does not exceed it. Dynamic add uses the concrete capacity `M/P`.
3. Translate DMA copies to `sea.memmove`, scalar accesses to loads and
   stores, and supported computations to bit-vector operations. Reject
   unsupported NKI operations instead of silently skipping them.
4. Add input assumptions and an independently written output spec. Read
   the initial values before the kernel executes, then compare them with
   the final output. Include the modeled bounds checks in the assertion.

The source is loaded unchanged for the ordinary jobs. Named mutants
change specific source expressions. Structural sweeps also use explicitly
scaled tile constants, so a small tensor can exercise multiple tiles and
partial tiles. The exported family records these distinctions in
[`nki/PROVENANCE.md`](https://github.com/seahorn/sea-mlir-bench/blob/main/nki/PROVENANCE.md).
The Python tracing harness is not included in that benchmark repository;
the generated `.mlir` jobs are.

The **agent** chooses the scope, implements the NKI operations used by
that scope, writes the spec and constructs controls. **Sea-MLIR** lowers
the resulting program, simplifies its memory dependencies with SeaM,
asks a solver for a violating input, and replays any counterexample in its
interpreter. Replay checks agreement between the VC and interpreter; it
does not validate the harness against the Neuron compiler or hardware.

### Bounds checks and assumptions

The harness rejects invalid direct accesses while tracing. For runtime
DMA offsets, it emits checks that the accessed row lies within its tensor.
A failed check makes the final assertion fail, as does a wrong output
value or an excessive runtime trip count.

These checks coexist with SeaM's reported **A-INB assumption** that
low-level memory accesses remain within their objects. A successful run
under A-INB does not independently discharge that assumption. The API-level
checks state useful bounds properties, but a complete discharge requires
a separate check whose encoding does not rely on the assumption being
proved. Read `seam.inb` alongside the verdict.

The model executes operations synchronously within one program. It does
not verify hardware scheduling, concurrent programs, or the independence
promised by an `affine_range`. Every result also depends on the harness's
interpretation of the supported NKI operations.

## Static structure versus dynamic difficulty

A fixed shape with direct accesses should be an easy verification case,
even when every tensor element is symbolic. Symbolic execution can follow
each write to the reads that use it. SeaM should resolve that aliasing
statically; leaving those questions to SMT would mean missing elementary
simplification. SMT may still be needed for arithmetic or the final
property, but not to rediscover the memory dependencies.

The three kernels show why “dynamic” alone does not predict difficulty:

- **Gather has symbolic source indices but direct output locations.**
  Its input is never overwritten. When the spec checks each output
  coordinate, the implementation and spec reduce to the same read of the
  initial input, even though that read's address contains a symbolic index.
  The good jobs need no residual alias constraints.
- **Dynamic add has a symbolic trip count but regular accesses.**
  Guarded unrolling preserves which iterations execute. The advancing
  offset and the spec still describe the same sequence of tiles. SeaM
  resolves the memory dependencies; the benchmark requires zero Ackermann
  constraints for this family too.
- **Scatter reads and writes through symbolic indices.**
  A later tile may read a row updated by an earlier tile. The proof must
  relate those updates to the spec's accumulation for each concrete output
  row. The studied encoding leaves alias and read-consistency questions
  for SMT, and its cost grows sharply with the number of tiles.

For example, an exploratory scatter run with `N=4`, `D=2` and a scaled
tile height of 2 left 436, 2,128 and 5,940 Ackermann constraints for
`K=2`, `4` and `6`, respectively. Run times grew from roughly 0.9 to 6.6
to 71 seconds. These are historical observations, not current performance
guarantees; they locate the difficulty in a tiny program with symbolic
updates. Some residual constraints were also avoidable address-arithmetic
work, so this is a target for better simplification as well as solving.

## Walk through a gather job

Run the following commands from the benchmark checkout, as described in
[Applications](index.md). The output and timings use that page's recorded
benchmark snapshot.

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

## Gather scaling

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

SeaM resolves the memory dependencies before the solver runs. No
Ackermann constraints remain to relate reads at potentially equal
addresses (`vc.ackermann-constraints: 0`); the solver still checks the
remaining side conditions. The same program without SeaM does not finish
within a two-minute solver limit:

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

## What did we find, and what can we gate?

This study did **not establish a new defect in the upstream NKI kernels**.
Counterexamples came from deliberately changed kernels or violated input
contracts. For scatter, dropping within-tile uniqueness exposes the
kernel's documented duplicate-index hazard; it is not a new kernel bug.

Those controls show what the checks can detect. Gather's shifted-source
mutant copies the wrong elements. Dynamic add's `no-advance` mutant keeps
writing the first row tile. Scatter's `drop-existing` mutant loses the
previous value. Other mutants need multiple tiles before they differ from
the original kernel. A one-tile proof cannot validate a check for a
cross-tile error, so the sweep includes scaled configurations that exercise
those boundaries and records equivalent mutants as such.

A **gate** packages a claim so that it can be checked again: source and
extraction provenance, an explicit contract, a harness, expected verdicts,
and controls that demonstrate sensitivity to relevant mistakes. Finding a
bug is one possible outcome. Establishing a conditional verification
result and preserving it through changes is another.

This study supplies two useful kinds of gate:

- **Behavior gates:** under the recorded model and input contract, the
  good kernel passes and the relevant mutants fail with replayed
  counterexamples. These target tile-offset mistakes, missed updates,
  incorrect accumulation and modeled bounds violations.
- **Simplification gates:** the gather good jobs and dynamic-add jobs
  require `vc.ackermann-constraints: 0`. For these jobs, static alias
  resolution is an expected capability, not merely a speed bonus.

The second kind already caught a SeaM regression. A change to address
comparison left the gather verdict unchanged but generated 11,280
memory-consistency constraints where there had been none. A verdict-only
gate would have missed it; the counter bound fails the benchmark on that
change. See [Benchmarks and gates](../reference/benchmarks.md).

The exported `.mlir` suite gates changes to **Sea-MLIR**. To gate changes
to an **NKI kernel**, regenerate the jobs from the changed source, review
changes to the extraction or contract, and rerun the controls. Keeping an
old exported job green says nothing about new Python code.

For an NKI developer, the immediate use is a focused regression check
around a data-movement refactor: changed tile sizes, partial-tile handling,
runtime loop advancement or indexed updates. The remaining practical
questions are which production shapes and API operations matter, whether
the modeled contracts match callers, and how much of the real execution
semantics the gate must cover. The current results establish useful
checks for these three slices, with clear boundaries for extending them.
