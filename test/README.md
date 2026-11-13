# sea-mlir tests: rules

Tests exist to catch wrong meaning, not to freeze the code. A large suite
that pins incidental details (output layout, SSA names, op order, step
counts, message wording) blocks refactoring. So tests come in two kinds,
with different rules.

## Oracles (protected)

An oracle checks meaning against an independent reference. Today:

- `Interp/semantics-z3.test` + `utils/check-semantics.py`: op semantics vs
  z3, for the interpreter (sea-run), for Expr solved with z3 and with
  Yices (sea-expr-eval; also Bitwuzla in `semantics-bitwuzla.test`), for sea-run after sea-expand-intrinsics
  (intrinsics are checked against their LangRef definitions in SMT-LIB),
  and for the op folders (`--fold`: sea-opt --sccp must fold every case
  to a sea.const of the reference value).
- `VC/fuzz.test` + `utils/fuzz-vc.py`: VC vs interpreter on random programs
  in BMC form (scalars, and memory: in-place stores and loads with aliasing
  3-bit addresses, address ops, versions merged at joins): z3 and Yices
  agree (`VC/fuzz-bitwuzla.test`: Bitwuzla too); every sat replays to assert-failed with every value equal to the
  model's; every unsat survives random interpreter runs; the program and
  its VC lowering (sea-vc-lower) agree value by value on the same inputs
  (the lowered run gets the values the original run read; values are
  matched by location; a scalar struct.extract, which the lowering
  erases, is observed through an `x | x` op the generator puts after it;
  sccp's folded constants lose their location and cse / canonicalize
  erase ops, so the values are compared on the lowering without them,
  `sea-vc-lower{fold=false opt=false}`, and the located ones also on the
  full lowering);
  sea-wrap-flags{mode=assert} fails exactly when a promise is broken.

- `Transforms/fuzz-place.test` + `utils/fuzz-place.py`: static placement
  vs the abstract interpreter on random abstract programs (objects,
  in-bounds pointer arithmetic, memory ops, pointers through joins): on the
  same random inputs both runs reach the same status and compute the same
  values op by op (addresses excluded); sea-vc --lower confirms every sat by
  replay; every unsat survives the abstract runs.
- `Transforms/widen-memory-diff.test` + `utils/check-widen.py`:
  sea-widen-memory (byte memory -> aligned k-byte words) vs byte memory,
  on the generated opsem programs: a byte run's nondet values and memory
  bytes are replayed (bytes combined into words) into the widened program,
  which must reach the same status and trace with no other input; at
  least one program must widen (k = 2, 4).

- `Transforms/int-ranges.test` + `unittests/IntRanges`: the integer ranges
  of the sea value ops (InferIntRangeInterface, upstream transfer functions
  adapted to sea's total semantics) and the facts of sea-range-fold (the
  bound of `icmp pred x, c`, its step back through sext / zext, the
  tightening of a range) vs evalPureOp at small widths (intersections of
  unsigned and signed intervals sampled at width 3): every value of every
  operand-range tuple lands in the inferred range.

Coming: expected verdicts of benchmark programs, verdict diffs against
SeaHorn BMC.

Oracles are listed here and carry `ORACLE` in their header. Changing what an
oracle accepts (its reference, its expected verdicts, its tolerance) needs
maintainer sign-off. Fixing the code so the oracle passes never does. An oracle
may be refactored (how it generates cases, how it runs tools) as long as it
still kills the same mutants.

## Everything else (disposable)

Roundtrip, printing, verifier diagnostics, CLI and trace format, and
hand-picked interpreter examples are disposable. If one blocks a change:

1. **Recreate** it when the property still matters but the details changed:
   rewrite it against the new code. If the change was meant to change
   behavior, say what changed in the commit message, so it is not taken
   for silently editing expectations to match.
2. **Disable** it when it is unclear what it should become: add
   ```
   // REQUIRES: disabled-test
   // DISABLED: <YYYY-MM-DD> <why; what would make it valid again>
   ```
   It is reported as UNSUPPORTED; `lit --param run_disabled=1` runs it.
3. **Kill** it when its property no longer exists or another test covers it.

No sign-off is needed for any of these. Every disable or kill is named in
the commit message. `grep -rn 'REQUIRES: disabled-test' test` lists
disabled tests; each QA summary reports the count, and a test disabled for
more than two weeks gets recreated or killed.

Before a disable or kill, check that the oracles still pass. A disposable
test failing alongside an oracle is a bug report, not friction.

## Writing tests that do not get in the way

- Check the property, not the whole output: `CHECK: status: assert-failed`,
  not the full report. Use `CHECK-NOT`/`CHECK-SAME` sparingly.
- Do not pin incidental details unless they are the point of the test:
  step counts, SSA value names (`%[[X:.*]]` captures), unrelated op order,
  full diagnostic wording (match a distinctive fragment).
- One property per test file, or one per `-split-input-file` chunk, so a
  break disables a small piece.
- Prefer generated cases (from the op list, as `check-semantics.py` does)
  over long hand-written lists: they follow the code when it changes.
- Coverage, not test count, is the measure: a new test should cover
  something no other test covers.

## SeaHorn parity (not a lit test)

`utils/run-seahorn-tests.py <seahorn>/test/opsem --seahorn ... --sea-vc ...`
compares the sea-mlir BMC engine with SeaHorn's mono engine on every
`%seabmc` RUN configuration of the tests (agree / WRONG / unsupported /
unknown / timeout / no-reference; configurations with memory-model flags
sea-mlir does not model are counted apart). With `--sea <seahorn>/bin/sea`
it also runs the `%sea` / `%fpfsea` RUN lines of `test/opsem2` (name each
directory: `opsem2 opsem2/widemem opsem2/trackingmem`; ownsem is out of
scope); there SeaHorn's per-assert vacuity reports, which sea-mlir does not
produce, are counted as checks-missing. It needs a SeaHorn build with the
sea-mlir engine, so it is run by hand; WRONG is exit status 1.

## SeaHorn parity regressions (`SeaHorn/opsem/`, generated)

`SeaHorn/opsem/*.mlir` are SeaHorn's `test/opsem` programs exported to sea
(each test's first `%seabmc` configuration), with the verdict of SeaHorn's
mono engine checked through `sea-vc --lower`. They are parity regression
tests, not oracles: they record that sea-mlir agreed with SeaHorn when they
were generated, and run without SeaHorn. Disposable: do not edit them by
hand; regenerate the whole set (it also drops tests that no longer export
or agree, and lists them) with
`utils/export-opsem-tests.py --seahorn <seahorn>/build/bin/seahorn
--tests <seahorn>/test/opsem --sea-vc build/bin/sea-vc`.
