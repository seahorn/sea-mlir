# NKI kernels: verifying data movement

Suppose you change the tiling of a gather kernel. The operation is still
`out[i, :] = input[index[i], :]`, but the implementation now splits that
copy across different DMA transfers. You want to know whether every output
cell still comes from the right input cell—for every valid index vector,
including repeated indices and a final tile that is only partly full.

That is a small enough question to state precisely, yet it reaches into
the part of the kernel you just changed. We tried answering it directly
from the Python source of three
[nki-library kernels](https://github.com/aws-neuron/nki-library/tree/ad2f1b921d8e5949fa3b9a186e5f9130269353dc).
Gather was tractable. A runtime loop in elementwise add was tractable too.
Scatter-add was harder: two tiles can update the same row, and the later
update must keep what the earlier one wrote. Here is what we checked,
how the checks work, and where they stop being practical.

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
payload operations. Its algebra can also hide mistakes: bit-vector
addition is associative,
so regrouping an accumulation preserves its result. Floating-point
addition is not associative; the same regrouping can change its result.
The functional claim is therefore
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

The current proof also assumes that low-level accesses stay within their
allocated objects. The tensor checks do not independently establish that
assumption; [the verification details below](#bounds-and-the-memory-model)
explain the distinction.

The model executes operations synchronously within one program. It does
not verify hardware scheduling, concurrent programs, or the independence
promised by an `affine_range`. Every result also depends on the harness's
interpretation of the supported NKI operations.

## Which parts were easy, and which were hard?

With fixed shapes and direct accesses, following a value through a kernel
is conceptually simple: each transfer names a known region, even if its
contents are unknown. Runtime data makes the problem harder only when it
changes dependencies between operations.

Gather reads rows chosen at runtime, but never changes its input. Each
output cell can be traced back to the selected input cell. Dynamic add
chooses how many tiles to process at runtime, but the tiles still follow
a regular sequence. Both fit this approach well. In the recorded gather
run at `N=64`, `D=16`, `K=32`, verification-condition generation took about
0.8 seconds and solving took 0.065 seconds.

Scatter-add has a different dependency. With a row-tile size of 2, the
index vector `[1, 3, 1, 2]` is legal: each tile has distinct indices, but
both tiles update row 1. The second tile must read the value produced by
the first. The checker must account for all such index relationships.
In exploratory runs at `N=4`, `D=2`, increasing `K` from 2 to 4 to 6 raised
the run time from roughly 0.9 to 6.6 to 71 seconds. Small tensors can
therefore be expensive when their updates depend on runtime indices.
These timings describe recorded experiments, not a limit on future tools.

## Trying a check

The [benchmark repository](https://github.com/seahorn/sea-mlir-bench)
contains the extracted programs. From its root, run:

```console
$ sea-vc nki/gather-hw-N4D3K2-all-good.mlir --lower --seam
...
result: unsat
```

`unsat` means there is no input that violates the assertion under this
job's assumptions. Now run the version with a deliberately shifted source
offset:

```console
$ sea-vc nki/gather-hw-N4D3K2-all-mut-src-shift.mlir --lower --seam
...
result: sat
...
replay: assert-failed
...
validate: checked=137 mismatches=0 dontcare=0
```

`sat` means the checker found a violating input. Replay executes that
input and confirms the failed assertion. The zero mismatch count says
the solver model and the interpreter agreed on the checked values.
The [verification section](#for-verification-developers) shows the
extracted code and the fuller output.

## What the experiments establish for NKI

We found **no new bug in these upstream NKI kernels**. We did check that
planted mistakes fail: a shifted source offset in gather, a missing offset
advance in dynamic add, and a lost previous value in scatter-add.
Dropping scatter's within-tile uniqueness requirement also produces a
counterexample to its documented contract.

The tile boundaries matter. A mutation that always uses the first column
tile is indistinguishable from correct code when there is only one column
tile. The structural sweeps scale tile constants down to exercise multiple
tiles and partial tiles with small tensors. They record which mutations
should fail at each shape, rather than treating every passing mutant as a
failure of the checker.

### Where we would try a CI gate

We recommend trying a small data-movement gate alongside the existing NKI
integration tests, starting with dynamic add or gather. The engineering
case is familiar from checking optimized implementations against reference
code: tiling and address calculations can change while the intended mapping
stays fixed. That stable contract gives the check a life beyond one proof.
A wrong mapping affects the returned tensor even when every access is legal.

The existing tests suggest a concrete place to start. At the pinned
revision, dynamic add's
[test input generator](https://github.com/aws-neuron/nki-library/blob/ad2f1b921d8e5949fa3b9a186e5f9130269353dc/test/integration/nkilib/experimental/dynamic_shapes/test_dynamic_elementwise_add.py)
sets `num_m_tiles` to the full capacity, `M/P`. The symbolic check covers
every legal count at each selected shape: zero, full capacity, and
intermediate stopping points when the shape has more than one row tile. It also checks that an early stop leaves the remaining rows
untouched. This directly exercises the runtime behavior that distinguishes
the kernel from a static elementwise add.

Similarly, gather's
[integration tests](https://github.com/aws-neuron/nki-library/blob/ad2f1b921d8e5949fa3b9a186e5f9130269353dc/test/integration/nkilib/experimental/misc/test_gather.py)
compare against a reference using sampled index vectors. A symbolic gate
checks all legal index vectors for its selected shapes, including repeated
indices and boundary rows. Adding concrete tests for these cases is useful
too. The symbolic check adds exhaustive coverage within its model, without
requiring a developer to enumerate the combinations.

A practical first gate would use a small set of shapes chosen to exercise
one tile, multiple tiles and partial tiles where supported. It would
regenerate the extracted programs when the kernel changes, require the
good cases to pass, and retain planted mistakes as checks on the harness.
Gather and dynamic add are promising because their studied cases simplify
well; scatter needs more work before we would recommend a routine CI gate.
Scaled tile constants exercise implementation structure cheaply, but their
proofs do not establish correctness at every production shape. Keep
hardware-size checks where affordable and the existing integration tests
for actual compiler, device and numerical behavior.

This is our case for spending a modest CI budget on a pilot: a stable
property, concrete runtime choices to cover, and evidence that relevant
mistakes are detectable at manageable sizes. We have not measured the
maintenance cost across NKI revisions or established how often these
kernels regress. The pilot should answer those questions before expanding
coverage. The exported jobs reproduce the experiments today; distributing
the tracing harness and automating regeneration remain necessary to turn
them into a gate on changing Python source.

---

## For verification developers

The rest of this page examines the encoding, solver work and regression
gates for Sea-MLIR itself. These explain why the checks behave as they do;
they are separate from the case for adopting a gate in an NKI project.

### Static alias resolution and residual solver work

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

### Inspecting the extracted program

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

### Solver output and replay

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

### Gather scaling

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

### Bounds and the memory model

These checks coexist with SeaM's reported **A-INB assumption** that
low-level memory accesses remain within their objects. A successful run
under A-INB does not independently discharge that assumption. The API-level
checks state useful bounds properties, but a complete discharge requires
a separate check whose encoding does not rely on the assumption being
proved. Read `seam.inb` alongside the verdict.

### A regression gate for Sea-MLIR

The exported programs are fixed inputs to Sea-MLIR, so they can gate
changes to the verifier without regenerating the Python extraction.
The manifest checks expected verdicts and also requires
`vc.ackermann-constraints: 0` for gather good and precondition-dropped
jobs, and dynamic-add jobs.
For those cases, static alias resolution is an expected capability.

The constraint counter exposed a regression that verdicts alone missed. A
change to SeaM's address comparison left the gather proof passing but
generated 11,280 memory-consistency constraints where there had been none.
The bound now makes that change fail the benchmark gate. See
[Verifier benchmarks and gates](../reference/benchmarks.md).

This is evidence for a gate on the verifier: a demonstrated regression,
a measurable expectation and a rerunnable check. It does not establish
the value of gating changes to the NKI source, which is the separate
engineering question above.
