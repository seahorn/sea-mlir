# SeaM: initial byte memories read as words

Status: design proposal for re-encoding initial byte memories as words. The
contracts and amendments below describe the exact encoding and its limits.

## Why

Width terms (SeaM-mixed-width.md) keep wide accesses whole through the
rewriting, but on vcc they do not shrink the VC: summed DAG 1,012,465
(no width terms) -> 1,016,562 (all width rules). The byte price SeaM still
pays is at the bottom of the memory: a wide read that reaches the initial
memory (an array constant `nd.<site>`, unconstrained) is expanded at the
solver boundary into byte reads, `wselect(A, j, S)` -> `concat(A[j+7], ...,
A[j])`. On vcc (w8, Yices query) 445 such wselects make about 3500 of the
5888 Ackermann reads, and the Ackermann pairs grow with their square
(Ackermann.md: every pair of reads of one array not decided is a
constraint). A word-aligned 8-byte read should be one read.

## Idea: re-encode the initial array, exactly

For an initial byte array A (index sort I = bv(n), cells bv(8)) and a word
size w = 2^t (t >= 1, w <= 2^n), the word array A_w : I -> bv(8w) with

    A[a] = byte (a mod w) of A_w[a - (a mod w)]                  (little endian)

where "byte o of x" is a constant extract when o is a numeral, and else the
selection of constant extracts ite(o = 0, x[7:0], ite(o = 1, x[15:8], ...))
(extracts rather than shifts: constant extracts are free when
bit-blasting; this uses the same helper as the width rules' byteOf)

is a bijective re-encoding: every byte array has exactly one word array
whose entries at w-aligned indices decode to it (A_w at unaligned indices is
unconstrained and never read). So replacing every read of A by its decoding
in terms of a fresh A_w is equisatisfiable, with no assumption, and a model
of A_w gives A back by the same decoding.

Reads of A, after the replacement:
- an aligned w-byte read (`wselect(A, j, bv(8w))` with j w-aligned):
  `A_w[j]`: one read;
- a byte read `A[a]`: byte `(a & (w-1))` of `A_w[a & ~(w-1)]`: one word
  read and a selection of constant extracts (with a numeral `a mod w`: one
  extract);
- an aligned r-byte read, r < w, inside one word: one word read and an
  extract; an unaligned or straddling read: the concat of the pieces of the
  two words it touches (exact; or the byte decoding per byte).

Alignment of j: the access's trusted `align` (carried by the width term,
SeaM-mixed-width.md: front-end facts) >= w, or proved (j's linear form:
an object base whose numeral is divisible by w plus a multiple of w, as the
rules' congruence / base checks; or a numeral). An unaligned read is still
exact through the general decoding, just not one read.

## Where

At the solver boundary (`SeaOpSem::solverTerm` / `SeaMRewriter::
solverTerm`), after store-map, width-term and memop expansion, before
the Ackermann lowering. Per initial array A of the query:
- applicable iff A is only read (every occurrence the array operand of a
  select or wselect, as the Ackermann lowering's condition; an array under
  a store, an equality of memories, an ite of arrays: A is kept bytes);
- worth it iff A has at least one aligned w-byte read (else nothing is
  gained); w = the widest power-of-two read width with an aligned read (8
  on vcc);
- then every read of A is replaced (all or none per array: the byte
  decoding keeps mixed reads exact).

So the boundary sees width terms of initial memory before expanding them:
the WideExpander leaves `wselect(A, ...)` over a re-encodable initial A to
this step (the order: re-encode, then expand what is left).

Interaction with the Ackermann lowering: A_w is a read-only array of the
query; the lowering lowers it as any other (reads w times fewer for word
traffic; pairs of word indices). Its pair reductions (offset, residue,
leaf) apply to word indices as to byte indices.

## Model and replay

The interpreter replays with initial memory bytes from the model
(Replay.cpp ModelSource for a MemNondetOp: `select(arr, addr)` evaluated in
the model). With the re-encoding the model has A_w, not A: the replay asks
for `A[addr]` through the same decoding (`select(arr, addr)` passed through
the boundary, so it becomes the A_w expression); the evaluation of an A_w
entry not constrained by the model is a don't-care (Replay: a term from the
evaluator = the value does not matter -> 0). Validation of loaded values is
unchanged (they go through solverTerm already).

