# Firedancer: QUIC fragment reassembly

A fragment overlaps bytes already received. Another arrives with an end
offset just beyond the transaction buffer. Then the stream is cancelled.
The reassembly code must handle each operation without corrupting a
neighboring payload or leaving a stale map entry behind. Testing the
operations separately does not by itself establish that the next call
starts from a valid state.

We check Firedancer's `fd_tpu_reasm_frag` and `fd_tpu_reasm_cancel` by
stating what must remain true between calls. That lets one verified step
cover a sequence of any length in the modeled configuration. The study
checks byte preservation, bounds and slot bookkeeping, and catches both
planted mistakes and a historical fix reverted in the source.

## Scope of the reassembly check

The harness fixes a small configuration: five slots, with two streams
prepared in the initial state. It checks fragment and cancel operations
on the target stream while preserving the other stream. Fragment offsets,
lengths and bytes are symbolic. The proof covers arbitrarily many of
these operations, not arbitrary slot counts or the complete prepare and
publish lifecycle.

## The question

For any sequence of `fd_tpu_reasm_frag` and `fd_tpu_reasm_cancel` calls,
do the claims C1 to C6 of the harness hold?

- C1: no fatal log or failed internal check is reached;
- C2: the return codes and the slot size are the ones specified;
- C3: the payload holds the bytes that were written, at a probe position;
- C4: no byte of the data region outside the target payload changes (the
  destination bound of the copy, fixed in Firedancer commit `bb4688cd`);
- C5: after a cancel, the target slot is free and gone from the map, and
  the other stream is still found;
- C6: the library's own consistency check, `verify_state`, passes.

## How the check follows the C code

