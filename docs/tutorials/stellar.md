# Stellar: preserving TTL bumps across parallel clusters

Two clusters start from the same ledger entry, whose lifetime extends to
ledger 100. One extends it to 200; the other extends it to 150. When both
finish, the entry should live until 200, whichever cluster commits first.
A refactor can break that rule without changing the `std::max` that
computes the answer.

That happened in stellar-core: a `return true` moved inside a callback,
so it no longer told the caller that the merge had succeeded. The caller
then overwrote the merged value. This tutorial shows how a small symbolic
harness reproduces that historical defect and checks the fixed behavior.
The useful boundary is the merge contract: it connects parallel execution,
ledger state and the scope machinery used to move entries between them.

## Why read-only transactions can still need a merge

stellar-core groups transactions into clusters that can execute in
parallel within a stage. Transactions that access the same ledger entry
must share a cluster if one writes it. Read-only users can run in separate
clusters, but they may still extend that entry's time to live (TTL).
For example, several transactions may use the same contract instance and
extend its lifetime.

These read-only TTL bumps are a special case. Each cluster can compute
its bump independently; committing the results must keep the largest
`liveUntilLedgerSeq`. If a transaction writes the underlying entry,
that freedom disappears: writes and TTL bumps must be ordered together.
The [parallel-apply contract](https://github.com/stellar/stellar-core/blob/a1f4383b588f56c01c72fe3318eb9d87fe09053e/src/transactions/ParallelApplyUtils.cpp#L41-L80)
explains this distinction.

The study uses stellar-core commit `a1f4383`. It checks C ports of selected
components, rather than compiling the full C++ node. The ports and their
source correspondence are included in the
[benchmark family](https://github.com/seahorn/sea-mlir-bench/tree/main/stellar).

## The property we check

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
Order independence alone would be too weak: consistently discarding every
bump would also be order independent.

### How the harness reaches the code

The agent-built harness reduces the sharded map to one symbolic key and
its slot. This is justified by the slice: `commitChangeFromThread` touches
`map[key]` through `try_emplace` and the resulting iterator. Other keys do
not affect that call's merge decision. The map mock avoids modeling hash
tables while retaining the entry's presence, value and flags.

The port also retains the relevant scope operations: moving an entry into
the global scope, inserting it, or assigning over an existing entry.
The specification maintains its own expected state alongside the port.
Clang compiles the C harness to LLVM; SeaHorn exports it to Sea-MLIR.
Sea-MLIR then checks the assertions and replays any counterexample.

The agent chooses this boundary, writes the port and specification, and
identifies the caller assumptions. The verifier checks the resulting
program mechanically. Agreement between its solver and interpreter does
not by itself prove that the hand port matches stellar-core's C++.

## Reproducing the historical regression

Run these commands from the benchmark checkout; [Applications](index.md)
records the snapshot used for the example output. The fixed port satisfies
the local contract:

```console
$ sea-vc stellar/ttl-contract-K1.mlir --lower --seam --widen-memory=4
...
result: unsat
```

Here `K1` means one committing cluster. `unsat` means no input admitted by
the harness violates the assertion. The recorded contract jobs also pass
for two and three clusters, each in under a second. These are bounded
results, not a proof for arbitrarily many clusters.

The historical fix is
[`7e4a427f`](https://github.com/stellar/stellar-core/commit/7e4a427f644e80c3eef86324b18fe493f4ca0394).
Before it, the callback correctly computed the maximum but the enclosing
`maybeMergeRoTTLBumps` function always returned false. Its caller therefore
replaced the merged entry with the incoming entry.

The strongest demonstration uses the `ttl-caller-*` jobs. They include
the caller facts that every cluster starts from the same pre-state and a
read-only bump can only increase the TTL. With the fix reverted, one
cluster cannot expose the lost-update pattern:

```console
$ sea-vc stellar/ttl-caller-revert-7e4a427f-K1.mlir --lower --seam --widen-memory=4
...
result: unsat
```

Two clusters can:

```console
$ sea-vc stellar/ttl-caller-revert-7e4a427f-K2.mlir --lower --seam --widen-memory=4
...
solve-time: 0.165
result: sat
...
validate: checked=1119 mismatches=0 dontcare=44
```

The failure has the shape of the opening example. Cluster A raises the
shared starting TTL more than cluster B. After A commits, B's commit
computes the correct maximum and then overwrites it with B's smaller bump.
`sat` supplies a violating input; replay confirms the failure in the
exported program with no model-validation mismatches.

This reproduces a known defect, not a newly discovered Stellar bug.
The harness does not model transaction admission or numeric TTL limits,
so its raw solver values are not ready-made ledger fixtures. The shared
pre-state and monotonic bumps establish the relevant caller pattern;
a native C++ regression should instantiate it with valid transactions.

## Where we would try a regression gate

The TTL merge is a strong candidate for a focused CI gate. There is a
known failure mode: losing a paid lifetime extension can let an entry
expire or be archived earlier than requested. History shows that a
scope/callback refactor can reintroduce it while
leaving the arithmetic intact. The intended result is small and stable:
retain the maximum legal read-only bump and the correct entry metadata.
The checked two- and three-cluster instances are also cheap enough to
justify exploring a per-change check.

We would start with the fixed caller-aware jobs, the historical revert as
a control, and a native C++ test of two valid clusters committing in both
orders. The native test would connect the source to the port; the symbolic
jobs would cover all values admitted by the bounded harness, including
combinations of entry presence, `mIsNew`, read-write-set membership and
value orderings that a single concrete example does not cover. Keep the
contract check as well as the order comparison: neither subsumes the
other.

The main adoption cost is maintaining the C port and its assumptions when
the C++ changes. Rechecking an old exported `.mlir` file only gates the
verifier. A gate on stellar-core needs a source-change workflow that
updates or confirms the port, regenerates the jobs, and reruns the
controls. The historical regression and the small symbolic state space
make this worth a pilot; they do not establish that this workflow is
already economical in Stellar's CI.

### What the result relies on

The contract is conditional on the port's fidelity, matching key and
entry types, and the ownership of map shards by workers. The harness
executes commits sequentially; it does not verify races between workers.
SeaM also assumes that low-level memory accesses remain within their
objects, reported as `seam.inb: assumed`.

Some caller facts span several components. For example, the merge keeps
the old dirty flag, so write-back depends on a global TTL entry already
being dirty. That follows from valid footprints excluding TTL keys and
the paths that populate the global map. It is a caller invariant to
justify, not a fact the merge function establishes on its own.

The family also checks the validator's cross-cluster conflict rule.
That rule supports the partition premise for transaction sets that pass
validation. Catchup and replay do not rerun that validator on every apply;
those paths additionally rely on the prior validity of historical data.
The study does not turn that trust boundary into a newly found defect.

## Other components in the family

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

---

## For verification developers

The remaining details concern specification strength, replay and the
encoding. They explain how to interpret the jobs rather than extending
the application claims above.

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
