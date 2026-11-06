# Background: SeaM

SeaM is the memory rewriter of `sea-vc` (`--seam`). This page explains the
problem it solves, what it keeps from the program, how it decides address
comparisons, and where it comes from.

## The problem

A C program that writes a buffer and then reads it gives the solver a
chain of array stores followed by a select. To evaluate the select, the
solver compares the read address with every store address, newest first:

$$
\mathit{select}(\mathit{store}(m, p, v), q) =
\mathit{ite}(p = q,\; v,\; \mathit{select}(m, q)).
$$

After a few hundred stores, each load is a deep `ite` chain, and most of
its comparisons are pointless: the store wrote another object, or other
bytes of the same object. The solver cannot see that, because pointers
have become plain bit-vectors, and the fact that separates them is gone:
in C, an offset into one object does not reach another object.

SeaM keeps that fact long enough to use it. While the query is built, it
rewrites each load over the stores before it, decides most comparisons
itself, and leaves the solver only the comparisons it could not decide.

## What it keeps

**Objects.** The base of a placed object is a named constant, `obj.<name>`,
with a side equation `obj.<name> = <address>`, not a numeral. An address
that normalizes to `obj.k + off` is a pointer into object `k`.

**A pointer sort.** Inside the query, addresses have their own sort with
explicit constructors (indexing, from bits, from an integer) and
observations (address, to integer, difference, equality). The rules can
then tell a pointer from an integer that happens to hold an address. At
the solver boundary every pointer term is translated to a bit-vector,
with no assumption.

**Store-Maps.** Writes into one object at distinct constant offsets are
grouped into one term, a map from offset to value. A load at a known
offset is then a lookup instead of a walk down the chain.

**Memory operations.** `memset`, `memmove` and `memhavoc` of a byte memory
are one term each, of any length, including a symbolic one. A read over
such a term is decided once.

## How it decides

A read-over-write step compares a store address $p$ with a read address
$q$. SeaM decides the comparison when it can:

- **Same object.** Both are `obj.k + c`, with constant offsets that do not
  overlap: the store is skipped. Address range maps extend this to offsets
  known only up to an interval. Neither uses an assumption.
- **Different objects.** $p$ points into object $a$ and $q$ into object
  $b \neq a$. Under the in-bounds assumption (A-INB: every access stays
  inside its object) their footprints are disjoint, and the store is
  skipped.
- **Otherwise** the step stays an `ite`, and the solver decides it.

Only the second case uses an assumption, and only for a comparison between
two accesses. A comparison written in the program, such as `a + 16 == b`
for one past the end of `a` against the start of `b`, is legal C. It is
not an access and is never decided by A-INB.

## Assumptions are reported

Every rule names the assumptions it uses, and every run reports what was
assumed:

```text
seam.inb: assumed accesses=437 conditions=0
```

An `unsat` under `--seam` holds for the executions in which those
accesses stay in their objects. `sea-vc` does not discharge A-INB; that
takes a separate memory-safety check of the program. A `sat` that needs an
out-of-bounds access does not replay, and `sea-vc` exits with status 4
instead of reporting it. When an access can be shown statically to leave
its object, SeaM drops the assumption and runs again without it
(`seam.inb: off`). `--seam-check` checks each rewrite against its premises
with the solver. It finds rewriting bugs; it does not discharge A-INB.

## Effect in practice

On the NKI gather proofs, SeaM resolves every load: the query has no
memory constraints left, and the $N = 64$ proof is solved in under a
tenth of a second, while the same program without SeaM does not finish
within two minutes ([NKI kernels](../tutorials/nki.md)). On Firedancer's
flow-control step, 437 accesses are resolved with no in-bounds condition
left for the solver. Where SeaM cannot decide a comparison, for example
between two indices computed by a hash, the `ite` chains reach the solver
([Inductive proof: FD-FRAG](../tutorials/inductive-proof.md)).

## Sources

The memory theory, the address range maps, Store-Map and the rewriter
follow Xiang Zhou's thesis:

> Xiang Zhou. *Expressive and Efficient Memory Representation for Bounded
> Model Checking of C Programs.* MASc thesis, Electrical and Computer
> Engineering, University of Waterloo, 2022.
> [UWSpace 10012/18953](https://uwspace.uwaterloo.ca/handle/10012/18953).

The thesis implements SeaM in SeaBMC, the bounded model checker of
SeaHorn:

> Siddharth Priya, Yusen Su, Yuyan Bao, Xiang Zhou, Yakir Vizel, Arie
> Gurfinkel. *Bounded Model Checking for LLVM.* FMCAD 2022, pp. 214–224.
> [doi:10.34727/2022/isbn.978-3-85448-053-2_28](https://doi.org/10.34727/2022/isbn.978-3-85448-053-2_28).

The thesis chapters map to sea-mlir as follows: the theory of memory
(Ch. 4), address range maps (Ch. 5), Store-Map (Ch. 6), the multi-step
rewriter (Ch. 7), and the evaluation (Ch. 8). sea-mlir differs from the
thesis in places. Memory is byte-addressed with mixed access widths;
memset, memmove and memhavoc are memory-valued terms; null is an object of
size 0; and the address range maps do not assume that pointer arithmetic
never wraps.

## Formal foundation

The pointer semantics of SeaM, its translation to bit-vectors, and its
rewrite rules are formalized in Lean in
[seam-lean](https://github.com/agurfinkel/seam-lean). `sea-seam-export`
exports the terms and rule firings that production runs build, for the
conformance checks in seam-lean ([SeaM export](seam-export.md)). The
formalization is not part of a sea-mlir release.
