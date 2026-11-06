# Firedancer: HTTP/2 flow control

HTTP/2 limits how much each side may send with flow-control windows, per
connection and per stream (RFC 9113, section 6.9). Firedancer's h2 and
gRPC client keeps these windows in a few counters, with a debt for windows
the peer shrank below zero. An error here stalls a stream or lets the peer
send unbounded data. This page proves one step of the protocol against the
RFC, and shows how reach jobs guard such a proof against vacuity.

## The question

Does each frame kind (SETTINGS, connection and stream WINDOW_UPDATE, data
sent, DATA received, window refills, stream open) keep the client's
windows equal to the windows the RFC defines, and does it raise exactly
the errors the RFC requires?

## The harness

The harness keeps the RFC's windows as ghost variables: $E_c$ and
$E_s[k]$ for what the peer allows the client to send, and $R_c$ and
$R_s[k]$ for what the client allows the peer. It builds an arbitrary state
of the connection and its streams, computes the ghost windows from the
client's counters (a send window is the counter minus its debt), and
assumes the invariant that relates them. Then it runs one operation, a separate job for each frame kind. For
the connection WINDOW_UPDATE:

```c
  uint raw = nd_uint(); uchar pl[4];
  FD_STORE( uint, pl, fd_uint_bswap( raw ) );
  long inc = (long)( raw & 0x7fffffffU );
  int rfc_proto = inc==0, rfc_fc = !rfc_proto && E_c+inc>MAXW;
  fd_h2_rx_window_update( c, tx, &cb, pl, 4UL, 0U );
  if( rfc_proto ) sassert( dead() && c->conn_error==FD_H2_ERR_PROTOCOL );
  else if( rfc_fc ) sassert( dead() && c->conn_error==FD_H2_ERR_FLOW_CONTROL );
  else { sassert( !dead() ); E_c += inc; }
```

The claims follow the RFC: an increment of 0 is a protocol error; an
increment that takes the window above $2^{31}-1$ is a flow-control error;
any other increment adds to the window. After the operation the harness
asserts the invariant again.

## The proof

```console
$ sea-vc firedancer/h2fc-wu-conn-proof.mlir --lower --seam --widen-memory=4
...
vc.ackermann-constraints: 0
seam.inb: assumed accesses=437 conditions=0
solve-time: 0.515
result: unsat
```

SeaM resolved all 437 accesses, with no in-bounds condition left in the
query and no memory constraints for the solver. With `--seam-stats`, the
counters show how: 228 reads were answered by a lookup in a Store-Map, a
per-object map of the writes at constant offsets
(`seam.rule.row.smap-hit: 228`).

## A revert

Firedancer commit `5f588f335` added the 31-bit limit on the connection
send window; the job `h2fc-wu-conn-revert-5f588f335` removes that limit
again.

```console
$ sea-vc firedancer/h2fc-wu-conn-revert-5f588f335.mlir --lower --seam --widen-memory=4
...
result: sat
...
replay: assert-failed
replay: assert-at: .../h2fc_step.c:269:21
replay: assert-op: firedancer/h2fc-wu-conn-revert-5f588f335.mlir:1330:3
replay: assert-path: .../h2fc_step.c:269:21 <- .../h2fc_step.c:269:12 <- OUT/fd_h2_conn.c:564:3 <- ...
validate: checked=839 mismatches=0 dontcare=0
interp.earlier-version-loads: 101
```

`assert-at` names the flow-control claim of the harness: the window
overflowed and the connection did not fail.
`interp.earlier-version-loads` counts the loads of the replay that read
an earlier memory version. The interpreter
reads memory in place, which is correct only if the cell was not
overwritten since that version, as the program's memory form claims.
`validate` checks the value of each of these loads, and found no
mismatch.

## A reach job

A reach job asserts the negation of one condition, with the other checks
off; a validated `sat` whose `assert-at` is that assertion shows that the
condition can occur
([Proof controls](../guide/proof-controls.md#reach-jobs)).
`h2fc-wu-conn-reach1` asserts that no increment overflows a connection
window that is below the maximum:

```console
$ sea-vc firedancer/h2fc-wu-conn-reach1.mlir --lower --seam --widen-memory=4
...
result: sat
...
replay: assert-at: .../h2fc_step.c:271:3
...
validate: checked=846 mismatches=0 dontcare=0
```

`assert-at` names the reach assertion, so an increment that overflows
the window does occur in the step. Without this job, the `unsat` above
could hold only because the harness excluded the interesting executions.

## Vacuity in practice

The harness records one case where this mattered. The step for received
DATA once contained a 48-iteration loop to fill a receive buffer. SeaHorn
unrolls loops to the bound 8 and blocks the paths that need more
iterations, so every path through that step was cut, and its proof would
have been trivially `unsat`. Reach probes found it. The harness now fills
the buffer with straight-line code. A
[vacuity twin](../guide/proof-controls.md#vacuity-twins) at the end of the
step detects this case directly.

## Results

The connection and stream WINDOW_UPDATE, SETTINGS, data sent, refill and
stream-open steps are proved, with reach jobs and mutants; both reverts of
`5f588f335` (connection and stream) are caught. The DATA-received step is
not solved within the benchmark limits (`h2fc-rx-data-*`, tier `hard`).
The `h2fc-blowup-*` jobs keep inputs on which SeaM's query generation once
needed more than 900 s and 20 GB, as regression tests.
