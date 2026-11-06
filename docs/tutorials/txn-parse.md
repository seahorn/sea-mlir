# Firedancer: transaction parsing

Every Solana transaction a Firedancer validator receives goes through
`fd_txn_parse_core` before anything else reads it. The parser reads an
untrusted payload of up to 1232 bytes and writes a descriptor into a
buffer of `FD_TXN_MAX_SZ` = 854 bytes. This page shows proofs of the
parser's bounds with loop invariants, and a size constant whose reason
the proof made explicit.

## The question

For every payload up to the size limit: does every write stay inside the
descriptor buffer, does every read stay inside the payload, and are the
extents and indices recorded in the descriptor valid? The harness states
these as claims T1 (footprint), T2 (extents) and T3 (header and account
indices).

## Cutting the loops

The parser loops over instructions, account indices and address tables.
A bounded harness unrolls each loop 8 times and covers only transactions
within that bound. The proof jobs (`txn-pf-*`) replace each loop by an
[inductive cut](../guide/proof-controls.md#inductive-cuts) in a copy of
the parser; the rest of the parser is unchanged.

The invariant of the instruction loop says that the read position $i$
stays within the payload, that

$$
i \ge i_0 + 3j
$$

after $j$ instructions (each instruction takes at least 3 bytes), and
that the extents and program id of every earlier instruction are valid. A
claim about all instructions is checked at one nondeterministic
instruction index, which stands for all of them.

## A mutant

The mutant `txn-pf-v1-mut-read` drops the bounds check before the parser
reads `ro_unsigned_cnt`, and the job checks that every read stays inside
the payload:

```console
$ sea-vc firedancer/txn-pf-v1-mut-read.mlir --lower --seam --widen-memory=4
...
vc-time: 0.614
...
vc.ackermann-constraints: 12563
seam.inb: assumed accesses=652 conditions=479
solve-time: 79.656
result: sat
...
replay: assert-failed
replay: assert-at: OUT/fd_txn_parse.c:118:31
replay: assert-op: firedancer/txn-pf-v1-mut-read.mlir:6882:3
replay: assert-path: OUT/fd_txn_parse.c:118:31 <- OUT/fd_txn_parse.c:114:77 <- OUT/fd_txn_parse.c:112:77 <- OUT/fd_txn_parse.c:103:7
validate: checked=96 mismatches=0 dontcare=1
```

The replay fails and agrees with the model. `assert-at` names the read
of `ro_unsigned_cnt` in the mutated copy of the parser, and `assert-path`
shows the header checks the run passed on the way. This read comes before
the parser's loops, so this run is a run of the parser from its entry.
In general, a counterexample of a cut program is a run of the cut
program: when it fails inside or after a cut loop, it starts that loop
from a havocked state, not from a state the parser reaches. It shows that
the invariant and the code disagree. Turning it into a concrete payload
is a separate step.

Unlike the Stellar and NKI jobs, this one leaves real memory work to the
solver: 12,563 constraints, and most of the run's time is in the solver.

## The proofs

| jobs | payloads | result | time when established |
|---|---|---|---|
| `txn-pf-legacy-spec`, `-spec-nonstrict` | legacy and V0 | `unsat` | 653 s, 570 s |
| `txn-pf-legacy-reads` | legacy and V0 | `unsat` | 838 s |
| `txn-pf-legacy-writes` | legacy and V0 | not solved within 900 s | |
| `txn-pf-v1-spec`, `-spec-nonstrict` | V1 | `unsat` | 71 s, 116 s |
| `txn-pf-v1-reads`, `txn-pf-v1-writes` | V1 | `unsat` | 60 s, 31 s |

Each proof has reach jobs and mutants beside it in the manifest
([Proof controls](../guide/proof-controls.md)).

## A constant the proof explained

The footprint claim, that the descriptor fits in `FD_TXN_MAX_SZ` = 854
bytes, needs a lower bound on the bytes each address table consumes. The
proof uses 35 bytes: 32 for the address, one for each of two compact-u16
counts, and at least one more because the parser rejects a table with no
entries
(`1 <= writable_cnt + readonly_cnt`). That is one byte more than the
parser's own `MIN_ADDR_LUT_SIZE` of 34. With 34 bytes per table, a
1232-byte payload fits 64 instructions and 25 tables, and their
descriptor needs 862 bytes.

A mutant removes the check that a table has an entry. With the writes
contract asserted and 34 bytes per table (`txn-pf-legacy-mut-alt-empty-w34`),
the mutant gives a counterexample: table 25 is written past the buffer.
So 854 is a tight bound, and it holds because of a check whose role in the
bound the code does not state.

The mutant also shows a pitfall of proof-mode mutants. In
`txn-pf-legacy-mut-alt-empty-inv34`, the job asserts only the loop
invariants, not the claims or the writes contract, and the answer is
`unsat`. A mutant
that is checked against the wrong obligation, or whose job assumes the
property it breaks, is not caught
([Proof controls](../guide/proof-controls.md#mutants-and-reverts)).
