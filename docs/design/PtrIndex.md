# sea.ptr.index: pointer arithmetic as pointer arithmetic

Status: stages 1, 2, 3a, 3b-i, 3b-ii step (a), and the Ackermann
part of stage 4 are implemented (stage 1: the op; 2: the exporter;
3a: the passes; 3b-i: the Expr operators and expander; 3b-ii (a):
section 0a; stage 4: section 0b).

The design keeps rule-made address arithmetic as nodes, makes view() return
an integer OFFSET term, and preserves flags on constant-only inbounds GEPs.
The pointer sort in Expr (PtrSort.md) refines this representation's typing.

## 0. Rev 6: PTR_INDEX is the only pointer-arithmetic constructor

In the bit-vector representation, `mk<BADD>(node, 8)` type-checks because
the node has sort bv(n), but it hides the pointer arithmetic. Therefore:

- The VC (OpSem) encodes **every** pointer / address addition as a
  PTR_INDEX node, never as a bvadd:
  - `ptr.add %p, %o` / `addr.add %a, %o` with a symbolic o: `PTR_INDEX(p,
    o, 1, 0, 0)` (one index, stride 1, no flags);
  - with a numeral o: `PTR_INDEX(p, c = o, K = 0)` with no index;
  - a constant-only inbounds GEP: `PTR_INDEX_NUSW(p, c, 0)` with no index
    (A5). The op allows `sea.ptr.index %p [] offset c inbounds`, and the
    exporter emits it (section 7), so `&t->count` keeps its promise.
    Without flags, a constant-only GEP may stay `ptr.add`: the VC makes
    the same unflagged node.
  - `ptr.index`: as in section 2, with its operator.
