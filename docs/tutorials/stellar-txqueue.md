# Stellar: retiring transactions and keeping fees with the right payer

A queue can contain Alice's transaction while Bob pays its fee. A
replacement may keep Alice as the source and name Carol as the payer.
The queue must release Bob's reservation and record Carol's. The source
account stays the same; the fee obligation moves.

Stellar also tracks which transactions are pending and how much capacity
they use. Those records must change together when a transaction is
replaced, evicted or removed after application. A stale reservation can
make the next admission decision use the wrong total.

When a ledger closes, the queue must also retire transactions whose source
and sequence number make them obsolete. Their fees and capacity must be
released, while reservations for other transactions remain intact.

We built regression checks around the unchanged C++ bodies that perform
these updates across the queue, limiter and surge-pricing components. We
started with bounded admission and replacement checks, then used an invariant
to verify applied batches of arbitrary finite length in one source/payer
profile. That batch gate passes 17 proofs and nine validated completion
checks, and detects six deliberate regressions. The original loop runs in an
environment that summarizes background transactions without storing or
ordering them.

This is one of the [Stellar stories](stellar.md). The
[lifetime-extension story](stellar-ttl.md) reproduces a historical merge
bug in a C port. This story follows a larger source boundary directly
through C++ and develops a candidate gate for future changes.

## One replacement updates several records

Suppose Bob backs a pending transaction with a fee of 200. A replacement
from the same source names Carol as payer and offers 4000. The queue's
local reservations should change as follows:

| Payer | Before replacement | After replacement |
|---|---:|---:|
| Bob | 200 | 0 |
| Carol | 0 | 4000 |

The queue must also replace the pending transaction and update the
resources it accounts for. Eviction adds another dependency: admitting a
higher-fee transaction can remove a different transaction, whose payer
may be shared with the incoming transaction or with either source account.
A removal callback must release that transaction's fee and capacity too.

These are local queue reservations. The study ends before ledger
application and the actual payment of fees. Its question is whether the
queue's records agree after each modeled operation.

## Retire the transactions a ledger has made obsolete

A sequence number orders transactions from one source account. Suppose the
queue holds Alice's transaction at sequence 41, sponsored by Bob. If a ledger
applies Alice's sequence 42, the queued transaction has become obsolete even
if its hash differs from the applied transaction's hash. The queue must retire
it and release Bob's reservation and the transaction's capacity.

