# Lambda memory encoding (port of SeaHorn's, to compare with SeaM)

Status: the z3 baseline is implemented (see "Implementation" below). This
page specifies the memory representation, simplification schedule, solver
boundary, elimination, and replay contracts, and describes comparison
variants.

## Why

SeaHorn's memory encoding for verify-c-common uses lambdas with z3's
simplifier applied at every write (`--horn-bv2-lambdas`,
`--horn-bv2-simplify`, verify-c-common `seahorn/sea.yaml`). The observed
performance depends on simplifying at each write; changing the frequency
can substantially change simplification time even when the final formulas
are equivalent. SeaM is our alternative (object-aware
rewriting under A-INB). The question this answers: does SeaM beat the
known recipe on the same programs, VC generator and solver? It is not a
benchmark of a new beta-reducer.

## What SeaHorn does

- `OpSemMemLambdaRepr` (lib/seahorn/BvOpSem2MemRepr.hh:97-131, .cc):
  - load: `fapp(M, p)` (a FAPP node; SeaHorn's Expr does not reduce it,
    `bind::fapp`, ExprOpBind.hh:195);
  - store: `lambda b0. ite(b0 = p, v, fapp(M, b0))` (.cc:467-478);
  - memset: `lambda b0. ite(p <= b0 <= last, v, fapp(M, b0))` with
    partial-word cases (.cc:112-179, 481-555);
  - memcpy: `lambda b0. ite(d <= b0 <= dLast, fapp(S, s + (b0 - d)),
    fapp(M, b0))` (.cc:559-604, 606-697);
  - an array value is coerced to `lambda x. select(A, x)`
    (`coerceArrayToLambda`, .cc:699; `coerce`, .hh:103).
- The reduction is z3's: `Bv2OpSemContext::write` simplifies every value
  written to a symbolic register (not only memories) with one cached
  `ZSimplifier` (BvOpSem2.cc:2917-2950; params `ctrl_c`,
  `:rewriter.flat false`, :2838), then coerces a const array back to a
  lambda. z3's simplifier beta-reduces `select(lambda, a)` (z3 sees a
  lambda as an array and FAPP as select, ExprToZ.def FAPP case) and
  rewrites the resulting ites.
- Before solving: no explicit simplify call (Bmc.cc:59 calls check); z3
  performs its own preprocessing inside `check()`.

## Memory representation (contract)

The faithful baseline uses SeaHorn's unary functional representation, not
arrays: Expr lambdas have `FUNCTIONAL_TY` (`ExprOpBinder.hh:158-176`), while
`SELECT` requires `ARRAY_TY`. The types must not mix under one memory value.

- Every memory value of a lambda-mode VC has functional type `I -> C`:
  `lambda i. <body>`, an ite of such values (a CFG join), or the coercion
  `lambda i. select(A, i)` of an array constant A (initial memory
  `nd.<site>`, a memhavoc array H). No bare array is a memory value.
- A read of a memory value M at p is `fapp(M, p)` (FAPP; not reduced by
  Expr in the baseline).
- The z3 simplifier round trip may return an array-valued term (a const
  array, a store, an ite of arrays, a lambda): it is coerced at one named
  boundary (`coerceMemory`, as SeaHorn's `coerce`) back to the functional
  representation; anything else is an error (`vc.lambda-unexpected`).
- Well-scoped terms: a complete memory value and a top-level load address
  have no free bound (de Bruijn) variable; inside a binder, occurrences are
  well-scoped at their nesting depth (a store body `fapp(M, i)` applies M
  to its own bound i; a nested memory has its own binder). Checked in
  debug by a depth-tracking walk. The baseline never substitutes under a
  binder in Expr; the narrower contract of a custom beta (unary, arguments
  without free bound variables) is stated and checked with that variant.

## Registers and joins (integration boundary)

- Memory registers: `SeaOpSem::mkSymbReg` makes an ARRAY_TY constant for a
  memory value (OpSem.cpp:298-312), and `sea.mem.nondet` havocs it. In
  lambda mode a memory register's key stays that array constant (names,
  model lookup); its value is the functional representation: a havoc /
  nondet memory is the coercion `lambda i. select(A, i)` of the havoced
  array A, written through the same adapter.