- **Composition only between unflagged nodes** (merging: K adds, K1 + K2,
  both 0 in practice since OpSem writes 0 and the expanders run after
  rewriting; c is reduced mod 2^n: an unflagged node has no range). A PTR_INDEX built on a
  PTR_INDEX base merges into it (indices appended, constants added) only
  when both are unflagged. A flagged inner node always stays the base of
  the outer one: its promise is about its own base + offset, and adding to
  its c would claim P2 / P3 for an address the front end never promised
  (a `ptr.add` after an inbounds GEP is a new, unpromised computation).
  Nesting is read by view() and the stride test as in section 2 (an
  integer-level chain, each node's own clause). The expanders add K into
  the node they expand, never `bvadd`.
- **Normal form changes.** The m = 0 restriction of rev 5 goes: a
  constant-only node `PTR_INDEX(p, c, K)` is legal, so that `&s->f` and
  `p + 8` are pointer arithmetic too. The rules that matched
  `bvadd(base, numeral)` (numeral-offset compares, smap.new, ARM, W-*) are
  unchanged, because view() of a constant-only node over a base with a
  numeral offset returns that numeral (A4; section 2). `ptrindex.fold` keeps
  a node with no index left (rev 5 made it `bvadd`).
- **Rule-made address arithmetic builds nodes too (A3).** SeaM's rules and the boundary build new addresses
  today by bvadd / bsub. The ones that produce an address build a node
  instead: an unflagged one, except the K writers (`splitRead`,
  `expandWrite`, InitialWords' `plus`), which keep the operator and write K
  (section 4):
  - `ptrAt` (store-map entries, SeaMRules.hh:1968): `PTR_INDEX(base, c)`;
  - `splitRead` (:912) and `expandWrite` (:926): the byte k of an access
    at a is `a{K+k}` (section 4);
  - the copy translation `s + (j - d)` (:1153): `PTR_INDEX(s, [j - d] * 1)`;
  - the boundary's copy read equations: `select(Ms, s + k)` (MemOps.hh:80)
    and MemOpTranslator's `src = s + (j - d)` (:457-460). They run in
    memoryTerm, before InitialWords / Ackermann, so LinearForms sees them
    as: `PTR_INDEX(s, [k] * 1)`;
  - `sub.const` (:1768-1769), when `a` is ptrish: the node form by the
    merge rule (unflagged: c - v merged; flagged: a new unflagged node over
    it). Otherwise it is bvadd as today;
  - StoreMap expansion (StoreMap.hh:225), xrt (ReadTable.hh:391),
    InitialWords' `plus` (:292).
  The offset-valued ones stay integer `bsub` / OFFSET terms (section 2):
  `sub.same-base`'s `off1 - off2`, `sub.ite-same-base` (:1790-1793,
  pointer differences resolved to `off1 - off2`), the memop range
  test `BSUB(idx, d)`
  (:2487), W-BYTE-IN's `k = j - i` (:981) and the A-INB leaf conditions
  (:786, :1273). So a raw bvadd on an address comes only from the expander
  at the boundary or from the integer side (`ptr.to_int` / `addr.to_int`,
  arithmetic, `from_int`). view()'s bvadd path stays as a fallback for
  those.
- **Consequence for stage 3b.** OpSem encodes ptr.add / addr.add / segment
  bases with numeral offsets through the node, so every SeaM consumer of
  addresses must handle PTR_INDEX from the start (view, ptrish,
  add.normalize, LinearForms, LeafDecider, AddrRangeMap, InitialWords,
  xrt, accessSpans / accessAligns, inBounds / rangeInBounds, the store-map
  and ARM paths). The gate of 3b-ii stays: verdicts, Ackermann reads and
  constraints identical on every corpus, with the stride rule off.
- **Section 4 (who writes K) simplifies.** K is still written only by the
  expanders, but `bvadd(node, k)` no longer has to be classified: nothing
  builds it.

## 0a. Stage 3b-ii step (a), as implemented

[History] Superseded by the pointer sort (PtrSort.md rev 5): nodes are
built in every VC (also without --seam, S2g), addr.add is a node too, and
the deciders read nodes structurally (S2a). Kept as the record of the step.

- The VC lowering no longer runs sea-lower-ptr-index (the pass stays, for
  standalone use and check-semantics). OpSem encodes `sea.ptr.index`:
  - without --seam: the lowering's arithmetic, the same term as before;
  - with --seam: a PTR_INDEX node over the sign-extended indices, K = 0;
    inbounds / nusw -> NUSW, else nuw -> NUW. Both flags give NUSW: [OPEN]
    an operator with both promises (stage 4 decides whether it needs one).
- ptr.add / addr.add stay bvadd (step (b) changes that).
- **Where a node lives.** A node stays only at address positions:
  - the index of select / store / wselect / wstore;
  - the address arguments of mcopy / mset / mhavoc;
  - an ite branch;
  - the base of a node.
  In any other position (compares, arithmetic, stored values) the rewriter
  replaces it by its **flat form** (SeaMRules.hh `flat`, counted
  `ptrindex.flat`).
- **The flat form** is the old lowering's term after the rewriter: the
  sorted sum of the base's summands, x_k * s_k normalized as bv.ac does,
  and the numeral c + K. This is the adapter called
  `addressSummands`.
- **Deciders read the old term.** The deciders read their arguments
  through `old(e)`: every node, at any depth, replaced by its flat form.
  This is the boundary's expansion, with a shared memo. The deciders are
  view, decidePair, decideEq, wideRelation, offsetDiff, congruentStarts,
  ARM and inBounds. The equalities wow.collapse and ite.same compare old
  terms (`sameAddr`).
  - They return decisions, not terms, so no node is lost.
  - Deep matters. A read at a node and a read of the same cell through
    ptr.add are two select terms, so a shallow flat form left value
    compares undecided (vcc byte_cursor_read).
  - A node's flat form flattens the nodes in an ite base's leaves too.
    Without that, ARM saw an unknown summand and one A-INB condition was
    lost (vcc byte_cursor_advance; test/VC/seam/ptr-index-ite-base.mlir).
- **Rules that build terms keep the node.** A row miss keeps its index;
  splitRead's `j + k` is flattened by the add rule.
- **The node rule.**
  - `ptrindex.fold`, with the flag-range check.
  - An unflagged node with no index left is its flat form.
  - `ptrindex.pull-ite` fires exactly when add.pull-ite would on the flat
    form, through the same ite.
    - It is node-preserving when that ite is the base or a stride-1 index
      (parity with the flat form).
    - Otherwise it pulls the flat form (`ptrindex.pull-ite-flat`).
- **Boundary.** `SeaMRewriter::expandPtrIndex` replaces every node by its
  flat form first, in solverTerm, solverTermWords and the xrt path. sea-vc
  refuses a node at the solver (`vc.ptr-index-at-boundary`).
- **Gate by construction.** Every rule decides on the old terms, and the
  solver gets the old terms. So the gate holds by construction. Measured
  against the lowering (HEAD 2ad21f5) on vcc-pi (228 jobs, seam and
  seam-wide), stricter than the gate:
  - 438 + 5 of 456 queries are byte-identical;
  - 4 differ only in argument order (sum order by term id);
  - 9 differ in text and are equivalent (z3: old != new unsat);
  - every A-INB condition list is identical, or for the 5 hash_table runs
    the counts are.
  - OpSem keeps the lowering's arithmetic alive, so its terms keep their
    ids and the sum order (by id) is the old one.
- **Deviation from section 3: the expansion runs before Ackermann, not
  after it.**
  - So Ackermann, InitialWords, xrt and the models see no node, and the
    LinearForms / LeafDecider node cases are not needed yet.
  - Moving the expansion after Ackermann, with those cases and the per-Read
    node record, is the first part of stage 4: the first step that reads
    nodes there.

## 0b. Stage 4, the Ackermann stride rule, as implemented

**Order.** Stage 4 comes before 3b-ii (b)-(d).
- The target is hash_table_eq's cross-field pairs, `&e[i].a` vs `&e[j].b`.
- Each is one GEP, which step (a) already makes a node.
- (b)-(d), the nodes for ptr.add and rule-made arithmetic, are sort-ready
  representation work. They come later.

**Flag.** `sea-vc --ack-stride` is on by default. Measured on vcc-pi
under yices and z3: verdicts identical, 0 reruns.
`--ack-stride=false` turns it off.

**Record.**
- `SeaMRewriter::solverTerm` translates store-maps, width terms and memops
  first.
- Then it records, for every select, the expanded index and the address
  the read had (`AccessOrigins`, Ackermann.hh).
- Then it expands the nodes.
- The words and xrt paths record nothing.
- The reorder (expand last) leaves every query unchanged with the rule
  off: vcc-pi as in 0a (443 byte-identical, 4 order-only, 9 z3-equivalent
  of 456).

**Eligibility.** A read is eligible iff every index term merged into it came
from a promised node. An ite whose leaves are all promised nodes (a GEP over
a phi) gives each leaf as an origin. Anything else makes the read plain.

**The test.** For each pair of origins a, b:
- same operator and the same base term;
- with D = (c_b + K_b) - (c_a + K_a), the gcd of the per-index-term stride
  differences does not divide D.

A node over an object of size 0 (null) is apart from everything: under
A-INB (cmp.size0-vacuous) no access is there. The test runs after the exact
tests (numeral, offset, residue, leaf), so `stride-pairs` counts
constraints removed.

**K writers (section 4).** With the flag (and A-INB), byte k of a wide
access at a promised node is that node with K = k:
- `splitRead`;
- the boundary's WideExpander;
- `op::ptr::withByteOffset`, which pushes k into ite leaves.

Their firings and the size-0 ones are counted `stride-a-inb-pairs` and are
off without A-INB.

**Soundness.**
- Unsat: dropping a functional-consistency constraint only weakens the
  lowered query, so unsat is sound with no premise.
- Sat: the promises (and A-INB for K and size-0) make the dropped
  constraints vacuous, so they are needed only for sat. A sat model that
  puts two dropped-pair reads at one index breaks one. It is found before
  the result (AckermannModel::strideBroken, the arrays built eagerly), and
  sea-vc reruns without the rule (`vc.ackermann-stride: fallback`).
- Eligibility avoids spurious reruns; it is not a soundness condition.
- So the Ackermann case needs no PremPtrIndex obligation. The premise and
  --seam-check formulas of section 5 are needed when the rule enters SeaM
  (W-SKIP / ROW).

**Measured (vcc-pi, yices).**
- Verdicts are identical on all 228 jobs.
- hash_table_eq (seam-wide): 95104 -> 43712 constraints; solve 54 -> 26 s.
- The plain-seam config: sea-split-wide-access makes the bytes
  `ptr.add(gep, k)`. With the flag, OpSem's addr.add of a numeral to a
  promised node (or an ite of them) is that node with K = k, k signed. The
  copy-hit source `s + k` is the same (copySource).
  - These K writers go beyond "the bytes of an access at the node".
  - That is safe here (for Ackermann lowering): in the Ackermann case a firing never
    changes the verdict, only the rerun rate (0 on vcc-pi).
- view() reads the shallow flat form. Its offset keeps the nodes of the
  loads in it, because rules build terms from it. A deep view (old())
  turned those loads into flat-address reads, which the record could not
  attribute: 42720 of hash_table_eq's pairs were not eligible. The deciders
  still read old() of their arguments.
- vcc-pi, with the rule on:

  | config | constraints | wall |
  |---|---|---|
  | seam | 107859 -> 55013 | 218 -> 175 s |
  | seam-wide | 109097 -> 56059 | 218 -> 191 s |

  hash_table_eq goes 95104 -> 43712 constraints in both. Its wall time
  under the bench is 108 -> 58 s (seam) and 97 -> 68 s (seam-wide). Rule
  off, the queries are unchanged (443 / 4 / 9 as above).

**Follow-up.**
- Additional coverage:
  - tests: not-eligible, nuw, negative stride, mixed operators;
  - --eq-audit of the dropped pairs;
  - greedier firing, with the rerun rate as the metric.
- Chains, which today count as other-base.

## 0c. R-STRIDE in SeaM (store-map R-ABORT), as implemented

The SeaM site that benefits from the stride rule is the store-map R-ABORT loop. ROW
over store chains and W-SKIP have almost nothing separable on vcc-pi.

**The rule.** `sea-vc --seam-stride` (SeaMOptions::strideRule), off by
default. In R-ABORT, the entry b + o is skipped for a read when:
- every ite leaf of the read is a promised node N = PTR_INDEX_F(b, x_k,
  s_k, c, K) directly over b, with an index;
- b + o < 2^n as integers (b the layout numeral);
- gcd(|s_k|) does not divide o - c - K.

Counted `row.smap-abort-stride` (reason `cmp.stride`).

**Premises.**
- New bit `PremPtrIndex` (= 8), counted `premise.ptr-index`. P1-P3 of N:
  front-end facts, trusted like ALIGN.
- PremLayout: the integer reading uses the layout numeral b (section 5,
  --seam-check).
- PremAInb when K != 0; with A-INB off, no such firing. A K != 0 leaf is
  accepted only for `b + size - 2^n <= K <= b` (b the layout numeral, size
  the object's; empty unless `b + size <= 2^n`). A-INB bounds the access
  address modulo 2^n only: it lies in [b, b + size). P3 gives `0 <= b + O
  < 2^n`. If `b + O + K >= 2^n` (K <= b < 2^n), the address is
  `b + O + K - 2^n`, and in [b, b + size) needs `K >= 2^n - O >= b + 1`.
  If `b + O + K < 0` (K >= b + size - 2^n > -2^n), the address is
  `b + O + K + 2^n >= K + 2^n >= b + size`. So within that range the
  integer `b + O + K` is the address and the equation `O + K = o` is
  exact. Outside it a wrap is possible: a program `ptr.add` of a numeral
  to a promised node writes K signed and unbounded (`addressPlus`, section
  0b), e.g. K = -200 or K = 4 at a node near 2^n
  (test/VC/seam/seam-stride-k-wrap*.mlir). The parent access's A-INB
  (section 5) would bound only the K of a split byte (0 <= k < units),
  and the rule cannot tell which writer wrote K.
- --seam-check adds `op::ptr::promise(N)`: the P1-P3 formula, in 2n + 8 +
  log bits, the checks of Semantics.cpp brokenPtrIndexPromise. The same
  generator will serve the paranoid pass.
- Unlike the Ackermann case, this rewrite changes the formula, so the
  premises matter for unsat.

**Verified.**
- hash_table_find2: 1440 firings, --seam-check 8186 / 8186 valid.
- Mutant (no divisibility test): 161 invalid.
- test/VC/seam/seam-stride.mlir.

**Measured: a loss, so off.** [SUPERSEDED, below: a single heuristic
trajectory, not a loss. Why I was misled: yices is deterministic, so its "3
runs" repeated one trajectory, and z3 was run at its default seed only.]
- With --ack-stride (the default), hash_table_eq: solve 28.7 s without the
  rule, about 50 s with it (yices); z3 28.8 -> 48.2 s. Ackermann
  constraints are unchanged (43712).

**Performance investigation.** A smaller VC need not solve faster.
For hash_table_eq, plain seam, z3 in-process:
- **The engine.** z3's SAT engine (bit-blasting, CDCL: `sat.stats` at
  verbose=1).
- **The query.** R-STRIDE skips 31696 store-map entries. The DAG shrinks
  only 209705 -> 206535 (ite 5170 -> 2290, = 89027 -> 88739):
  hash-consing shared most of the skipped levels. The initial CNF is
  4.37 M -> 4.25 M clauses.
- **The mechanism** (z3 verbose=2, default seed):
  - Without the rule, at conflict ~1800 (15.7 s), ~188 k unit literals are
    learned at once (fixed vars 9981 -> 197831).
  - The next `sat-cleaner` then deletes 1.13 M satisfied clauses (2.8 M ->
    1.42 M), and the search ends 6 s later.
  - With the rule, the same avalanche (to ~211 k units) comes only at
    conflict ~3970 (40.9 s). Until then each conflict runs on twice the
    clauses.
- **Seeds** (`--z3-param=sat.random_seed=N`, solve time, without -> with):

  | seed | without | with |
  |---|---|---|
  | 0 | 30.0 s | 49.0 s |
  | 1 | 40.3 s | 30.4 s |
  | 2 | 40.4 s | 36.9 s |
  | 3 | 35.0 s | 31.2 s |

  The avalanche's conflict moves with the seed both ways. So the default
  seed's gap is a trajectory effect, and the rule is not slower on z3 (if
  anything slightly faster).
- [OPEN] What the avalanche is: which literal (or few) fixes ~188 k
  variables. Next: z3 tracing on that unit, and the same seed sweep for
  yices.
- The flag stays off until that is understood.

## Why

Pointer arithmetic must remain distinct from integer arithmetic until its
explicit lowering. The ptr-to-int and int-to-ptr operations identify where
provenance is lost. Frontend GEP no-wrap flags are trusted promises;
paranoid mode can check them separately.

Today, the exporter (SeaHorn `lib/seahorn/SeaMlirExport.cc` exportGep,
:586) lowers a GEP to `sea.mul %i, stride` (no flags) plus a chain of
`sea.ptr.add`. The structure and `inbounds` are lost.

[OBS] vcc hash_table_eq under eager Ackermann: 6,264 of 11,888
undecided word pairs (50,112 of 95,104 byte pairs) are `p + 24 i + 80` vs
`p + 24 j + 96`. Modulo 2^64 they can be equal. Under the facts below,
they cannot.

## 1. The op

```
%q = sea.ptr.index %p [%i * 24, %j * -4] + 80 inbounds
       : !sea.ptr<64>, i64, i32
```

- **Base.** `%p : !sea.ptr<n>` (or `!sea.addr<n>` after placement).
- **Indices.** `%i_k` integers of width w_k <= n, sign-extended to n bits
  (GEP: sext). The verifier requires w_k < n with sext, or w_k = n with
  none. The exporter truncates a wider index first; under nusw the
  trunc promise is dropped (precision only).
- **Strides.** `s_k`, nonzero n-bit signed attributes. The exporter skips
  an index whose stride is 0 (zero-sized element types): its
  contribution is 0.
- **Constant.** `c`, an n-bit signed attribute.
- **Flags.** `inbounds` | `nusw` | `nuw` | none. LLVM 18, SeaHorn's build,
  has `inbounds` only; inbounds implies nusw.

Value (total, modular; the flags never change it):

    q = p + sext(i_1)*s_1 + ... + sext(i_m)*s_m + c        (mod 2^n)

**The promise, order-free.** LLVM promises no wrap in GEP operand
order with constants interleaved; the exporter moves constants into c, so
operand-order partial sums of the op are not promised (Q11). Every consumer
uses only these order-free consequences, so they are the promise of nusw:
- (P1) each product `sext(i_k) * s_k` fits in n signed bits;
- (P2) the total offset `O = sum_k sext(i_k)*s_k + c`, as an integer, fits
  in n signed bits (LLVM-order nsw implies it, Q11b);
- (P3) `p + O`, unsigned p plus signed O, is an exact integer in [0, 2^n):
  no wrap above and none below zero.

nuw: P1 to P3 with unsigned forms. inbounds adds in-bounds-of-the-
allocation facts. They are not used yet, but kept distinct (later use:
close to A-INB for the pointer).

**Signs.** Indices, strides, c and O are signed. Negative field offsets
(`container_of`: `p - 16`) and negative indices (`p[-1]`) are common
in low-level code. Nothing assumes a sign. A negative index under nuw with |s| > 1
violates the promise; that is a consequence, not an assumption.

**Where the promise is a fact (rev 3).** In LLVM a broken flag makes
the GEP poison, and poison becomes UB only when the pointer is used. So
the promise is a fact for a ptr.index that is the address of an access (a
load, a store, or a range operation of nonzero length), on the paths that
execute that access. It is not a fact for a value only compared (ptr.cmp,
`eq()` in SeaM) or computed speculatively. Rewriting an access's read term
under the flag is sound because the term is read only on those paths, as
for A-INB (OpSem.cpp:289-291).

ptr.add stays (one offset, no structure). ptr.to_int / ptr.from_int (and
the addr forms) remain the only places where a pointer becomes a number,
so they mark provenance loss.

## 2. Expr encoding

Three operators:
- `PTR_INDEX` (no promise);
- `PTR_INDEX_NUSW` (inbounds or nusw: P1-P3 signed);
- `PTR_INDEX_NUW` (P1-P3 unsigned).

Arguments, all operators:

    (b, x_1, s_1, ..., x_m, s_m, c, K)

- `b`: the base term (n bits).
- `x_k`: the index as an n-bit term. OpSem applies the sext, so a node has
  no extension field; the Lean model is the same.
- `s_k`, `c`, `K`: `Terminal<mpz_class>` arguments holding integers (as
  `mkTerm<unsigned>` carries the alignment of W_SELECT, ExprOpArray.hh:221;
  mpz terminals as kids are the bvnum pattern). Invariant on `s_k` and
  `c` (not K):
  - PTR_INDEX_NUSW: signed n-bit values, in [-2^(n-1), 2^(n-1));
  - PTR_INDEX_NUW: unsigned n-bit values, in [0, 2^n). OpSem's
    ptrIndexTerm stores the n-bit pattern of the signed attribute read
    unsigned (a stride or offset -1 is 2^n - 1), as Semantics.cpp
    brokenPtrIndexPromise reads it; `op::ptr::promise` reads the stored
    value as it is, so the two agree only under this invariant;
  - PTR_INDEX: reduced mod 2^n.
  ptrindex.fold keeps a flag only if the folded c is in its range (S5).

`K` is the byte offset of an access inside the expansion of a wider
access (section 4). OpSem always writes K = 0. Only the expanders write
K != 0.

Value of a node: `b + sum x_k*s_k + c + K (mod 2^n)`. The promise of a
flagged node is about `b + O`, with O excluding K. K is a property of how
the term was made, not part of the GEP.

TypeChecker: the sort of `b` (as WSelect, ExprOpArray.hh:116-122).

Hash-consing: the operator and every argument are part of the node's
identity. Flagged and unflagged arithmetic of one value are different
terms, and a node with K = 8 is a different term from `bvadd(node, 8)`.
This is why the information is in the term and not in a side table keyed
by terms.

**Normal form (rev 6/7).**
- A node may have no index (m = 0): `PTR_INDEX*(b, c, K)`. The VC builds
  every pointer or address addition as a node (section 0).
- `ptrindex.fold`: an index that becomes a numeral v (read as signed n-bit)
  folds into c as the integer `c' = c + s_k*v`. If c' alone (not c' + K)
  fits in n signed bits (nuw:
  unsigned) the operator is kept; otherwise the result is the unflagged
  `PTR_INDEX`, counted `ptrindex.fold-dropped-flags` (Q12). On an
  unflagged node the fold has no range check (c is reduced mod 2^n). A node
  with no index left stays a node `PTR_INDEX*(b, c', K)`.

