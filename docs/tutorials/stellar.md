# Stellar: verifying ledger updates and transaction accounting

Stellar can execute transaction clusters in parallel and let one account
pay fees for another. Our verification studies check the rules behind
those features: ledger commits must preserve requested lifetime
extensions, and the transaction queue must keep fee reservations and
capacity counts consistent as transactions enter and leave. After a ledger
closes, the queue must also retire transactions that have become obsolete
while preserving reservations for the transactions that remain.

These rules span several components. We use Sea-MLIR to check their
combined behavior across ranges of transaction and ledger states,
building repeatable QA checks that can protect the same rules as the
implementation evolves.

| Study | What we verify | Code under verification |
|---|---|---|
| [Ledger lifetime extensions](stellar-ttl.md) | Committing parallel results preserves the largest requested extension and the associated entry metadata. | A C model of Stellar's merge and scope operations. |
| [Transaction-queue accounting and retirement](stellar-txqueue.md) | Fee reservations follow the payer; applied batches retire obsolete transactions while preserving remaining reservations, membership and resource counts. | Original C++ method bodies in a specialized harness, with an invariant for arbitrary finite applied batches in one source/payer profile. |

## Establish the ledger contract in a C model

Several transaction clusters can request longer lifetimes for the same
ledger entry. The final entry must retain the largest extension,
regardless of commit order. That rule protects the continued availability
of contract data while allowing clusters to execute independently.

We modeled the relevant merge and scope operations in C. This gave us a
focused setting in which to state the contract, identify its caller
assumptions and check symbolic entry values and commit orders. The
model of the fixed code passes the recorded bounded checks; reverting a
historical Stellar fix reproduces the expected failure.

**This checks a ledger guarantee across the merge and its caller.**
The check covers the committed entry and its metadata, with a known
regression demonstrating its ability to detect changes that violate the
contract. The [ledger study](stellar-ttl.md) develops that contract.

## Carry verification into the original C++

The second study takes the next step: keep Stellar's implementation
and specialize its environment. We extract unchanged C++ method bodies
from the transaction queue, limiter, pricing and resource code. A harness
supplies selected accounts, transactions and service responses so that
the checks can follow an operation through those components.

For example, a replacement can change a transaction's fee payer. The
queue must release the old payer's reservation, reserve the new payer's
fee, and update membership and capacity together. We check these
relationships through eight source/payer profiles and several admission
and removal paths. That first, bounded Classic-transaction scenario passed
all 26 symbolic jobs and detected six deliberately introduced accounting
regressions.

We then extended the removal check to arbitrary finite batches. An invariant
summarizes the other queued transactions while the original C++ loop processes
an applied request. In the selected source/payer profile, 18 proof obligations
establish both correct retirement and preservation of fee reservations,
resources and membership. Ten completion witnesses validate the case premises;
seven deliberate regressions test the gate's sensitivity.

**The batch proof covers a production C++ loop for any finite number of
applied requests in the selected profile.** Each run extracts the selected
bodies afresh. The invariant and explicit environment contracts make the
guarantee repeatable as the application changes.
The [queue study](stellar-txqueue.md) explains how we built and tested it.

Agents prepared the C model and C++ extraction, built harnesses, split proof
obligations, ran the checks and investigated difficult cases. Human review shaped the
properties and environment models. Sea-MLIR supplied the mechanical
verdicts and concrete replays. Together, these studies demonstrate a
progression from a focused ledger contract to verification over the
original C++ across cooperating components and arbitrary finite applied
batches in a specialized environment.
