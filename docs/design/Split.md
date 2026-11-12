# Case splitting of the VC

`include/sea/VC/Split.h`, `lib/VC/Split.cpp`; `sea-vc --split`,
`--split-leaves`, `--split-auto` (`sea-vc --agent`, section SPLIT); tests
`test/VC/split/`.

## Why

A query that times out is one formula; nothing says which case of it is
hard. A split answers several smaller queries, each a case of the
original, and reports each part's counters (open alias questions,
Ackermann pairs, `row.*` firings, query size), answer and time. It is
infrastructure for experiments and for debugging timeouts; it has no
automatic strategy beyond a simple, optional heuristic.

SeaM does no path-condition reasoning: an alias question whose answer
depends on a branch stays open. A split is an explicit, counted, top-level
case split outside the rewriter: SeaM rewrites each part with the case
fixed.

## Construction

Shannon cofactoring. For a term `t` of the VC and values `v_1 .. v_n` with
conditions `c_1 .. c_n` such that

- (E) `c_1 \/ ... \/ c_n` is valid (exhaustive), and
- (X) `c_j => t = v_j` is valid for each j (exact),

the query `Q` is satisfiable iff some part `Q_j = Q[t := v_j] /\ c_j` is:
a model of `Q` satisfies some `c_j` (E) and then `t = v_j` (X), so it is a
model of `Q_j`; a model of `Q_j` satisfies `c_j`, so `t = v_j` (X) and it
is a model of `Q`. The substitution may replace any subset of the
occurrences of `t`. So: unsat iff every part is unsat; sat iff some part
is.

Targets:

- an i1 value with term `t`: `v = 1, 0`, `c = (t = v)`;
- a block guard `g` (a Bool constant of VCGen): `v = true, false`,
  `c = g` / `!g` (the entry block has none: refused);
- an address whose term is an ite tree, or `ptr-from-bits` of one: one case
  per distinct leaf `l` (hash-consed identity, first-occurrence order),
  `v = l` (`ptr-from-bits(l)`, normalized by the rewriter), `c` = the
  disjunction over the paths to `l` of the conjunction of the ite
  conditions on the path.

## Where: during the encoding

SeaM rewrites each load and store as the op is encoded (`SeaOpSem` calls
the rewriter), so there is no unrewritten VC to substitute into. The split
happens at the definition: `OpSemContext::defHook` (after each op
of a block, and after the block arguments of a join) and
`OpSemContext::guardHook` (when VCGen creates a block guard). The hook
replaces the target's value in the symbolic store; every later use reads
the case's value, and SeaM rewrites those uses with it. The target's own
term (`t`) is as the encoding built it, rewritten. The part's condition is
conjoined to the query before its normalization. With no split option the
hooks are null: no term changes (query byte-identical).

Nested targets: one level per target the encoding meets, in encoding
order, each under the cases chosen before it (the tree is not simplified under
the parent's case, so a nested level may yield vacuous parts, whose
condition is false under the parent's; they count toward
`--split-max-parts`). A run takes the choices of
its first levels (the prefix) and case 0 below; its levels give the
siblings. Parts are enumerated depth first, one encoding per part. A
level whose case count does not cover its planned choice stops the split
(`vc.split-unstable`): the encoding is assumed deterministic, and this is
where that assumption would show.

## Checks

`--split-check` (default on) states (E) for every level of a run and (X)
for its chosen case, on the solver's terms, and asks z3 under an rlimit. For
`--split-leaves`, (X) holds at the pointer level by construction (the value
is a leaf of the term's ite tree, or the rewriter's normal form of
ptr-from-bits of one; checked structurally, counted as `leaf-structural`,
an error `vc.split-target` if it fails); the z3 check compares addresses
only. Every case of every level is the chosen case of
some part, so a complete split checks (X) for all of them. A failed check
is exit 5 (`vc.split-check-failed`); an unknown is a warning
(`vc.split-check-unknown`) and the construction is not checked. The
`split.verdict` record carries `check-unknown=<n>` (levels whose check was
unknown or unexpressible) and `check=on|off`, so an incomplete check is
visible in the verdict. The
hidden `--x-split-plant=drop-case|wrong-value` plants the two bugs; the
tests check that the check catches them.

## The heuristic (`--split-auto`)

A pick run (encoding, SeaM, census; no solver) records each address
value whose term is an ite tree, and counts the open alias questions
(`IS_OVERLAP` left in the normalized query) whose pointer arguments
contain the value's term or one of its leaves that has no object (SeaM's
leaf-pair decisions split an ite pointer, so the open question holds the
unknown leaf, not the ite). Ties: more leaves first. The first candidate
with two or more leaves and at least one question is split on its leaves.
The attribution over-counts (an unknown leaf behind several ites counts
for each of them): it is a ranking for experiments, not a strategy.

## Open questions

- Default of `--split-check`: on; cheap on the measured queries. A target
  whose term is large could make it costly.
- Splitting a leaf that is itself an ite of pointers under `to-bits`
  (reported as `leaf:unknown(ite)`): a second `--split-leaves` level
  cannot name it (it is not a program value).
- Selecting targets by location matches every op result of the kind with
  a position on the line, inlined call sites included; canonicalization
  may merge the op the user means into another one (then the name of the
  lowered program, from `split.candidate` or the error, is the handle).