**Eligibility: when a term carries a promise (S2).** A promise is
read only from a term whose operator is PTR_INDEX_NUSW or PTR_INDEX_NUW
and that is itself the address of an access, or a K-node an expander made
from one (section 4).
- No pointer view of `b` is required: the lemma needs only the same base
  term on both sides. WideExpander, where K is written, has no registry,
  and a loaded-pointer base has no view.
- A flagged node nested in another term carries nothing usable. Since the
  VC and the rules build pointer additions as nodes (section 0), a flagged
  node is never under a VC-made bvadd. A program `ptr.add` on a flagged
  node is a new unflagged node over it (the merge rule never absorbs the
  flagged one): its promise is the inner node's, at the inner address, not
  at `+ 8` (Q9).
- Chains (A6): the chain of an access is the maximal run of flagged nodes
  reached from the access address through base arguments. Each node's P3
  is a fact at the access: in LLVM, poison through a GEP base is poison,
  so a broken inner promise makes the access UB. The run stops at the first
  unflagged node, the opaque base. It is well defined because an unflagged
  node never absorbs a flagged one.

**view() (A4, B1).** The offset part of a view is an integer, never a
pointer node.
- A new integer-sorted operator, `OFFSET(x_1, s_1, ..., x_m, s_m, c, K) :
  bv(n)` (op::ptr, MPZ terminals as in PTR_INDEX; no base, no flags), has
  the value `sum x_k*s_k + c + K`. The expander turns it into bit-vector
  arithmetic, like PTR_INDEX.
