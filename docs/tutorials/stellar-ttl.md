# Stellar: preserving lifetime extensions across commits

Two clusters start from the same ledger entry, whose lifetime extends to
ledger 100. One extends it to 200; the other extends it to 150. When both
finish, the entry should live until 200, whichever cluster commits first.
Losing the larger extension can let the entry expire earlier than requested.

A historical Stellar bug broke this rule even though the merge computed
the correct maximum. We reproduced the failure in a small C port of the
ledger-merge code, then checked the corrected behavior across symbolic
values and commit orders. The failure crossed a callback and its caller:
the value was right, but the signal that told the caller to keep it was lost.

This is one of the [Stellar stories](stellar.md). The
[queue-accounting story](stellar-txqueue.md) checks unchanged C++ method
bodies directly and explores how to model their environment.

## Why separate clusters still share a lifetime

Stellar groups transactions into clusters that can execute in parallel
within a stage. Transactions that access the same ledger entry must share
a cluster if one writes it. Read-only users can run in separate clusters,
but they may still extend that entry's time to live (TTL). Several
transactions can use the same contract instance and request more time for it.

Each cluster can compute its read-only bump independently. Committing the
results must preserve the largest `liveUntilLedgerSeq`. A transaction that
writes the underlying entry changes the rule: writes and TTL bumps must
be ordered together. Stellar's
[parallel-apply contract](https://github.com/stellar/stellar-core/blob/a1f4383b588f56c01c72fe3318eb9d87fe09053e/src/transactions/ParallelApplyUtils.cpp#L41-L80)
explains that boundary.

## The maximum was right; the caller overwrote it

The failure becomes visible when the larger bump commits first:

| Event | Intended lifetime | Lifetime with the historical regression |
|---|---:|---:|
| Shared starting entry | 100 | 100 |
| Cluster A commits its bump to 200 | 200 | 200 |
| Cluster B commits its bump to 150 | 200 | 150 |

`maybeMergeRoTTLBumps` performed the merge inside a callback. The callback
returned true, but the enclosing method returned false to its caller.
The caller interpreted false as “no merge” and replaced the entry with
B's smaller value. The arithmetic inside the callback needed no change.

The historical fix,
[`7e4a427f`](https://github.com/stellar/stellar-core/commit/7e4a427f644e80c3eef86324b18fe493f4ca0394),
carries the merge result back to the caller. The caller then keeps the
merged entry. Our check includes both methods so it can distinguish a
correct local maximum from a correct committed result.

## Check the caller's pattern

Both clusters must start from the same entry, and a read-only bump can
only extend its lifetime. Those facts explain why two clusters matter.
One cluster's legal bump cannot fall below its own starting lifetime.
Two legal bumps can differ, so the later commit must preserve an earlier,
larger extension.

The caller-aware check reproduces the regression with two clusters. The
corrected port satisfies the recorded contract for one, two and three
clusters, each in under a second in the recorded runs. The checks vary
entry values and metadata within each bounded scenario; they also compare
forward and reverse commit orders.

Order independence alone would be too weak. A function that discarded
every bump would also be order independent. The contract therefore checks
the resulting entry and metadata as well as the two orders.

## A regression gate around the merge boundary

This boundary is worth protecting because a refactor can lose the result
between a callback and its caller while leaving the merge formula intact.
A useful gate checks the fixed contract, keeps the historical revert as a
control, and confirms that the interesting commit pattern remains reachable.

The study uses C ports of selected Stellar components. Their correspondence
to the C++ source and the caller assumptions need review when that source
changes. Running the old exported program again checks the verifier against
the same input. Maintaining an application gate also requires regenerating
or reviewing the source model. The
[direct-C++ queue story](stellar-txqueue.md) takes a different approach to
that maintenance problem: it extracts the method bodies afresh.

---

## For verification developers

The sections below state the contract, reproduction commands and limits.
The study uses stellar-core commit `a1f4383`. The ports, source
correspondence and exported jobs are in the
[Stellar benchmark family](https://github.com/seahorn/sea-mlir-bench/tree/main/stellar).

### Contract and environment

For one selected key, the harness applies a concrete number of cluster
commits with symbolic entry values and flags. It checks
`commitChangeFromThread` and `maybeMergeRoTTLBumps` against these rules:

- Ignore an incoming entry that is not dirty.
- Insert the entry if the global map has no entry for the key.
- Merge a read-only TTL bump by taking the maximum, when both values
  exist and the key is outside the read-write set.
- Otherwise replace the payload with the incoming value, while preserving
  the `mIsNew` flag established when the global entry was first tracked.
- Leave the global entry dirty after a committed change, under the
  global-TTL-dirty premise needed for write-back.
- Complete the modeled scope operations without an exception.

The harness checks presence, payload and metadata, not just the TTL
number. The contract job also checks the dirty flag;
`ttl-no-global-dirty-K1` shows why its premise is needed. Separate jobs
compare forward and reverse commit orders under the caller premises.

The map environment tracks one selected key. The relevant methods touch
`map[key]` through `try_emplace` and the resulting iterator, so the model
retains presence, value and flags for that key while omitting hash-table
layout and allocation.

The port retains the relevant scope operations: moving an entry into the
global scope, inserting it, or assigning over an existing entry. An
independent specification tracks the expected state. Clang compiles the
harness to LLVM; SeaHorn exports it; Sea-MLIR checks the assertions and
replays counterexamples. Solver/interpreter agreement validates the
exported program's execution; it does not establish source-to-port fidelity.

### Reproduce the fixed behavior and historical revert

Run from the benchmark checkout. [Applications](index.md#reproducing-the-experiments)
records the snapshot used for these transcripts.

```console
$ sea-vc stellar/ttl-contract-K1.mlir --lower --seam --widen-memory=4
...
result: unsat
```

`K1` fixes one committing cluster. UNSAT means no input admitted by that
harness violates the assertion. These are bounded results, not a proof for
arbitrarily many clusters.

The caller-aware revert needs two clusters to expose the lost update:

```console
$ sea-vc stellar/ttl-caller-revert-7e4a427f-K1.mlir --lower --seam --widen-memory=4
...
result: unsat
$ sea-vc stellar/ttl-caller-revert-7e4a427f-K2.mlir --lower --seam --widen-memory=4
...
solve-time: 0.165
result: sat
...
validate: checked=1119 mismatches=0 dontcare=44
```

SAT supplies an input that violates the assertion. Replay confirms the
failure in the exported port with no model-validation mismatches. This
reproduces a known defect. The harness does not model transaction admission
or numeric TTL limits, so its raw solver values are not ready-made ledger
fixtures. A native C++ regression would instantiate the pattern with valid
transactions.

### What the result relies on

The port's fidelity, matching key and entry types, and workers' ownership
of map shards are premises. The harness executes commits sequentially;
it does not verify worker races. SeaM assumes low-level accesses stay
within their objects, reported as `seam.inb: assumed`.

The merge keeps the old dirty flag. Write-back therefore depends on a
global TTL entry already being dirty. Valid footprints exclude TTL keys,
and the paths that populate the global map supply this caller invariant;
the merge function does not establish it on its own.

The family also checks the validator's cross-cluster conflict rule. That
supports the partition premise for transaction sets that pass validation.
Catchup and replay do not rerun that validator on every apply; they also
rely on the prior validity of historical data. The study does not establish
a defect at that trust boundary.

### The local contract can expose unreachable inputs

A function-level harness may deliberately admit more states than its
callers can produce. In `ttl-revert-7e4a427f-K1`, the global TTL and the
incoming TTL are independent. A single smaller incoming bump therefore
exposes the historical overwrite:

```console
$ sea-vc stellar/ttl-revert-7e4a427f-K1.mlir --lower --seam --widen-memory=4
...
result: sat
...
replay: assert-failed
replay: assert-at: .../scope_port.c:127:31
replay: assert-op: stellar/ttl-revert-7e4a427f-K1.mlir:775:3
replay: assert-path: .../scope_port.c:127:31 <- .../scope_port.c:125:7 <- .../scope_port.c:123:7 <- .../ttl_merge_port.c:270:5 <- ...
validate: checked=619 mismatches=0 dontcare=22
interp.earlier-version-loads: 3
```

The path ends in `optle_eq`, where the harness compares the global entry
with its specification. Replay validates this execution of the harness.
It does not make the one-cluster input reachable in stellar-core: a
read-only caller cannot lower the shared starting TTL. This is why the
two-cluster, caller-aware control is the application-facing demonstration.

### Ghost state and memory simplification

The harness tracks one symbolic key $k^*$ and computes the specification's
result (`sHas`, `sVal`, `sIsNew`) alongside the global entry (`gm`):

```c
  sassert(gmHas == sHas);
  if (sHas) {
    sassert(gm.mLedgerEntry.mScopeID.mMovedFrom == false);
    sassert(optle_eq(gm.mLedgerEntry.mEntry, sVal));
    sassert(gm.mIsNew == sIsNew);
  }
```

For these TTL jobs, the structure is concrete and accesses resolve
statically. SeaM leaves no Ackermann constraints to relate potentially
aliasing reads (`vc.ackermann-constraints: 0`). Symbolic payloads and flags
still determine the branches and final property. This is the expected
simplification baseline for this slice, not evidence that the input values
were concrete.

The output keeps the encoding assumption visible:

```console
$ sea-vc stellar/ttl-contract-K1.mlir --lower --seam --widen-memory=4
...
seam.inb: assumed accesses=316 conditions=0
solve-time: 0.090
result: unsat
```

### Bounds, controls and more expensive state spaces

Each job fixes a size $K$: calls, committing clusters, transactions, fees
or validator clusters. A mutant may remain equivalent below the smallest
size that exposes it. The manifest therefore expects `unsat` below that
size and `sat` at it
([Proof controls](../guide/proof-controls.md#mutants-and-reverts)). Reach
jobs check that the harness admits the intended behaviors.

The scope harness is more expensive than the TTL slice because each step
chooses the operation, entry slot and scope symbolically. Slots can alias.
When expectations were established, `scope-spec` took 0.6 seconds at
$K=1$, 4 seconds at $K=2$ and 37 seconds at $K=3$. Those costs describe a
broader symbolic state space, not the cost of the small TTL regression
check. The [family provenance](https://github.com/seahorn/sea-mlir-bench/blob/main/stellar/PROVENANCE.md)
records the export configuration; the manifest records the individual
bounds, controls and expected verdicts.

### Related jobs in the benchmark family

The exported jobs explore several related boundaries. Each uses concrete
structure sizes and symbolic data; the prefix identifies the relevant
jobs and harness in the benchmark.

| Prefix | Component | Checked property |
|---|---|---|
| `scope-` | `LedgerEntryScope` | Scope access and move behavior agree with a ghost specification; separate queries characterize API misuse. |
| `ttl-`, `comm-` | Parallel TTL commit | Merge results match the contract; forward and reverse orders agree under the recorded caller premises. |
| `part-` | Parallel transaction-set builder | Different bins have no conflicting transactions; cluster membership and resource accounting remain consistent. |
| `conf-` | Footprint-to-conflict construction | Every required conflict is present and the relation is symmetric; distinct-key hash collisions are allowed. |
| `fee-` | Transaction-set fee totals | For nonnegative fees, saturated totals equal the exact sum capped at `INT64_MAX`; a historical overflow fix supplies a control. |
| `val-` | Cross-cluster validator | The cross-cluster check accepts exactly the stages without cross-cluster read/write conflicts, within the modeled domain. |

These are separate bounded checks. In particular, a fee-total theorem does
not determine the full transaction-set ranking, and a builder check does
not verify the complete path from a proposal to ledger application.