- Joins: `--vc-join=ite` only (as `--seam`): a joined memory is an ite of
  functional values. `--vc-join=eq` (fresh phi constants with guarded
  equalities, VCGen.cpp:149-153) is refused in lambda mode
  (`vc.lambda-join`) until fresh functional symbols and the meaning of an
  equality of memories are specified.
- Phi writes: VCGen writes block arguments with `ctx.write` directly
  (VCGen.cpp:148); in lambda mode they go through the same per-write
  simplification hook as op results (one hook, counted as category `phi`).

## Encoding

`sea-vc --mem=lambda` (an encoding of the VC generator, not a program
change), byte (index-unit) memories first; a word layout is refused with
`vc.lambda-word-layout` (a narrower scope, diagnosed). Incompatible with
`--seam` (refused with a message): no A-INB rule, no assumption beyond the
program's own assumes and the lowering's existing restrictions.

- store: `lambda i. ite(i = p, v, fapp(M, i))`;
- memset: `lambda i. ite((i - d) <u n, v, fapp(M, i))`;
- memmove: `lambda i. ite((i - d) <u n, fapp(S, s + (i - d)), fapp(M, i))`;
- memhavoc: `lambda i. ite((i - d) <u n, fapp(H', i), fapp(M, i))`, H' the
  coercion of a fresh array (one per memhavoc, as today);
- symbolic lengths are exact: lambda mode skips `sea-expand-memset`, as
  `--seam` does (no length-bound assume).

Range semantics: `(i - d) <u n` is circular bit-vector membership, exactly
as SeaM-memops.md (with 4-bit d = 14, n = 4, i = 0 is a hit; n = 0 is a
no-op). This is an adaptation of SeaHorn's ordered inclusive endpoints
(`p <= b0 <= last`, last = d + n - 1 or the last word), which differ on
wrapping and on zero-length ranges (n = 0 is in bounds, but an endpoint
formula with d + n - 1 needs its own guard: SeaHorn's guards on the chosen
path to be confirmed). The two agree on nonempty, nonwrapping ranges;
zero-length and wrapping cases are classified separately from scheduling
effects.

## Simplification: the baseline and the variants

Baseline (`--lambda-simplify=z3`, default schedule `per-write`): one
ZSimplifier (one context and cache for the run, fixed parameters
`:rewriter.flat false`) applied to every value written to a symbolic
register: scalar results, memory results of loads / stores / range ops,
and phi / block-argument values; not side conditions (SeaHorn simplifies
those only where its write does) and not internal helper caches. Counters
per category (`lambda.simplify.<category>: calls, cache hits, dag in /
out`) and time. Solve with z3.

Variants, each named and measured apart:
- schedule: `every-k` (every k-th write), `off` (no per-write simplify);
- an explicit final ZSimplify of the query (`--lambda-final-simplify`),
  independent of the schedule: not the same thing as z3's preprocessing
  inside check() (different contract, no shared cache);
- `--lambda-simplify=ours`: the rewriter's BvRules plus beta-reduction in
  Expr (after the beta audit below), and `none`;
- eager Expr beta at loads (a load of a lambda reduced at once): changes
  the shapes reaching the simplifier, so a variant, not the baseline;
- solver: z3 only at first. Yices / Bitwuzla only where the query is
  lambda-free after elimination (below), with `vc.lambda-unsupported` when
  it is not; a z3-per-write + Yices run is a hybrid, recorded as such.

## Elimination, replay

- `--smt2` prints z3's native form of the asserted query (lambdas, as
  arrays, included when they remain): the dump is the solved query. A
  portable lambda-free export is a later, separately named option.
- Before a non-z3 solver (later): recursive elimination of FAPP over
  lambdas, ites of memories and coerced arrays (typed dispatch, not
  `betaReduce` on whatever comes), then a check that no LAMBDA, FAPP or
  free bound variable remains.
- Replay: the interpreter recomputes memories, but `replayAndValidate`
  evaluates stored symbolic values of the VC (memories included) in the
  model: in lambda mode that evaluation goes through z3's model evaluator
  (which handles lambdas), or the elimination above. Tests with sat
  results: reads after memory phis, symbolic copy lengths, overlapping
  copies, havoc versions.

## Beta reduction in Expr (only for `ours` and eager-beta)