- `view(PTR_INDEX*(b, I, c, K))` needs `view(b)`:
  - with no index (m = 0) and a numeral `view(b).off`: off is the numeral
    `view(b).off + c + K (mod 2^n)` (A4). Every numeral-offset rule
    (wideRelation :882, decidePair :1369, SMAP-NEW / HIT, R-HIT / MISS,
    shadowCommutes, rangeInBounds, pruneSize0, ARM leaves) is unchanged;
  - otherwise: off is an OFFSET:
    - a numeral `view(b).off = d` gives `OFFSET(I, c + d, K)`;
    - an OFFSET `view(b).off = OFFSET(I_b, c_b, K_b)` gives `OFFSET(I ++
      I_b, c + c_b, K + K_b)`. K has no semantic role in OFFSET beyond the
      sum.
    - An OFFSET always has m >= 1 (with no index it is the numeral). Its
      (x_k, s_k) pairs are sorted by the index's id, as sortSum does, and
      so are a merged unflagged node's. Otherwise `cmp.same-base` by
      equality would miss reordered index lists.
- The view never carries a flag: stripping the promise is a sort change
  (pointer to integer) that no rule can undo, and it is the shape the
  pointer sort needs (PtrSort.md). Rules build integer terms from off
  (`sub.same-base` :1773, `cmp.same-base` :1580, `inBounds` :1267): those
  are OFFSET or bv terms, never PTR_INDEX.