Stellar states this rule in
[`TransactionQueue::removeApplied`](https://github.com/stellar/stellar-core/blob/a1f4383b588f56c01c72fe3318eb9d87fe09053e/src/herder/TransactionQueue.cpp#L754-L816).
After looking up the source, its retirement decision uses sequence ordering:

```cpp
transaction->mTx->getSeqNum() <= appliedTx->getSeqNum()
```

| Queued sequence | Applied sequence from the same source | Queue action |
|---|---|---|
| 41 | 41 | Retire the queued transaction. |
| 41 | 42 | Retire it even when the hashes differ. |
| 41 | 40 | Retain the newer queued transaction. |

A request for another source must leave Alice's transaction alone. If Bob
also sponsors other transactions, removing Alice's must preserve their
reservations. These requirements connect the retirement decision to the
accounting that follows it.

## Keep the accounting decisions in Stellar

The checks call the production methods that admit and remove transactions.
The selected code includes the queue, its limiter, surge-pricing decisions
and integer resource arithmetic. Source extraction copies the method
bodies from the selected Stellar checkout each time the gate runs.

For example, admission charges the fee-source account with this code in
[`TransactionQueue::tryAdd`](https://github.com/stellar/stellar-core/blob/a1f4383b588f56c01c72fe3318eb9d87fe09053e/src/herder/TransactionQueue.cpp#L650-L724):

```cpp
    auto& thisAccountState = mAccountStates[tx->getFeeSourceID()];
    thisAccountState.mTotalFees += tx->getFullFee();
```

The surrounding environment supplies the accounts and transactions;
Stellar chooses the account and performs the update. The same applies to
removal. Production code releases fees, removes known hashes and updates
the limiter through its ordinary calls and callbacks.

The checks compare those effects through independent observations. A
hash insertion or erasure records a membership change. An assertion then
compares the selected payer's actual reservation with the fees associated
with those events. The observation code never performs the reservation
update for Stellar.

## Select the individuals that matter

The accounting question needs a resident transaction, an incoming
transaction, and the accounts that can be their sources or payers. We
allocate actual account-state objects for these individuals. A relevant
lookup returns the chosen object's address or reports absence. Indexing
returns its actual state reference, so the production assignment above
writes the memory that the property checks.

The source and payer identities can coincide. We check self-payment,
shared sponsorship, a change of payer, crossed source/payer identities,
and a selected payer unrelated to either transaction. Those relationships
exercise different ways to replace, retain or remove an account record.

Our early container models retained too much machinery. A small backing
store still made lookup, sorting and buffer management part of the
verification problem. The accounting claim needed a few individuals and
consistent observations of insertions and removals. We replaced the
container implementations with that narrower interface.

The priority interface exposes the selected transaction when pricing
needs a candidate. It does not sort a collection. The vector interface
exposes one resource or lane value with ordinary value-copy behavior.
The ledger supplies a balance and validation results. The queue's
admission and accounting decisions remain in the extracted bodies.

## Summarize the background instead of storing it

The initial admission checks used a resident and an incoming transaction.
For the batch proof, we keep a selected transaction T, its source S and its
payer P as explicit individuals, and summarize the other transactions by
their reserved fees and operations. We prove one profile: S differs from P,
and P pays T's fee. The anonymous transaction encountered by an applied
request can share P, share S, pay for itself or have another payer.

The selected individuals retain their addresses and state across a proof
step. Anonymous account views are refreshed at each loop cut; a new view
forgets which earlier anonymous individual it might have represented. A
lookup for P or S still returns the selected account's actual address. There
is no backing collection to sort or permute.

For example, Bob may reserve 200 for T and an arbitrary additional amount
for other transactions. Removing an anonymous transaction with a fee of 70
must refund exactly 70. T's 200 and any remaining background reservations
must survive. The property needs those relationships, rather than the names
or order of all the other transactions.

An invariant connects these summaries to the queue's actual fee counters,
resource counters, account payloads and membership observations. The harness
checks the initial state, then considers an arbitrary loop-head state that
satisfies the invariant. The original C++ body handles an applied request.
The next checkpoint checks both the required retirement and preservation of
the invariant. This proves the step for a batch of any finite length in the
modeled environment. One symbolic iteration is the induction step, not a
one-item batch limit.

## Require the removal, as well as correct accounting

Mutation controls helped us strengthen the specification. We deliberately
changed Stellar's `<=` comparison to `==`. Our earlier selected-removal
obligation still proved: a later applied sequence could leave the queue
untouched, preserving every conservation equation. Conservation described
what happened to removed transactions but did not require the right
transaction to be removed.

We added an expectation before the body: which observed transactions must
remain after this request? The postcondition checks that outcome alongside
accounting. The deliberate `==` edit then fails with a concrete replay: the
applied sequence is later, T must be absent, and T is still live. This was a
gap in our specification; the production comparison was already correct.

The strengthened gate detects four comparison mistakes: exact sequence
matching, a strict comparison, hash-only matching, and removing a newer
transaction. It also detects clearing a payer's entire reservation and
omitting a refund. Each counterexample must replay at the intended functional
assertion. A failure in a mock's interface guard does not count as detecting
the regression.

## Investigate the cost before shrinking the claim

One case in the initial bounded suite reached the solver's 120-second limit:
the source and payer were the same account. A corresponding case with a separate sponsor
proved. We kept the inputs fixed and inspected the memory dependencies.
The slow case even had a slightly smaller expression graph.

The iterator interface exposed work the proof did not need. Raw pointers
advertised random access to the C++ standard library, which generated
paths that examined several elements at once. Our environment supplied
zero or one element. A forward iterator could express exactly the
traversal that the production callers used.

With that interface change, the self-paying case completed in about
21 seconds. The input bounds and Stellar method bodies stayed fixed.
The change also resolved the initial removal-case timeouts. The
[encoding details below](#why-the-iterator-changed-the-memory-encoding)
explain which memory assumptions and conversions changed.

This was a useful division of work. The agent extracted code, constructed
and ran the harness, and collected evidence. Human review shaped the
scope and environment, rejected unnecessary container behavior, and
directed the investigation toward memory dependencies. The solver
checked the result.

The batch study added a different challenge: a combined proof reached its
solver limit even after separating the selected objects improved memory
precision. We split the proof by queue behavior: no removal, selected
removal, or anonymous removal, then by the anonymous payer's relationship to
the selected accounts. These cases cover the original domain.

We also proved retirement and accounting as independent postconditions from
the same invariant. Neither proof assumes the other's result. Their
conjunction gives the required guarantee, while the separate retirement
queries take only 3.5–9.5 seconds. The full batch run takes about 6.2 minutes
with two jobs. Its slowest accounting query still takes 107 seconds of a
120-second budget, so runtime margin remains work for a dependable CI gate.

## Build a gate that can notice a regression

The initial suite checks eight source/payer relationships through
admission, admission followed by banning the incoming transaction, and
admission followed by removing an applied transaction. It also checks
initialization through production admission and an unrelated payer's
write history.

Concrete executions reach successful admission, rejection, replacement
and eviction. Deliberate source edits test whether the checks can notice
accounting mistakes:

| Change introduced by a control | Consequence the gate detects |
|---|---|
| Charge the source instead of the payer, or omit a charge/refund | The selected payer's reservation disagrees with membership events. |
| Omit known-hash erasure | Reservation changes and source payloads disagree with observed membership. |
| Omit limiter removal or resource subtraction | The limiter retains resources that should have been released. |

Every symbolic property and concrete control in the recorded suite
matched its expected result. This establishes a gate for the stated
scenario. The mutations are sensitivity checks; the study did not
establish a newly discovered Stellar defect.

Each gate invocation extracts from the chosen checkout, compiles the
C++ harness and checks it again. That connects the result to changing
application code. The environment contracts still need review when the
code's dependencies change. The batch extension answers the next scaling
question: symbolic summaries and an invariant cover background transactions
and arbitrary finite applied batches while retaining only selected
individuals explicitly. The admission suite and batch proof have distinct
domains; the latter establishes the stronger loop result for one profile.

---

## For verification developers

The remaining sections define the claims, input domain, evidence and
encoding, starting with the initial bounded suite and then the
[batch extension](#batch-model-and-induction-contract). The study uses stellar-core
`a1f4383b588f56c01c72fe3318eb9d87fe09053e`. The snippets above are production
code; the specifications below describe the harness contracts rather than
another implementation of admission or removal.

### Claims and independent observations

Choose a payer P and check its reservation delta:

```text
reservation_after(P) = reservation_before(P)
                      + inserted_fees(P) - erased_fees(P)
```

Known-hash indexing and erasure record the inserted and removed
transactions. Those callbacks update observation state, independently of
`AccountState::mTotalFees`. The harness also requires:

- Successful admission inserts the incoming hash.
- Source transaction payloads agree with observed hash membership.
- Limiter operation totals equal initial resources plus inserted
  resources minus removed resources, and stay within capacity.
- An erased account has neither a pending transaction nor reserved fees.

Logical erasure changes lookup presence. The physical account node stays
allocated for the unit. Object handles preserve identities and casts;
reference counting and deallocation are outside the model.

A separate frame profile selects an unrelated allocated payer. It enables
tracking, resets modification history, executes the operation and checks
`!sea_is_modified(node)`. The check concerns the whole allocated node;
it is not a field-specific frame theorem. A control writes its age field
and must fail the frame check.

### Symbolic domain and loop scope

Most jobs start with one coherent resident transaction. The harness
initializes its account payload and reservation as a precondition;
production limiter reset/add code establishes its resource counts. A
separate job establishes the resident through production `tryAdd`.

| Input | Domain |
|---|---|
| Transaction family | Classic fee bumps; full fee equals inclusion fee |
| Resident transaction | Two operations, sequence 1, fee 1–1024 |
| Incoming transaction | Two or three operations, sequence 0–2, fee 0–65535 |
| Available balance | 0–65535, constrained to be at least the resident fee |
| Queue capacity | Two through four operations; pool multiplier one |
| Validation responses | Four independent symbolic Booleans |
| Transaction identities | Distinct fixed hash tokens |

The eight identity profiles are below. Equal letters denote the same
account; P is distinct from A and B when shown separately.

| Profile | Old (source, payer) | New (source, payer) | Selected payer |
|---|---|---|---|
| 0 | (A, A) | (A, A) | A |
| 1 | (A, P) | (A, P) | P |
| 2 | (A, P) | (A, B) | P |
| 3 | (A, B) | (A, P) | P |
| 4 | (A, P) | (B, P) | P |
| 5 | (A, B) | (B, A) | A |
| 6 | (A, A) | (B, B) | Unrelated P |
| 7 | (A, B) | (B, A) | B |

These profiles do not enumerate every partition of arbitrary identities.
The environment has one Classic resource dimension and one lane, at most
one eviction candidate and one supplied removal item. Older ban epochs
are empty. The unroll bound is two: constructor metric loops have two
iterations, and the relevant resource, lane, batch and candidate loops
have at most one. Those bounds describe the initial suite. The batch proof
below replaces the applied-input traversal with an inductive iterator
contract and summarizes background state.

### Extraction and environment contracts

The extraction boundary retains 2218 implementation lines from
`TransactionQueue`, `TxQueueLimiter`, `SurgePricingUtils` and `TxResource`,
plus 342 numeric-helper lines. These counts include comments and blanks;
they are not verified-LOC or source-coverage measurements. Some retained
methods are outside the exercised entry points.

Extraction omits methods for network flooding policy and floating-point
configuration scaling. Every retained method body is copied unchanged.
The harness models surrounding types and APIs, including the omitted
scaling operation. In particular:

- Every lane-configuration receiver is the actual
  `DexLimitingLaneConfig` created by the Classic limiter. A declaration
  model resolves its interface calls by that concrete type. This is a
  closed-type premise, not a claim about arbitrary C++ dynamic dispatch.
- With pool multiplier one, configuration scaling returns the supplied
  limit. Queue/resource accounting uses the integer production bodies;
  the study makes no deep floating-point claim.
- Account and hash interfaces preserve the required identities, repeated
  lookups and membership events. Persistent traversal needs a separate
  contract. Pricing iteration exposes at most one candidate.
- Ledger validation supplies the symbolic responses above. Metrics and
  timers have no effect on the accounting property. Cryptography, XDR
  construction, concurrent execution and general exception unwinding
  are outside the check.

SeaM uses the explicit A-INB premise that accesses stay within their live
objects. The study does not discharge that premise universally or prove
ownership and memory safety. The production-admission seed job completed
after SeaM fell back without A-INB; the other 25 symbolic jobs used it.

### Proof jobs, completion witnesses and mutations

The recorded matrix has 26 symbolic UNSAT results and 37 concrete
witnesses. The 26 jobs instantiate the accounting and frame claims across
the profiles; they are not 26 independent application requirements.

| Job family | Count | Expected result |
|---|---:|---|
| Eight aliases × admission, ban, applied removal | 24 | UNSAT |
| Seed through production admission; unrelated-payer frame | 2 | UNSAT |
| Completion with all original checks enabled | 26 | Reach the dedicated final assertion |
| Admission, rejection, replacement, eviction reaches | 4 | Reach the intended branch assertion |
| Six accounting mutations; shadow-write control | 7 | Fail an assertion |

A completion witness preserves every original property and model check.
After them, a fresh Boolean controls a dedicated failing assertion. The
interpreter must reach that exact location. An earlier failure does not
count as completion. This supplies non-vacuity evidence without another
solver search.

Concrete witnesses are executions of the extracted model, not symbolic
proofs. The CI driver rejects interpreter errors and memory errors.
For symbolic jobs it requires UNSAT with a successful exit; unknown,
timeout and export failures make the gate fail. The export admitted no
unapproved external no-op calls.

The recorded snapshot used Clang 18.1.8, SeaHorn `2e9eed07a322` with the
[indirect-predicate preprocessing fix](https://github.com/seahorn/seahorn/pull/615),
and sea-vc `v0.1.0-84-g0d842ebcd891`. With two CI jobs allowed concurrently,
solver times were 9.5–45.4 seconds and proof-job wall times were
12.7–54.1 seconds on the development machine. These are single-run
observations, not runtime guarantees. Full run logs and generated programs
are not bundled with this tutorial.

### Why the iterator changed the memory encoding

The raw-pointer version led STL random-access algorithms to emit paths
with four-element reads over a one-element representative. SeaM found
84 statically out-of-object accesses and fell back from A-INB. The VC
also retained 48 opaque pointer reconstructions.

A zero/one forward observation removed those accesses and conversions.
The caller-visible traversal needed by this profile stayed the same.
The self-paying proof then completed in 21.05 seconds; its separate-sponsor
counterpart completed in 19.25 seconds in the isolated trial. The earlier
self-paying run had reached the 120-second solver limit.

The new result explicitly relies on A-INB; the earlier representation
triggered a fallback that disabled it. The change kept the Stellar
bodies and input bounds fixed, while restoring the intended memory
reasoning. Expression-graph size alone did not explain the cost: the
self-paying graph had been slightly smaller than the sponsor graph.

### Use concrete coverage before more solver queries

The tools' `--agent` guides describe concrete-first reach exploration.
For a regenerated symbolic job, a command of this form runs a seed corpus
and fuzz trials, leaving the remaining targets unqueried:

```sh
sea-vc queue-admission.mlir --lower --seam --seam-wide --widen-memory=4 \
  --reach=blocks --reach-corpus=seeds --reach-fuzz=1000 \
  --reach-query=false --reach-witness=witnesses --format=jsonl
```

Here `queue-admission.mlir` and `seeds` stand for the regenerated job and
its input corpus; this page does not distribute those artifacts. The
recorded exploration used nine seeds and 1000 fuzz trials. It reached
210 of 597 lowered blocks with zero solver queries and zero reported
A-INB-violating runs. The concrete-run phase took about 0.8 seconds.
The remaining 387 targets were unknown, not unreachable.

These are model/block coverage counts. The lowered program includes
inlined library code and failure paths, so the ratio is not a percentage
of Stellar source lines. Source locations on the reached paths include
fee charging, eviction callbacks, limiter addition, hash insertion and
fee release.

The C++ library also emits pointer/integer casts. Concrete execution
therefore uses the placement pipeline documented in `sea-run --agent`.
An unsupported cast on an unplaced program is an interpreter limit, not
a successful witness. The useful change in this study was a narrower
iterator contract that preserved the traversal the accounting checks needed.

### Batch model and induction contract

The batch extension uses the same source revision and production
`removeApplied`, `dropTransaction`, fee-release, limiter and resource bodies.
Its abstract state has selected transaction T, source S and payer P, plus
aggregate background fees and operations. The proved profile has S distinct
from P and T paid by P. P has no separate pending source transaction; it may
still sponsor arbitrary background obligations within the numeric domain.
The proof does not extend the initial suite's eight profiles to arbitrary
batches.

The selected account objects are strong singletons: a matching lookup returns
their actual addresses and production assignments update their fields.
Anonymous source and payer roles use reusable account views. Their origin is
forgotten at a loop cut, while repeated lookups within an iteration agree.
This provides weak updates across anonymous observations without retaining
identity histories or collection permutations.

Known-hash erasure records the removed transaction and updates ghost totals.
Those observations are independent of production fee and limiter writes.
The invariant includes these relationships:

```text
reservation(P) = background_fees(P) + (T.live ? T.fee : 0)
initial_reservation(P) = reservation(P) + removed_fees(P)
initial_operations = remaining_operations + removed_operations
T.live + T.removal_count = 1
```

The corresponding source-account equations also hold. Account presence and
payloads, the selected transaction's hash observation, normal and flood
priority membership, and both resource counters must agree with the summaries.
An erased account must have no remaining payload or fee obligation. Account
objects remain allocated after logical erasure.

The input-range iterator supplies the induction contract:

1. Check the invariant after initialization.
2. Havoc mutable singleton state, aggregate totals and anonymous views;
   assume the full invariant at this arbitrary loop head.
3. Choose whether the loop exits or executes an iteration of the unchanged
   production body.
4. At iterator increment, check preservation and cut the back edge.

Immutable initial totals relate the remaining state to cumulative removals.
The zero-iteration branch checks the exit invariant. This is partial
correctness over arbitrary finite batches under the model contracts; it does
not prove termination or impose a fixed batch-length unrolling bound.

| Batch input or summary | Domain |
|---|---|
| Selected and current transaction | Classic fee bumps; full fee equals inclusion fee |
| Each transaction's fee | Positive signed 64-bit value |
| Each transaction's operations | 2–100 |
| Aggregate reserved fees | Nonnegative, with relevant totals at most the signed 64-bit maximum |
| Remaining and initial operation totals | At most the unsigned 32-bit maximum |
| Request sequence | Normalized older/equal/later cases 0–2, with queued sequence 1 |
| Request hash | Matching or differing from the selected transaction's hash |
| Anonymous payer | P, S, its own source, or another anonymous account |

Initialization constructs a coherent abstract projection of a queue. The
study does not prove every represented projection reachable through native
admission. Classic dispatch, integer resource arithmetic, object lifetime and
interface contracts remain as described above. A-INB stays assumed throughout
this batch proof; ownership, universal memory safety, concurrent execution and
generic floating-point reasoning remain outside its scope.

### Specify the required transition independently

For each observed transaction, the harness snapshots the expected liveness
before executing the body. Schematically, for an entered iteration:

```text
expected_live(T) = live_before(T)
                  AND NOT (request.source == T.source
                           AND T.sequence <= request.sequence)
```

It specifies the analogous outcome for the current anonymous transaction.
The retirement postcondition compares these expectations with the liveness
observed through hash erasure. The accounting invariant connects that
observation to actual payloads and priority membership. The expected outcome
is never assumed at the head or derived from a production erase event.

The selected-removal obligation accepted the deliberate `<=` to `==` edit
with the older accounting-only postcondition. This was one measured
obligation, not a complete run of a mutated earlier suite. The strengthened
control instead produces a validated trace with queued sequence 1, applied
sequence 2, expected T liveness false and observed T liveness true.

Retirement and accounting are proved independently over the same production
body and full head invariant. Outcome-only ghost fields are omitted from the
accounting queries and cannot constrain them. The conjunction of both
postconditions supplies the stronger guarantee. Completion queries enable
both at once.

### Cover the source cases, then split the postconditions

The source partition distinguishes these disjoint cases:

| Case | Meaning |
|---|---|
| Zero iterations | The input range exits immediately. |
| No removal | No observed pending transaction is made obsolete by the request. |
| Selected removal | The request retires T. |
| Anonymous, paid by S | The selected source account sponsors the anonymous transaction. |
| Anonymous, self-paid | Its source and payer coincide, excluding P and S. |
| Anonymous, another payer | Its payer differs from P, S and its own source. |
| Anonymous, paid by P, T gone | P retains only background reservations. |
| Anonymous, paid by P, T live | P must also retain T's reservation. |

The anonymous payer cases exclude earlier alternatives, making them
exhaustive without overlapping P or S with an anonymous role. Selected and
anonymous removal are disjoint by source identity. The no-removal case is the
complement of those two removal predicates; entering or exiting the loop
covers the remaining choice.

Initialization is proved first and reused as a lemma. Every preservation
query still starts from the full original head invariant. In the last case,
the invariant postcondition is itself split into P's actual reservation
equality and all remaining conjuncts. Those proofs are independent too;
neither assumes the other's postcondition. Unknown results at parent queries
are resolved by these exhaustive cases and conjunctive proofs, never counted
as passes.

Combining retirement and accounting left the last remainder unknown at 120
seconds. The independent retirement query for that case proves in about eight
seconds in the final run. The accounting remainder still takes about 107
seconds. Splitting proof obligations retained the complete modeled input
domain and numeric ranges.

### Reachability and memory precision guide the proof work

Two validated completion models visited 313 of 666 lowered blocks in the
unsplit batch export. A bounded `--reach=blocks` pass classified another 214
as unreachable and left 139 unknown. Targets inside the initialization check
were all classified; harder targets were spread across the loop head,
removal and preservation checks. This supported an initialization checkpoint
and source-level case analysis. It did not identify a single instruction as
the cause of the solver cost.

Those reach-corpus prefixes carried `inb=undetermined`. They are coverage
observations with that qualification, separate from the UNSAT proof results
under A-INB and the strictly validated completion checks.

Memory precision also depended on how the harness exposed the selected
objects. An earlier layout stored their pointers as fields of the batch
state. A variant constructs them in separate aligned storage and exposes
their addresses directly. Unused storage bytes are made arbitrary before
construction; the logical state domain and invariant stay fixed. Under
matching compiler flags, the recorded profile's open alias-query count fell
from 703,892 to 15,524, and VC generation fell from 60.431 to 2.883 seconds.
The unsplit proof still reached its solver limit, so this layout improvement
and the source-level proof decomposition solved different parts of the
problem. The batch result required no additional SeaHorn or Sea-MLIR source
changes.

### Batch results and regression controls

The recorded batch family passes all 26 checks, plus six deliberate
regression controls:

| Obligation or control | Count | Required result |
|---|---:|---|
| Initialization | 1 | UNSAT |
| Invariant preservation and zero-iteration exit | 9 | UNSAT |
| Retirement for each entered source case | 7 | UNSAT |
| Initialization and case completion | 9 | SAT at the intended marker, validated with zero mismatches |
| Deliberate comparison and fee regressions | 6 | SAT at the intended functional assertion, validated with zero mismatches |

The batch controls are distinct from the initial admission suite's controls:

| Deliberate production edit | Behavior detected | Solver seconds |
|---|---|---:|
| Replace `<=` with `==` | An obsolete transaction survives a later applied sequence. | 9.841 |
| Replace `<=` with `<` | The transaction survives an equal applied sequence. | 9.232 |
| Match full hashes instead of sequence ordering | An obsolete transaction survives when hashes differ. | 6.372 |
| Replace `<=` with `>=` | An earlier applied sequence removes a newer transaction. | 11.135 |
| Clear the payer's entire reservation during refund | Reservations for other transactions are lost. | 8.654 |
| Omit a refund | The retired transaction's reservation remains. | 8.476 |

The `>=` control's trace removes the current anonymous transaction while T is
already absent. Its expected liveness is true and its observed liveness
becomes false. The controls therefore exercise both selected singleton and
weak individual observations. A control must change its copied production
statement and fail at the intended functional assertion; an earlier model
guard, export error, timeout or unvalidated replay is rejected.

The snapshot used Clang 18 and sea-vc `v0.1.0-84-g0d842ebcd891`. With two jobs,
the positive family took approximately 372 seconds, including compilation,
export and replay. The seven added retirement proofs took 3.542–9.486 seconds
each. The slowest accounting proof took 107.310 seconds under a 120-second
solver budget; each query had an 8 GB memory cap. Mutation queries used
60-second budgets. These are development-machine observations, not CI runtime
guarantees. The accounting bottleneck still needs more margin.

This establishes a repeatable gate for the stated profile and contracts.
Further source/payer profiles, review of the environment assumptions, and
convenient CI integration remain work. The tutorial reports the method,
properties and recorded results; it does not bundle the harness distribution
or large run artifacts. No new Stellar defect is claimed by these deliberate
regression controls.
