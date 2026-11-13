# NKI kernels: checking core-barrier synchronization

The [data-movement study](nki.md) checks kernels that run as one program.
Some NKI kernels run as two. A die has two physical cores that share HBM,
and a kernel can drive them as two concurrent virtual cores that synchronize
only through `nisa.core_barrier`. If a barrier is missing, two cores can
access the same HBM cell with nothing ordering the accesses, and the result
depends on timing.

The question for the author is whether every pair of conflicting cross-core
accesses to shared memory is separated by a barrier. This page shows how to
check that on the kernel source, what the check found on one nki-library
kernel, and where the check stops. The data-movement page states that its
model does not cover concurrent programs; this page covers that case.

## What a two-core NKI kernel does

Both cores run the same Python kernel. Each reads its index from
`nl.program_id` and the core count from `nl.num_programs`, and shards the
work: a core writes its own slice of an output and leaves the rest to its
peer. `nisa.core_barrier(tensor, (0, 1))` makes both cores wait until each
has reached it. Accesses before the barrier are ordered before accesses after
it. Intended cross-core transfers use `nisa.sendrecv`.

A **data race** is two accesses to the same HBM cell by different cores, at
least one of them a write, with no barrier between them.

## The property and its cost

Race-freedom depends on which cells each core accesses and where the barriers
are. It does not depend on the values the kernel computes. The check can
therefore replace matrix multiplications, activations and reductions with
their memory accesses and reason only about addresses and barriers. The
value reasoning that makes the [scatter-add
proofs](nki.md#which-parts-were-easy-and-which-were-hard) expensive does not
arise.

### The epoch monitor

The barrier is the only ordering primitive. For each tracked tensor and each
core, the monitor counts the barriers on that tensor that the core has
executed; that count is the core's current **epoch** for the tensor. Two
accesses to a tensor by different cores are unordered exactly when both
cores are in the same epoch for it. Race-freedom becomes a disjointness
condition per epoch:

> In every epoch, the cells one core writes are disjoint from the cells the
> other core accesses.

The monitor records, per core and epoch, the cells accessed and whether each
access writes. After tracing, it asserts the cross-core disjointness for every
epoch. Program end counts as a final barrier. A violation names the epoch and
the conflicting accesses.

Epoch k of one core lines up with epoch k of the other only if both cores
execute the same barriers. The tracer checks this: it fails if the two cores
end with different barrier counts on a tracked tensor, and it rejects a
`core_barrier` inside a runtime-guarded block, where the trace-time count
would not match execution.

## From Python to Sea-MLIR

The extraction extends the single-core [tracing
route](nki.md#from-python-to-sea-mlir):

1. `nl.program_id` returns the core being traced and `nl.num_programs`
   returns 2. The kernel is traced once per core into one `sea.program`.
   The inputs are one nondeterministic memory, so both traces read the same
   symbolic values, and the rows a gather selects are comparable across cores.
2. `nisa.core_barrier(tensor, cores)` advances that tensor's epoch for the
   current core.
3. Each access to a tracked shared-HBM tensor is recorded with its core,
   epoch, read or write, and the start offset of each row it touches. That
   offset is symbolic when a gather or scatter selects the row.
4. After both cores are traced, the monitor emits the disjointness assertions
   and the program asserts their conjunction.

The monitor compares row-start offsets, which identifies a conflict exactly
when every tracked access covers whole rows. The tracer checks this and
rejects a tracked access that covers part of a row.

The **agent** chooses the scope, implements the NKI operations it uses, and
writes the controls. **Sea-MLIR** lowers the program, simplifies memory with
SeaM, asks a solver for a violating input, and replays any counterexample.
Only the functions that access the tracked tensor need to be traced
unchanged. Computation between a load and a store can be stubbed with fresh
symbolic tiles, since it does not change which cells are accessed.

## Two back-ends

The traced program is input to both of Sea-MLIR's back-ends:

- `sea-vc --lower --seam` decides the property. `unsat` means race-free under
  the run's assumptions; `sat` means a race, with a replayed witness.
- [`sea-run --fuzz`](../reference/fuzzing.md) runs the same program on
  generated inputs.

Write an input bound as one unsigned comparison, `assume(x < N)` (`sea.icmp
ult`). `sea-run --fuzz` reads a range off that form and draws the input from
`[0, N-1]`, biased to the ends ([Ranges](../reference/fuzzing.md#ranges)), and
`sea-vc` handles it as a plain assumption. A signed pair `x >= 0 && x < N`
gives the solver the same constraint, but the fuzzer does not read a range off
it and draws from the full 64-bit range, so most trials fail the assumption.

## Where the results come from

The results below come from three different programs. None of them is a
race-freedom proof of the nki-library kernel.

| Program | What it models | Results |
|---|---|---|
| Micro kernel | Two cores, four shared cells: a sharded initialization, a producer that writes a cell, a barrier, a consumer that reads a cell | proof `unsat`; barrier-deletion mutant `sat`; vacuity twin `sat`; `sea-run --fuzz=200`: no failures on the proof, 14 to 24 failing trials of 200 on the mutant (seeds 1 to 5) |
| Blockwise stand-in | The kernel's initialize-then-accumulate structure, with a parity partition in place of its token sharding | sharded initialization `unsat`, from 0.05 s at 8 rows by 4 blocks to 0.34 s at 256 rows by 16 blocks |
| Kernel trace | The nki-library kernel's functions that access `output`, traced unchanged, computation stubbed | every configuration `sat`; single-core trace `unsat` |

## Controls

The [three kinds of job](index.md#three-kinds-of-job) apply, plus one check of
the monitor itself:

- **proof** (`unsat`): race-free under the stated assumptions.
- **mutant** (`sat`): one `core_barrier` deleted. The epochs on either side
  merge, the accesses the barrier separated become unordered, and the check
  reports the race. A forgotten barrier is the regression this guards against,
  so every race proof should carry this mutant.
- **reach** (`sat`): the vacuity twin, which asserts false at the end of the
  program. `sat` shows the end is reachable under the assumptions.
- **monitor wiring** (`unsat`): the same trace with one core only. With no
  cross-core pairs the monitor asserts nothing, so the result must be `unsat`.
  A `sat` here would mean the monitor is wrong.

## The blockwise MoE kernel

We applied the tracer to `blockwise_mm_baseline_shard_intermediate` in
nki-library's
[`bwmm_shard_on_I.py`](https://github.com/aws-neuron/nki-library/blob/ad2f1b921d8e5949fa3b9a186e5f9130269353dc/src/nkilib_src/nkilib/core/moe/moe_cte/bwmm_shard_on_I.py)
at commit `ad2f1b9`. We traced the three functions that access the shared
`output` tensor (the initializer, the per-block load of the previous partial
result, and the per-block store) unchanged, stubbed the computation between
them, and assumed that the block's token indices are distinct and in range.
Sizes were scaled down: two-row tiles, two tiles per block, four output rows,
one block.

The check reports two patterns:

1. **Initialization against the first block.** On the accumulating path both
   cores zero the whole `output` tensor, and the first `core_barrier` comes at
   the end of block 0. Epoch 0 therefore holds core 1's zero writes and core
   0's block-0 stores to its own rows: a zero against a computed value. If
   core 1's zeroing runs late, it erases core 0's first partial result. The
   two cores also zero the same rows as each other; that zero/zero pair writes
   identical values and is the only part of this pattern that the monitor's
   treatment of write-write conflicts overstates.
2. **Whole-block load against the peer's store.** Within a block, each core
   loads the previous partial result for all rows of the block, and each core
   stores only its own rows. One core reads rows the other core writes in the
   same epoch. The loaded rows feed the cross-core reduction.

With the initialization removed, the trace is still `sat`: the second pattern
alone is a conflict. Adding a barrier after the initialization separates the
zeroing from the block-0 stores but leaves the zero/zero pair and the second
pattern, so that configuration is also `sat`. The variant of the initializer
that writes one core's part expects a three-dimensional output with one plane
per core and does not apply to this kernel's two-dimensional output.

Whether either pattern produces a wrong result on hardware depends on
execution ordering that the Python source does not state: when each core's
DMA transfers complete relative to the other core's, and whether a core uses
the peer rows it loads. We did not establish that either pattern is a bug.
The check reduces the question to these two named access pairs, which the
kernel's maintainers can answer from the intended synchronization.

### Where a gate would go

A race-freedom gate is the proof together with its barrier-deletion mutant,
regenerated when the kernel changes. A tiling or sharding change can break
the barrier discipline without changing any computed value, and the check
costs little because it ignores the arithmetic. For the kernel above the
check returns `sat`, so a gate cannot pass until the two patterns are resolved:
either the code changes, or the intended ordering is written into the model
as an explicit assumption that the run reports.

## Limits

- **Write-write conflicts are reported whatever the values.** Two cores
  writing the same value to a cell are reported as a race, as with the
  zero/zero pair above. A check that compares the written values would
  remove those reports; it is not implemented.
- **Assumptions are part of the result.** The kernel trace assumes distinct,
  in-range token indices. Address reasoning carries the same A-INB assumption
  as the data-movement proofs. The run reports the assumptions.
- **The model is the barrier structure, not the hardware.** The check shows
  whether barriers order the conflicting accesses. It does not model DMA
  completion or the accelerator's memory model.
- **Barrier alignment is checked by count.** The tracer compares the number of
  barriers each core executes per tracked tensor. With several tracked tensors
  it does not compare the interleaving of barriers on different tensors.
- **Stubs and stand-ins.** The kernel trace stubs the computation between
  tracked accesses and replaces the token-index loader with distinct symbolic
  indices. The blockwise stand-in replaces the kernel's sharding with a parity
  partition. Each changes what the result covers.

---

## For verification developers

For the blockwise stand-in with sharded initialization, `sea-vc` time grew
from 0.05 s (172-line program, 8 rows by 4 blocks) to 0.34 s (20,504 lines,
256 rows by 16 blocks). The monitor emits one disequality per cross-core
access pair per epoch, at least one of them a write.

For a gather or scatter, a recorded row offset is a token index the kernel
loaded from memory. Both cores load their indices from the same shared input
tensor, so a disequality compares loaded values, which the distinctness
assumption constrains. On the kernel trace, `sea-vc` reports
`vc.ackermann-constraints: 0`, with about 0.03 s of VC generation and 0.02 s
of solving. The single-core control checks that the monitor asserts nothing
when there are no cross-core pairs.

On the micro kernel, `sea-run --fuzz=200` with the `ult` bound ran 95 of its
200 trials with range draws. On the barrier-deletion mutant, 14 to 24 of 200
trials failed the race assertion across seeds 1 to 5; the clean kernel had no
failures. With a signed `sge`/`slt` bound the fuzzer reported no ranges, and
173 of 200 trials stopped at the assumption.