- `vi->off == vj->off` keeps working (hash-consed OFFSET terms).
- The stride test runs on the original access addresses, never on
  view().off.

**Nested GEPs.** A chain of eligible flagged nodes (the
base of one is the other) composes, in the stride test only, into one
integer linear form: mpz constant, gcd over every stride of the chain.
Composition stops at the first unflagged node or ptr.add (the opaque
base). The premise is P3 of every node of the chain. A composed node with
an n-bit c is never built (Q5c).

**Rules that learn the operators** (A7 adds to the list of section 0):
- view, ptrish, add.normalize (keeps the node as an addend);
- `LeafDecider::leaves` (Ackermann.hh:362-395): a node case, combining
  leaves(b) with `sum s_k of(x_k) + c + K`;
- `iteLeavesWithBase` (:1803), cancel, sub (:1756-1795), accessSpans
  (:811-835);
- the words-check byte definitions (sea-vc.cpp:1211-1216), expanded before
  z3;
- layoutValues (sea-vc.cpp:1148-1158), still keyed by the obj.k constants;
- `ptrindex.fold`;
- `ptrindex.pull-ite`: `PTR_INDEX*(ite(g, p, q), ...)` becomes `ite(g,
  PTR_INDEX*(p, ...), PTR_INDEX*(q, ...))`, keeping the operator. Sound
  because each leaf's promise is used only for decisions about that leaf
  under its guard (Q10 unsat; Q10b sat if aggregated across the
  guard). decideEq, ITE-EQ and LeafDecider already decide per leaf pair.
  The Lean RuleStep takes the guarded conjunction as its condition.