The ported `bind::betaReduce` (ExprOpBinder.hh:321) is not a general
capture-safe substitution: `SubBnd` shifts binder offsets by 1
whatever the binder arity and inserts replacements without lifting their
free bound variables, and it keeps visitors in a `std::vector m_pinned`
that cached DagVisit instances reference (growth invalidates them). The
baseline does not use it (z3 beta-reduces). Before `ours`: a focused audit
and fix (deque for the pinned visitors; unary closed arguments only,
checked), with nested-binder tests.

## Comparison

Same lowered inputs and layout (byte memories), pinned Ackermann policy,
simplifier parameters, solver versions and build. Report per phase:
encoding, Expr beta (variants), marshaling to / from z3, simplification,
elimination, solve, replay; call / cache counts and DAG sizes. Verdicts:
lambda vs the plain array encoding must agree where their lowerings admit
the same executions (both assumption-free; the array encoding's bounded
symbolic-memset expansion is not the exact symbolic length outside its
bound: compare with the same lowering, or classify those differences);
lambda vs SeaM may differ only where A-INB is violated (explained by the
independent bounds check, not by re-verifying without A-INB). Then
SeaHorn itself on the same jobs (sea.yaml) as the reference.

## Implementation (baseline)

- `SeaOpSem::setLambdaMemory`: loads `fapp(M, p)`; stores and range ops as
  lambdas (`mkLambda`: `LAMBDA(decl, body)` over `bvar(0)`); a nondet
  memory is havoced and its array kept (`initialArray`, for replay), the
  register gets `lambda i. select(A, i)`; a const memory `lambda i. v`;
  memhavoc arrays as before (`havocArray`). `coerceMemory`: a lambda stays,
  an ite is coerced per branch, an array term `T` becomes `lambda i.
  select(T, i)`.
- `OpSemContext::writeHook`: every register write (op results and VCGen's
  block arguments); sea-vc sets it to ZSimplifier (one EZ3 and cache per
  VC, `:rewriter.flat false`) on every k-th write, then `coerceMemory` for
  memory registers. Counters `lambda.writes`, `lambda.simplify.memory` /
  `.scalar` / `.skipped`, `lambda.simplify-time`;
  `--lambda-final-simplify` (z3_simplify of the query, own context).
- Refusals: with `--seam` / `--simplify`, `--vc-join=eq`
  (`vc.lambda-join`), a non-z3 solver (`vc.lambda-unsupported`), a word
  memory (`vc.lambda-word-layout`, described below). The lowering
  skips sea-expand-memset.
- Test: test/VC/lambda/memops.test (memops corpus programs, sat with replay,
  exact symbolic lengths, knobs, refusals). Every memops program agrees
  with the array encoding, and with --seam where the array encoding bounds
  a symbolic length (memmove-over-bound: sat, as the interpreter).
- Joins: a joined memory is represented as one lambda,
  `lambda i. ite(c, fapp(L1, i), fapp(L2,
  i))`, as SeaHorn's `bind::lite` / `push_ite_lambda`
  (ExprOpBinder.hh:265-319), applied before the write is simplified. A
  plain ite of lambdas is left alone by z3's simplifier
  (`expand_select_ite` is off by default), so a read after a join stayed
  `select(ite(c, L1, L2), p)`: array_list_shrink_to_fit T/O -> 2.3 s
  (SeaHorn 3.0 s); vcc lambda 223 solved / 4124 s -> 228 / 830 s.
- Word memories: the measured best mode promotes to wider memories. A
  widened memory (sea-widen-memory: aligned k-byte cells keyed
  by their first byte, range lengths constant multiples of k) uses the
  same equations; memset's byte is repeated per word. `vc.lambda-word-
  layout` is gone.
- z3 tactic: `--z3-tactic` / `--z3-param` (SeaHorn's
  `--horn-bmc-tactic`, Bmc.cc:38-43).
- Not yet: the debug well-scopedness walk, eager-beta / ours variants
  (need the SUBBND audit), non-z3 elimination.

## Open

1. Is z3's simplify as strong as BvRules, or stronger (contextual
   rewrites)? Measure both (`z3` vs `ours`), and with the equality audit.
2. Deep chains: copies branch across memory versions and changed indices
   inhibit sharing; measure DAG sizes before assuming linear growth.
3. Word layouts: later.