## Assurance

- The re-encoding is an identity (bijection, defining equation above):
  checked by a unit test per read shape (aligned word, byte with numeral /
  symbolic offset, sub-word, unaligned straddling, wrap at 2^n) against the
  byte array, at 4-bit addresses with w = 2, 4, 8, 16, and with the Ackermann
  lowering after it.
- `--seam-check` is unaffected (rewrite firings are checked before the
  boundary; the boundary is semantics-only).
- SAT replay on the memops / VC corpora with the re-encoding on: the
  interpreter's counterexamples agree (initial bytes decoded from A_w).
- A switch (`--boundary-words`, default off until measured) and the same
  corpus agreement as every SeaM change.

## Measure

vcc (w8, --seam --seam-wide; and without --seam-wide: wide reads of initial
memory exist there only through the split, so expect a gain only with
width terms): Ackermann reads and constraints, DAG, solve time per solver;
hash_table_eq, linked_list_swap_contents, hash_iter_*, string jobs first.

## Contracts

1. Exact encoding vs the aligned fast path.
   - The decoding (every read of A in terms of A_w) is exact with no
     premise; "bijection" holds on aligned-index functions (entries of A_w
     at unaligned indices are irrelevant and unconstrained).
   - Replacing a whole w-byte read at j by the single `A_w[j]` needs
     `j mod w = 0`: from the access's `align` annotation (a trusted
     front-end fact, not an assumption; counted as
     `words.aligned-by-annotation` so reliance is visible), or proved (a
     numeral, a congruence / base check with LAYOUT for a placed base).
     Otherwise the general decoding (pieces of the words touched).
   - The encoding is a separate solver-facing preparation step on the
     final query, not part of `solverTerm`: `solverTerm` keeps the byte
     meaning, which `--seam-check` (firingObligation, its premises) uses
     as the reference; rev 1's "--seam-check runs before the boundary" was
     wrong (the checks translate both sides and the premises through
     solverTerm).
2. A frozen per-query map. An `InitialWords` object: analyze the complete
   prepared query once, freeze `A -> (w, A_w)` (fresh A_w named
   `<A>.w<w>`, through the unique-name check; keys and symbols pinned for
   the model's lifetime), then translate with that map (bottom-up through
   nested reads in addresses). No later call chooses a width or makes a
   second word array; arrays not in the map stay bytes. Pipeline:
   store-maps expanded and memop / compound reads normalized as today,
   keeping `wselect(A, ...)` over eligible initial arrays; analyze;
   translate every occurrence of a chosen A; byte-expand the remaining
   width terms; unique names; Ackermann. Postcondition: no chosen A left in
   the query, no width term left, every word access decodes the same way.
   Eligibility: A a read-only data memory (every surviving occurrence the
   array of a select / wselect, as Ackermann's condition; no binder in the
   query); shadow / companion memories excluded explicitly (the shadow-root
   registry), not by spelling.
3. Model reconstruction. A model adapter (as AckermannModel) used by every
   model consumer (Replay getCell for initial memory, validation of values,
   havoc arrays left alone): an observation of A at a becomes the decoding
   of A_w at the word index, evaluated through AckermannModel when it is
   on. Completion is per word: a byte in a word with constrained bytes takes
   the word's consistent value; only a genuinely unconstrained word is a
   don't-care (0). Replay-only shapes (A under a store / ite in an
   eliminated SSA value) are evaluated over the reconstructed word array
   (as AckermannModel reads store / ite histories) or refused visibly.

Smaller agreements: reads longer than w (e.g. 12 bytes with w = 4) are the
concatenation of their pieces (or the per-byte decoding); v1 fixes w = 8
(bounded by 2^n), the choice kept separate from the encoding; aligned
indices are kept direct so the Ackermann linear-form reductions still apply;
measure surviving constraints and their widths, not only pair counts, with
Ackermann on and off.

Acceptance cases: one array with eligible and ineligible uses; two
initial arrays with equal addresses; a nested loaded-pointer address;
model evaluations in changed order; misleading `align` with the reference
checker still catching the bad rewrite; a chosen A under stores in
replay-only values; a constrained word with replay-only byte reads; fresh
symbol uniqueness and a stable map under repeated translation.

## Amendments

These refinements take precedence over the earlier contracts. The decoding
identities were checked for each shape (n = 1..5, w = 2..16, including w = 2^n).