- `cmp.refl-mod-flags`: nodes equal except for the operator are equal
  values (precision only).

Everything else treats the operators as opaque: arguments are normalized
in place, no rule builds a node from expanded arithmetic, and no rule
changes a node's operator except fold and view's rebasing.

## 3. The boundary: two entry points

[History] As implemented the boundary is one entry point,
`SeaOpSem::solverTerm`, which also erases the pointer sort; the guard is
`vc.ptr-sort-at-boundary` (PtrSort.md, "The boundary"). The design below
is the record.

- `SeaOpSem::memoryTerm(e)`: today's solverTerm (store-maps, memops,
  width terms). Nodes stay.
- `SeaOpSem::solverTerm(e) = expandPtrIndex(memoryTerm(e))`: for model
  evaluation, replay and every check (Replay.cpp:27, 62, 97; OpSem.cpp:118,
  121; sea-vc eq-audit, --seam-check premises, side conditions, dag
  stats).
- The sea-vc main query path: memoryTerm, then words / xrt, then
  Ackermann, then `expandPtrIndex` once, before any backend.
- --words-check expands its `defs`, `query`, `bytesR` and `dec`.
- `PtrIndexExpander`, one per SeaOpSem with a shared memo, has the shape
  of WideExpander. It expands PTR_INDEX* and OFFSET (rev 7). The backend
  guard covers both.
- Every backend `add` is guarded: a term containing a PTR_INDEX* operator
  is the error `vc.ptr-index-at-boundary` (the analog of
  vc.memop-term-at-boundary).
- Ackermann: `LinearForms::compute` gets a case, `of(b) + sum s_k*of(x_k) +
  c + K` (exact mod 2^n). Each Read records its index already expanded
  (expanded at lower() when the read is created), so the constraint terms,
  the recorded index and AckermannModel use one node.
- Ackermann stores, per Read, the access-address node it came from, if
  eligible, for the stride test.
- InitialWords: through LinearForms, plus K (section 4).
- AddrRangeMap::node, xrt (ReadTable.hh), accessSpans and accessAligns get
  cases.

## 4. Who writes K

K != 0 is written only where the expanded byte or word is part of an
access at the node, whose A-INB was stated for the whole span:
- `WideExpander::at(a, k)`: if `a` is an eligible node with K = K0 (K0 =
  0 at a wselect / wstore), the result is the same operator with K = K0 +
  k, for 0 <= k < units of the access.
  - If `a` is `ite(g, x, y)` over eligible leaves, k is pushed into the
    leaves: `ite(g, x{K+k}, y{K+k})`. This is exact. Without it, every
    byte read of a phi-based address would lose the rule (S3).
  - Otherwise (a non-node address) the result is the unflagged
    `PTR_INDEX(a, c = 0, K = k)` (A1): a node, not a bvadd.
- `splitRead` (SeaMRules.hh:909-917): the same, including the ite push.
- InitialWords, aligned path: the word index `j - o0 + q*w` of an access at
  an eligible node j becomes the node with K' = K - o0 + q*w. The aligned
  word that contains bytes of an access in the address space is itself in
  the space (2^n is a multiple of w), and `j - o0 >= 0` since j = o0 mod w
  on that path.
  The ite push applies here too.
- InitialWords, masked path: `B_S = s0 & ~(w-1)` is not a node, so the
  stride rule does not apply (correct, just not decided).
- WordsModel::build evaluates wordIndices through the inner model
  (InitialWords.hh:582-584). K-node indices are expanded there first (S4).
- The Ackermann per-Read record keeps the unexpanded index, which carries
  the node. When two reads share one, the first term's record is kept,
  which is sound either way. The stride test enumerates leaf pairs, as
  LeafDecider::leaves does.

## 5. The stride rule

**Shape.**
- Two access addresses A_1, A_2. Each is an eligible node (or the leaf of
  one under pull-ite, per leaf pair), over the same base term p after
  chain composition.
- Integer constants c_j, byte offsets K_j, and g = gcd of |s| over every
  stride of both nodes and chains.
- Access span widths u_1, u_2 (1 for a byte cell, the Ackermann case).

**Lemma** (exact; the Lean RuleStep condition). With O_j the integer
offsets:
- if P3 holds for each node of each chain (p + O_j is an exact integer in
  [0, 2^n)),
