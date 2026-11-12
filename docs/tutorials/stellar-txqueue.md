# Stellar: keeping fee reservations with the right payer

A queue can contain Alice's transaction while Bob pays its fee. A
replacement may keep Alice as the source and name Carol as the payer.
The queue must release Bob's reservation and record Carol's. The source
account stays the same; the fee obligation moves.

Stellar also tracks which transactions are pending and how much capacity
they use. Those records must change together when a transaction is
replaced, evicted or removed after application. A stale reservation can
make the next admission decision use the wrong total.

We built regression checks around the unchanged C++ bodies that perform
these updates across the queue, limiter and surge-pricing components.
Within a bounded Classic-transaction scenario, every
recorded accounting check passed, and deliberate edits that broke the
accounting were detected. The work was to choose an environment that
preserved the relationships the code needs without rebuilding the node
around it.

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

## Investigate the cost before shrinking the claim

One case initially reached the solver's 120-second limit: the source and
payer were the same account. A corresponding case with a separate sponsor
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

## Build a gate that can notice a regression

The recorded suite checks eight source/payer relationships through
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
code's dependencies change. The next scaling question is whether
symbolic summaries and an invariant can cover arbitrary other queued
transactions while keeping only the selected individuals explicit.

---

## For verification developers

The remaining sections define the claims, input domain, evidence and
encoding. The study uses stellar-core
`a1f4383b588f56c01c72fe3318eb9d87fe09053e`. The snippets above are production
code; the specification below describes the harness rather than another
implementation of admission.

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
have at most one. No arbitrary queue-prefix invariant is established.

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