1. Phase order (sharpens Contract 2). Byte reads of A are *created* by the
   WideExpander (a wselect over a chain ending in A) and by MemOpTranslator
   (an mhavoc / mset / mcopy base, or an mcopy source, equal to A). The
   translator refuses a wselect over a memop term. Order:
   (a) expand the store-maps;
   (b) byte-expand the width terms that are not directly over an initial
       byte-array constant (a syntactic test);
   (c) memop translation;
   (d) analyze eligibility and freeze the map;
   (e) translate the chosen arrays;
   (f) byte-expand the surviving wselects;
   (g) unique names;
   (h) Ackermann.
   The postcondition is a hard error (no chosen A, no width term left).
2. Symbolic byte reads keep linear indices. The mask `(a+k) & ~(w-1)` is a
   BAND atom for LinearForms / LeafDecider, so it would turn free offset
   pairs (the 8 bytes of one split read) and layout-decided pairs into live
   constraints. Instead, decode a byte read by the index's linear form
   S + c:
   (i) If S is provably w-aligned (coefficients multiples of w, a LAYOUT
       base numeral divisible by w), the word is `A_w[S + (c & ~(w-1))]`
       and the byte is one constant extract.
   (ii) Otherwise, use one canonical atom per S, `B_S = rebuild(S) &
        ~(w-1)` (hash-consed), and `o_S = rebuild(S) & (w-1)`. With
        c = qw + c', the byte is byte `o_S + c'` of `concat(A_w[B_S +
        (q+1)w], A_w[B_S + qw])`. This is exact, including the wrap at
        w = 2^n.
   Every word index of one S is then B_S plus a multiple of w, so its pairs
   are offset pairs and free. Acceptance: for a split read at a symbolic
   unaligned address, the Ackermann constraint count does not grow. Count
   `words.masked-reads` and `words.word-reads`. Teaching LinearForms the
   mask on w-divisible forms is a later refinement.
3. Worth it. "One aligned word read" is too weak. Byte-dominated arrays (the
   string jobs' p+i loops) get a larger DAG and no pair gain [HYP]. v1
   keeps a per-array census and a reported rule, and measures the string
   jobs with the encoding forced on and off.
4. memhavoc arrays have the same shape (a fresh read-only byte array, the
   same Ackermann and getCell path). They are included in v1 under the same
   eligibility. This supersedes "havoc arrays left alone" in Contract 3.
5. Direction of the `align` trust (add to Contract 1). A_w is defined as the
   packing at every index, so every replacement, including the fast path,
   is an identity whenever the annotation holds. The word query is
   therefore unsat only if the byte query is. A wrong annotation, or an
   incomplete translation, can only give a spurious SAT, which replay
   rejects (exit 2). sea-vc's mismatch report names this second cause and
   points at `--check-align`.
6. Assurance: `--words-check`, a differential check of the real pipeline on
   the final query. The finite defining equations A_w[j_k] = concat(A
   bytes), one per word index read, are conjoined, and defs ∧ (Q_bytes xor
   Q_words) must be unsat (Ackermann off, no alignment premises; it holds
   by point 5). Run it on fuzz-seam and on sampled vcc jobs. More acceptance
   cases: byte reads produced by the translator and by the WideExpander,
   two chosen arrays through one loaded pointer, the adapter outside
   AckermannModel, and the Ackermann count of point 2.
7. Corners:
   - w = 8 needs n >= 3 (else ineligible).
   - At w = 2^n the straddle form reads one word twice, which is still
     exact.
   - A sub-word read of r | w bytes at an r-aligned j never straddles.
   - When r does not divide w, use the general form.
8. Measurements to add:
   - constraints by pair class (word vs masked);
   - masked indices per array;
   - DAG per array;
   - jobs already widened (no change expected);
   - Bitwuzla without Ackermann.

## Open

1. w per array: the widest aligned read width, or a fixed 8? Mixed 4 / 8
   reads of one array: re-encode at 8, 4-byte aligned reads are sub-word
   extracts.
2. Initial memories under stores in the query (Ackermann keeps those): a
   word-level store chain would need wide writes at the boundary; not in
   v1.
3. Shadow / fat-pointer companion memories: they are not byte arrays of
   program data; leave them. (memhavoc arrays are data: rev 3 point 4.)