The jobs use Firedancer commit `37c658197`. Their C harnesses and extracted
programs are in the [benchmark repository](https://github.com/seahorn/sea-mlir-bench/tree/main/firedancer).
An agent writes the contract, chooses the environment assumptions and
constructs deliberate regressions. Clang and SeaHorn compile the harness
and selected source into Sea-MLIR; Sea-MLIR checks the resulting program
and replays violating inputs. The export settings and source transformations
are recorded in the family's
[provenance](https://github.com/seahorn/sea-mlir-bench/blob/main/firedancer/PROVENANCE.md).

The agent writes a model of the target slot and an invariant for the
queue and map membership. The harness starts from any state satisfying
that invariant, executes the real fragment or cancel code, and checks the
claims and the invariant again. Initialization is checked separately with
a native test of the fixed base state.

The slot map is replaced by a contract for a set with unique keys. This
avoids reasoning through hash calculations when the claim concerns
reassembly behavior. The map implementation itself remains a separate
verification obligation. The other assumptions are listed below; this
model boundary is part of the result, not an implementation detail to hide.

## Results

| job | expected | time when established |
|---|---|---|
| `step-claims-cancel`, `step-claims-frag` | `unsat` | 7 s, 24 s |
| `step-inv-cancel`, `step-inv-frag` | `unsat` | 7 s, 15 s |
| `step-vs-cancel`, `step-vs-frag` | `unsat` | 156 s, 239 s |
| four mutants, one revert of `071c0006` | `sat` | 1 s to 14 s |
| `step-reach-sz`, `step-reach-trim` | `sat` | 599 s, 7 s |

The bounded runs did not finish for one operation. The inductive step
proves C1 to C6 for any number of the modeled fragment/cancel operations
in the fixed configuration, under the assumptions below.

## A case for a reassembly gate

We would pilot a gate around fragment trimming, destination bounds and
cancel bookkeeping. These are stateful operations on network-supplied
inputs, and a mistake can affect a different payload or a later call.
There is concrete regression evidence: the family includes a historical
revert, and the planted off-by-one and trim mistakes fail the claims they
were designed to challenge.

The invariant is particularly useful for maintenance. Removing a map entry
on cancel may look like bookkeeping, but leaving it behind can violate
the state needed by future operations even if the immediate return value
looks right. Checking both the operation's result and the state it leaves
protects that relationship through a refactor.

The reported costs make a focused pilot plausible, although some
consistency and reachability jobs take minutes. Regenerate from changed
C source, retain the native base check and map-contract assumptions, and
run the relevant regression controls. This is a conditional gate for the
modeled operations; extending it to prepare, publish or larger layouts
requires additional work.

## What the proof rests on

- **The map contract.** The slot map implements a set of slots with
  unique keys. The step uses this contract instead of the map's code, and
  checks the contract's preconditions where the code calls the map. The
  map's own operations are a separate obligation.
- **Valid keys.** Preparing a slot for a new stream inserts a valid key
  into the map (assumption E-KEY in the harness).
- **In-bounds accesses** for the jobs that report `seam.inb: assumed`.
  `step-inv-cancel` above does not rely on it.
- **The base case**, checked natively.
- **The pointer analysis of the export.** The invariant constrains a
  pointer loaded from havocked memory, and SeaHorn's pointer analysis
  needs to be told that it aliases a known buffer
  ([Known limitations](../reference/limitations.md#programs-exported-by-seahorn)).

The controls are the four mutants and the revert of `071c0006`, each
expected `sat` against the obligation it breaks, and two reach jobs: a
fragment rejected for its size (`step-reach-sz`) and a fragment that
overlaps data already received (`step-reach-trim`). See
[Proof controls](../guide/proof-controls.md).

---

## For verification developers

These sections explain why the bounded encoding was difficult, how the
inductive step avoids it, and how to interpret the memory-model diagnostics.
Commands and timings use the snapshot in [Applications](index.md).

### Bounded runs

The bounded jobs `frag-spec-K1` and `frag-spec-K2` build a reassembly
state with the library's own initialization and then run $K$ calls. The
verifier has to tell slots apart through the map's hash of the connection
and stream ids. Neither job was solved within the benchmark limits of
900 s and 16 GB; they are in the `hard` tier with expected verdict
`unknown-ok`.

### One inductive step

The `step-*` jobs prove an invariant Inv instead. With $\mathit{Init}$
the initial states, $T$ one call of `fd_tpu_reasm_frag` or
`fd_tpu_reasm_cancel` with arbitrary arguments, and $C$ the claims:

$$
\mathit{Init} \subseteq \mathit{Inv}, \qquad
\mathit{Inv}(s) \land T(s, s') \implies \mathit{Inv}(s') \land C(s, s').
$$

The harness starts from an arbitrary state, assumes Inv, runs one
operation, and asserts the claims and Inv. The program has no loops left
to unroll, and its proof covers any number of operations. Inv says that
the slots form the library's queue and that the map holds every busy slot
with unique keys. It also ties the slots to ghost state: a model of the
target slot (its key, its length, whether it is busy) and one ghost bit
per slot for membership in the map. The base case, that Inv holds after
initialization, concerns one deterministic state and is checked by a
native test outside the benchmark.

The proof is split by operation and by obligation: the claims
(`step-claims-*`), the invariant (`step-inv-*`), and `verify_state`
(`step-vs-*`). A job that checks one of these may assume another that a
different job checks ([check classes](../guide/proof-controls.md#check-classes)).

### Running it

The invariant after a cancel:

```console
$ sea-vc firedancer/step-inv-cancel.mlir --lower --seam --widen-memory=4
Warning: an access leaves its object (7 access(es) with no alternative inside its object: a numeral offset or an offset range wholly outside it, or an object of size 0 such as null): --seam's A-INB does not hold; running without it [vc.seam-inb-violated]
tool.version: sea-mlir 0.1.0 (...)
...
vc.seam-inb-violated: at .../reasm_inv.h:92:7 load: numeral offset outside: offset 5 + 1 unit(s) > size 5 of object global.in_map
...
vc.seam-inb-violated: at .../reasm_inv.h:19:16 load: numeral offset outside: offset 304 + 4 unit(s) > size 304 of object global.ws
...
vc.seam-inb-violated-count: 7
...
vc.ackermann-constraints: 2252
seam.inb: off (fallback: an access leaves its object)
solve-time: 5.889
result: unsat
```

This run shows a safeguard at work. The invariant loops over the 5 slots
(`SLOT_CNT`), but SeaHorn unrolls every loop to the bound 8. Iterations 5
to 7 are infeasible, yet they contain accesses past the end of `in_map`
and `ws` at constant offsets. SeaM finds accesses that cannot be inside
their object, so it does not assume that accesses stay in bounds, and runs
again without that assumption (`seam.inb: off`). This `unsat` therefore
rests on no in-bounds assumption. The report names the accesses; it cannot
tell that their path is infeasible.

### A mutant

The mutant `step-mut-bounds-1` relaxes a size check in `fd_tpu_reasm.c`
by one byte, so that a fragment may end one byte past the payload. Its job
checks the frame claim C4 alone:

```console
$ sea-vc firedancer/step-mut-bounds-1.mlir --lower --seam --widen-memory=4
...
seam.inb: assumed accesses=152 conditions=66
solve-time: 0.896
result: sat
...
replay: assert-failed
replay: assert-at: .../reasm_step.c:284:3
replay: assert-op: firedancer/step-mut-bounds-1.mlir:1030:3
replay: assert-path: .../reasm_step.c:284:3 <- OUT/fd_tpu_reasm.c:206:3 <- OUT/fd_tpu_reasm.c:198:7 <- ...
validate: checked=472 mismatches=0 dontcare=0
```

The replay fails and agrees with the model, so this is a counterexample.
`assert-at` names the C4 check of the harness: after the copy, a byte of
the data region outside the target payload has changed. `assert-path`
lists the branches before it, most recent first. They are in the mutated
copy of `fd_tpu_reasm.c` (`OUT/` is the export's output directory), so
the run went through the relaxed check.
