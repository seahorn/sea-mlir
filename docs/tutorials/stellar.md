# Stellar

stellar-core applies transactions in parallel clusters and merges each
cluster's ledger changes into a global view. The Stellar jobs check hand
ports of stellar-core components to C (commit `a1f4383`), each against a
specification written as ghost state in the harness. This page follows the
merge of read-only TTL bumps (ST-TTL). It shows a historical bug, and a
counterexample that is a real run of the harness but not a run of
stellar-core.

## The question

After `commitChangeFromThread` and `maybeMergeRoTTLBumps` run for $K$
committing threads, is the global entry for a key the one the
specification gives? The specification: read-only TTL bumps are merged by
taking the maximum; otherwise the last writer wins.

## The harness

The harness tracks one symbolic key $k^*$. It computes the
specification's result (`sHas`, `sVal`, `sIsNew`) next to the port's
global entry (`gm`) and compares them:

```c
  // the global entry for k* is the spec's
  sassert(gmHas == sHas);
  if (sHas) {
    sassert(gm.mLedgerEntry.mScopeID.mMovedFrom == false);
    sassert(optle_eq(gm.mLedgerEntry.mEntry, sVal));
    sassert(gm.mIsNew == sIsNew);
```

The number of threads is concrete and only the data is symbolic, so there
is no aliasing for SeaM to resolve. The jobs on this page leave no memory
constraints for the solver (`vc.ackermann-constraints: 0`).

## The fixed code

```console
$ sea-vc stellar/ttl-contract-K1.mlir --lower --seam --widen-memory=4
...
seam.inb: assumed accesses=316 conditions=0
solve-time: 0.090
result: unsat
```

The contract also holds for $K = 2$ and $K = 3$, each in under a second.

## A historical bug

stellar-core commit `7e4a427f` fixed the merge. Before the fix,
`maybeMergeRoTTLBumps` always returned false, and the caller overwrote the
merged entry with the thread's entry. With the fix reverted and an arbitrary
pre-state, one thread is enough for a counterexample:

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

The path ends in `optle_eq`, called from line 270 of the harness: the
comparison of the global entry with the specification's value. The replay
validated, so this is a real run of the harness.

It is not a run of stellar-core. In this run the thread chose its TTL
independently of the global one, and the callers never do that: every
cluster starts from the same pre-state, and a read-only bump only raises
the TTL. The `ttl-caller-*` jobs assume these caller invariants. With
them, one thread is not enough:

```console
$ sea-vc stellar/ttl-caller-revert-7e4a427f-K1.mlir --lower --seam --widen-memory=4
...
result: unsat
```

Two threads reproduce the bug:

```console
$ sea-vc stellar/ttl-caller-revert-7e4a427f-K2.mlir --lower --seam --widen-memory=4
...
seam.inb: assumed accesses=556 conditions=0
solve-time: 0.165
result: sat
...
validate: checked=1119 mismatches=0 dontcare=44
```

From a common pre-state, cluster A raises the TTL, cluster B raises it by
less, and committing A and then B loses A's bump.

A `sat` from a harness with havocked inputs is a statement about the
function's contract. It is a statement about the system only when the
callers' invariants are in the harness.

## The other components

The family has ports of six components, each with proofs and mutants:

| prefix | component |
|---|---|
| `scope-` | `LedgerEntryScope` (ST-SCOPE) |
| `ttl-`, `comm-` | the TTL merge, and the order independence of committing clusters |
| `part-` | the stage and cluster construction of the parallel transaction-set builder |
| `conf-` | the per-transaction conflict sets of the builder |
| `fee-` | total fees of a transaction set, with a historical fix (`bcb13715`) |
| `val-` | the cross-cluster conflict check of the validator |

Each job runs one configuration at a size $K$ (calls, threads,
transactions or clusters). A mutant job is expected `unsat` below its
shortest violating $K$ and `sat` at it
([Proof controls](../guide/proof-controls.md#mutants-and-reverts)). The ST-SCOPE harness makes the
operation, the slot and the scope symbolic at every step, so slots can
alias, and the cost grows with $K$: `scope-spec` took 0.6 s at $K = 1$,
4 s at $K = 2$ and 37 s at $K = 3$ when the expectations were established.