- and each span is in the address space: `0 <= p + O_j + K_j` and `p + O_j
  + K_j + u_j <= 2^n`. The lower bound is needed because P3 bounds only
  p + O_j and K can be negative; Q13 is sat without it, Q13b unsat with it
  (S1). The expanders satisfy it: WideExpander and splitRead have
  k >= 0; InitialWords has j - o0 >= 0 (Q15). OpSem's `addressPlus` (a
  program `ptr.add` of a numeral, section 0b) does not: its K is signed
  and has no parent access, so R-STRIDE bounds K by the layout instead
  (section 0c).
- then the circular spans [A_j, A_j + u_j) meet iff the integer spans meet.

So, with D = (c_2 + K_2) - (c_1 + K_1), computed from the nodes' integer
c and K (not from LinearForms' numeral, which is reduced mod 2^n; 24 does
not divide 2^n, S5):
- the spans are disjoint iff no integer m has -u_2 < g*m + D < u_1;
- enumerate m from floor((-u_2 - D)/g) to ceil((u_1 - D)/g), testing each
  m with the strict inequality. A non-empty range alone is not the test
  (Q8c);
- for u_1 = u_2 = 1 this is "g does not divide D".

**Premises.**
- `PremPtrIndex`: P1-P3 of every node involved.
- `PremAInb`, unless K_1 = K_2 = 0 and u_1 = u_2 = 1 (a multi-byte
  span can wrap even at K = 0, Q8). The span-in-space condition comes from
  the A-INB of the parent access, a named, reported assumption (objects fit
  the space: SegmentOp::verify, 7e21123).
- --seam-check reports "unexpressible" when that access has no pointer view
  (a loaded-pointer base). Today inBounds silently yields true for viewless
  leaves (SeaMRules.hh:457-462); this rule must not inherit that (S2).

So the flags-only case is exactly the 1-byte, K = 0 pair. Every other
firing, including every W-SKIP of wide accesses and every Ackermann pair
made by the expansion, counts `premise.a-inb`.

**Where it applies.**
- SeaM: W-SKIP / row skip of a write and a read in one object (the span
  form); ROW compares of byte cells (the equality form). Never in program
  compares (section 1).
- Ackermann: the pair loop, after the residue test. Counted in
  `Ackermannizer::Stats` as `stride` and `stride-a-inb` pairs, printed as
  `vc.ackermann-stride-pairs` / `vc.ackermann-stride-a-inb`. The
  Ackermann header's "no A-INB" (Ackermann.hh:26-27) becomes "A-INB only
  for the stride rule, reported".

**--seam-check.**
- `Premise` gets `PremPtrIndex = 8`; countPremises gets
  `premise.ptr-index`.
- `accessPtrIndexPremise(lhs)` is the promise of the read address, guarded
  by its ite conditions: `promise(N)` at a flagged leaf, `true` at another
  leaf, `ite(c, P(x), P(y))` at `ite(c, x, y)` (section 8: on the path
  where a leaf is selected, only its promise is a fact; the conjunction of
  all leaves' promises is stronger than the fact and can be false in every
  state, test/VC/seam/seam-stride-guarded-premise.mlir).
- R-STRIDE records `PremLayout` too: its integer reading uses the layout
  numeral b (`b + o < 2^n`), and for an entry offset `o >= 2^(n-1)` P2
  alone does not exclude the wrapped solution
  (test/VC/seam/seam-stride-layout-premise.mlir). --seam-check adds the
  placement equations.
- The formula of a node `PTR_INDEX_NUSW(b, x, s, c, K)` over expanded
  terms:
  - for each k, no signed mul overflow or underflow of `x_k * s_k`;
  - no signed overflow of the whole sum `sum + c`, computed in n +
    ceil(log2(m + 2)) bits;
  - `O >=s 0 -> no unsigned overflow of b + O`, and `O <s 0 -> b >=u -O`.
  - nuw: the unsigned forms.
- One function generates these formulas for --seam-check and the paranoid
  pass.
- The obligation goes through solverTerm (expanded) on both sides.

## 6. Lowering, paranoid mode, interpreter

- `sea-lower-ptr-index` turns the op into mul / add / ptr.add (mapping the
  flags onto nsw / nuw where the meaning matches) for consumers that do not
  want the op. The VC keeps the op.
- Paranoid mode (`--check-ptr-index`, optional): for each access whose
  address operand reaches a flagged ptr.index through a chain of ptr.index
  / ptr.add results only (no block arguments), assert the order-free
  promise right before the access, once per (ptr.index, access) pair.
  - A flagged ptr.index with no such use has its flags dropped, counted
    `check-ptr-index.unused`.
  - The output has several asserts (not BMC form), as sea-wrap-flags
    mode=assert.
  - It is a sanity tool, not a discharge obligation.
