# Firedancer: HTTP/2 flow control

A peer can reduce the initial stream window while a gRPC stream is open.
The stream may then have sent more data than the new window allows.
Subsequent WINDOW_UPDATE frames must repay that debt before it can send
again. Keeping
only a nonnegative “bytes available” counter loses this distinction.

Firedancer represents the effective send window with a counter and a
debt. We check that its HTTP/2 handlers and gRPC callbacks preserve the
protocol's accounting as SETTINGS, WINDOW_UPDATE and send operations
change those fields. A small arithmetic contract connects the protocol
rule to the implementation, including the error cases at the 31-bit limit.

## The protocol contract

HTTP/2 has separate windows for the connection and each stream. The
harness models what the peer permits the client to send and what the
client permits the peer to send. For a stream's send window, the protocol
value is `tx_wnd - tx_wnd_debt`.

For example, a connection WINDOW_UPDATE carries a 31-bit increment. Zero
requires a protocol error; an increment that makes the window exceed
`2^31 - 1` requires a flow-control error. Otherwise, the handler must add
the increment and keep the connection live. The harness encodes these
rules from RFC 9113 section 6.9 independently of the handler's code.

Each job runs one operation from an arbitrary state satisfying the
accounting invariant, with one connection and up to two streams. It checks
both the resulting windows and the required errors. This covers arbitrary
values at that step, rather than a short list of packet examples. These
are single-step preservation checks: no initialization base case is
checked, and the DATA-received step remains unresolved. Unlike the
[reassembly proof](inductive-proof.md), this is not yet a complete
inductive argument over operation sequences.

## What code is checked

See [Firedancer source and extraction](index.md#firedancer-source-and-extraction)
for the pinned revision, harnesses and shared frontend pipeline.

The selected code includes the HTTP/2 handlers and the real gRPC
flow-control callbacks, extracted from `fd_grpc_client.c`. The callbacks
matter: HTTP/2 reports a change to the initial window, while gRPC adjusts
its open streams and records their debt.

The harness omits ring-buffer payload copies to focus on accounting.
It therefore does not prove that transmitted or received payload bytes
are preserved. The results also retain the memory assumptions reported
by Sea-MLIR. These are checks of selected protocol transitions, not a
verification of the entire HTTP/2 client.

## Results and a regression worth guarding

The connection and stream WINDOW_UPDATE, SETTINGS, sending, refill and
stream-open steps pass their contracts. The DATA-received step remains
unsolved within the benchmark limits. No new upstream defect was found
in the proved steps.

Reverting the connection or stream limit check from Firedancer commit
`5f588f335` makes the corresponding check fail. For the connection case,
the harness finds an update that exceeds the allowed window but leaves
the connection live. Deliberately dropping debt creation or repayment is
also caught. These controls exercise the boundary between valid progress
and a required protocol error.

Losing debt can make the client send beyond the peer's permitted window,
causing the peer to reject the traffic with FLOW_CONTROL_ERROR. A missed
31-bit limit check instead keeps a connection live when the protocol
requires it to fail. We recommend exploring a CI gate around these
accounting transitions.
The protocol gives a stable reference while implementation changes can
move responsibility between HTTP/2 and gRPC. The historical limit fix and
the debt controls identify concrete behavior to preserve. Most of the
individual steps were practical in the study, so a pilot can begin with
WINDOW_UPDATE and expand after measuring regeneration and runtime costs.

Keep integration tests for packet handling and payload behavior. The
symbolic gate adds coverage over window values and update sizes under its
state invariant; it does not replace those tests. To protect changed
source, follow the [application CI workflow](../agents/index.md#5-regenerate-and-check-in-ci).
Retain the reachability checks for interesting window states.

---

## For verification developers

The following sections show the ghost-state model, solver output and
controls against a vacuous proof. The commands and timings use the
snapshot described in [Applications](index.md).

### The harness

The harness keeps the RFC's windows as ghost variables: $E_c$ and
$E_s[k]$ for what the peer allows the client to send, and $R_c$ and
$R_s[k]$ for what the client allows the peer. It builds an arbitrary state
of the connection and its streams, computes the ghost windows from the
client's counters (a send window is the counter minus its debt), and
assumes the invariant that relates them. Then it runs one operation,
with a separate job for each frame kind. For the connection WINDOW_UPDATE:

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

### The proof

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

### A revert

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

### A reach job

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

### Vacuity in practice

The harness records one case where this mattered. The step for received
DATA once contained a 48-iteration loop to fill a receive buffer. SeaHorn
unrolls loops to the bound 8 and blocks the paths that need more
iterations, so every path through that step was cut, and its proof would
have been trivially `unsat`. Reach probes found it. The harness now fills
the buffer with straight-line code. A
[vacuity twin](../guide/proof-controls.md#vacuity-twins) at the end of the
step detects this case directly.

### Verifier regression inputs

The `h2fc-blowup-*` jobs retain inputs on which SeaM's query generation
once needed more than 900 seconds and 20 GB. These gate the verifier's
performance; they are separate from a gate on Firedancer's flow-control
behavior.
