# Known limitations

## Scope

- sea-mlir 0.1.0 is a research prototype. The text format of the dialect
  and the command-line options can change between releases.
- Linux on x86-64 is the only platform that is built and tested.
- z3 is the supported solver. Yices 2 and Bitwuzla work and are
  smoke-tested; Bitwuzla is optional at build time.
- `sea-vc` checks programs without loops (BMC form). Loops are unrolled to
  a bound by the front end, or replaced by inductive cuts in the harness,
  as in the [Applications](../tutorials/index.md).
- There is no per-assertion vacuity check. Reach jobs in the harness are
  the check.

## Assumptions that are not discharged

- The in-bounds assumption of SeaM (every access stays inside its object)
  is reported but not discharged by sea-mlir. Discharging it takes a
  separate memory-safety check of the program. `--seam-inb=0` builds the
  VC without the assumption, more slowly; it is a measurement, not a
  discharge, and the lowering's dropped memhavocs still rest on it
  (`seam.inb: off (lowering: drop-fresh-memhavoc=<n> rests on A-INB)`).
- Front-end annotations (`align`, and the no-wrap flags of pointer
  arithmetic) are trusted. `--check-align` and `--check-ptr-index` check
  them as separate jobs.
- `--lower` of an abstract program removes memhavocs of fresh objects;
  when one rests on A-INB (an access of another object could land in the
  object after placement), it reports `assume.a-inb:
  drop-fresh-memhavoc=<n>`, also without `--seam`. A separate
  memory-safety check of the program would discharge it; sea-mlir has
  none yet.
- Address 0 is the null pointer (`addr0-is-null`). Placement puts the null
  object at address 0, so every VC of a lowered program rests on it
  (`assume.addr0-is-null: vc null-uses=<n>`), and the replays recover null
  from zero bytes under it (`assume.addr0-is-null: uses=<n>`). It is a fact
  about the target (false in some embedded and kernel environments), not
  about the program: no check discharges it. `--addr0-is-null=false`
  changes the replays only.
- Without `--seam`, `--lower` bounds a memset or memmove of symbolic
  length and reports the bound (`assume.memset-bound`); an `unsat` then
  holds only for lengths up to the bound.

## Running

- `--timeout` bounds the solver only. Query generation (lowering,
  encoding, SeaM rewriting) is not bounded by it, and large exports have
  needed tens of GB. Use an external `timeout` and `--mem-limit`; there is
  no memory limit by default.
- When the static in-bounds check fails (`vc.seam-inb-violated`), `sea-vc`
  warns, names the accesses, and runs again without the assumption. The
  access it names can lie on an infeasible path, such as an iteration of
  an unrolled loop that cannot execute.
- `--ptr-mode=address` can be much slower on code that aligns pointers by
  integer arithmetic.

## Programs exported by SeaHorn

These concern only programs that reach sea-mlir through SeaHorn's front end
and exporter. Programs written directly in the sea dialect are not
affected.

- **Pointer analysis at havoc cuts.** sea-dsa, SeaHorn's pointer
  analysis, can miss pointer equalities that are only assumed after a
  havoc. An `assume` constrains values only, and sea-dsa takes a pointer
  loaded from havocked memory to alias no known object. The memory regions
  of the exported program can then be wrong. In the
  [FD-FRAG step](../tutorials/inductive-proof.md), the invariant says
  that a field of the reassembly state points to the data buffer; without
  more, sea-dsa placed the copy's destination in a region apart from that
  buffer, and a claim about the buffer read a stale region. This is a
  soundness hazard of the export path, outside sea-mlir; a harness states
  such aliases with `sea_dsa_alias`. Any cut that havocs pointers needs
  this check.
- **Unified assumptions.** Export with `--horn-unify-assumes=false`.
  Unified assumptions merge assumptions into the assertion, and the check
  jobs (`--check-ptr-index`, `--check-align`) then report spurious
  failures.
- **Exporter gaps.** The exporter drops or mishandles some constructs:
  initializers over 16 KiB, small constant tables, `llvm.cttz`,
  `llvm.ctlz` and `llvm.expect`. Check the export's warnings
  ([example](../tutorials/firedancer.md#the-decode-table)).
- **Type-based alias annotations are trusted.** SeaHorn's memory analysis
  uses C and C++ TBAA. Code that writes an object through one type and
  reads it through an unrelated one (a strict-aliasing violation, for
  example a mock that mirrors a library type with its own struct) can read
  a stale memory value and make a property vacuously true. The export
  warns `TBAA moved N shadow-memory use(s) past stores`; re-export with
  `--horn-shadow-mem-use-tbaa=false` to check whether a result depends on
  it. Write mocks against the real types.
- **One failure site per harness.** With several `__VERIFIER_error`
  calls, optimization can drop the location of the one that fails, and
  `assert-at` then names the last located operation before it. Route every
  failure through the harness's check macro.

## What the formal proofs cover

The Lean proofs cover the pointer semantics of SeaM, its translation to
bit-vectors, and its rewrite rules. They do not cover the rest of the
tool, or the C++ code that applies the rules; the conformance checks of
`sea-seam-export` and the testing described in
[Why trust it](../trust/index.md) connect the two.