- **As implemented: `sea-check-ptr-index`, `sea-vc
  --check-ptr-index`.**
  - It runs after placement, at sea-check-align's place, and drops the
    program's asserts.
  - What it follows from an access address: GEP bases, ptr.add /
    addr.add bases, and sea.select (the promise guarded by the condition).
  - Before the access, it asserts every flagged GEP found: P1-P3 in 2n +
    8 + log bits, signed and / or unsigned as Semantics.cpp checks them.
  - Block arguments (phis) are not followed: warning
    `check-ptr-index.phi`. [OPEN] Follow them, with the edge guards. The
    Ackermann rule's ite-of-node origins come from such joins.
  - Unused flagged GEPs are not touched; sea-merge-asserts makes the BMC
    form.
  - Not with --check-align: each job drops the program's asserts.
  - Test: test/VC/check-ptr-index.mlir.
  - [OBS] On vcc-pi (seam, w8): 162 unsat (every used promise
    holds), 66 sat. All 66 stay sat with the program's asserts assumed
    (`--check-ptr-index-assume-asserts`, hidden, diagnostic only: circular
    as a discharge). So the broken promises are not past a failed
    assertion. Example: array_list_pop_back, memset at `list->data +
    item_size * (length - 1)` with huge nondet sizes, P3 broken.
  - [HYP] These are memory-unsafe executions (the access out of every
    object), which A-INB excludes in the main job. [SUPERSEDED, next
    item. Why I was misled: the replay shows huge nondet sizes, which look
    like out-of-bounds; the real cause is where the harness's assumes
    sit.]
  - [FACT] The 66
    sats come from SeaHorn's `horn-unify-assumes: true` (verify-c-common's
    sea.yaml).
    - Every `assume` is folded into one `sea.assume` in the exit block, so
      a check job's assert, landing at the merged assert, never sees the
      harness's assumptions (`i < len`, is_valid, bounded sizes).
    - Re-exported with `horn-unify-assumes: false`, all 228 jobs are
      unsat for --check-ptr-index. The main job's verdicts are identical
      (227 unsat, 1 sat).
    - No genuine wrap. The P1-P3 formula agrees with LangRef.
    - `--check-ptr-index-assume-asserts` is vacuous on these exports: the
      only assert is the final one.
    - A-INB is neither the cause nor needed.
  - [OBS] Re-exported as bench jobset vcc-pi2 (sea-mlir
    takes no unified assumes; SeaHorn unifies them only for gsa +
    dataflow + coi):
    - --check-ptr-index: 228 / 228 unsat;
    - main job (seam, yices): verdicts identical to vcc-pi, Ackermann
      constraints identical (55013);
    - corpus wall 175 -> 141 s, default seed only: a lead, not a result.
  - [OPEN] Remaining recommendations:
    1. Export for sea-mlir with `horn-unify-assumes: false`, at least for
       the check jobs, and record the flag in the corpus .json.
    2. The check jobs refuse a unified-assume program (export SeaHorn's
       `unified.assume` tag; error `check.unified-assume`).
    3. Drop or redefine the assume-asserts diagnostic.
    4. The same pattern holds for sea-check-align and any future
       bounds-check job.
    5. Note that P3 is placement-relative.
- The interpreter warns `interp.ptr-index-promise` once per kind (a
  diagnostic, as interp.align-promise). It does not stop: a broken promise
  on a never-used pointer is a legal program.

## 7. Front end

SeaMlirExport::exportGep emits one sea.ptr.index per GEP with a dynamic
index of nonzero stride, and per inbounds GEP with constant indices only
(`sea.ptr.index %p [] offset c inbounds`; A5). A constant-only GEP without
inbounds, or one whose dynamic indices all have stride 0, stays `ptr.add`
(the VC makes the same unflagged node). The op's verifier allows an empty
index list (stage 1 required one).
- the base;
- each dynamic sequential index with its stride (zero strides skipped)
  and sext;
- the folded constant, accumulated in APInt(2W);
- `inbounds` from `GEPOperator::isInBounds()` (LLVM 18, /usr/lib/llvm-18,
  CMakeLists.txt:181), emitted only if the folded constant
  `isSignedIntN(W)`. Otherwise the exporter warns
  `export.gep-constant-overflow` and emits no flags (Q6).

Constant-expression GEPs take the same path.

## 8. Lean model (seam-lean)

The operators map to constructors carrying the arguments, with the same
pattern as the alignment increment:
- erasure: interpretation and compilation do not depend on the operator
  (eval of PTR_INDEX_NUSW = eval of PTR_INDEX), as eval_eraseAlign;
- the stride RuleStep's condition is the lemma of section 5 (P3 of each
  node, spans in space), a semantic fact that attaching an operator cannot
  prove;
- pull-ite takes the guarded conjunction;
- Negative controls should expose each omitted premise: wraparound,
  overlapping spans, unguarded branch facts, and incorrectly propagated flags.

## 9. Assurance

- The op goes into check-semantics.py (value only), plus roundtrip,
  verifier and interpreter tests.
- Unit tests for the stride and span rules:
  - word level K = 0 (decided, flags only);
  - byte level (A-INB counted);
  - same residue (kept);
  - an unflagged or ineligible side (kept);
  - a ptr.add of 8 on a flagged node, a new unflagged node (kept: Q9);
  - `&t->count` (constant-only, inbounds) vs `slots[i].f` (decided, A5);
  - nested chains; an unflagged link (stops);
  - negative c, strides and indices;
  - overlapping spans with unequal starts (kept);
  - the enumeration at its bounds.
- Check the stride lemma with independent z3 queries and negative controls.
- --seam-check with PremPtrIndex on every firing that uses a promise.
- vc.ptr-index-at-boundary never fires on the corpora.
- Measure on vcc: Ackermann constraints and solve time per solver, for
  bytes and words. Expected: hash_table_eq loses about half its pairs.

## 10. Open

1. Probe pairs on hash_table_eq (5,140): `24*((H+p1) & M) + 96` vs
   `24*((H+p2) & M) + 96`. They really can be equal in a small table
   (mask M = 0 or 1). Deciding them needs path facts and mask arithmetic.
   Left to the solver.
2. Later uses of what `inbounds` adds over `nusw`.
3. [HYP] hash_table_eq's slot base is `map->p_impl`, a loaded pointer
   (hash_table.c:109). It probably resolves to an allocation by a ROW hit
   in the harness. If it does not (nondet-initialized), the stride rule
   still applies (no view needed, S2), but its --seam-check is
   unexpressible.
