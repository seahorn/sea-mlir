# Known limitations

## Scope

- sea-mlir 0.6.1 is a research prototype. The text format of the dialect
  and the command-line options can change between releases.
- Linux on x86-64 is the platform the release gates run on. From 0.6.1
  on, sea-mlir also builds on macOS (arm64) and passes `check-sea` there.
- z3 is the supported solver. Yices 2 and Bitwuzla work and are
  smoke-tested; Bitwuzla is optional at build time.
- `sea-vc` checks programs without loops (BMC form). Loops are unrolled to
  a bound by the front end, or replaced by inductive cuts in the harness,
  as in the [Applications](../tutorials/index.md).
- Vacuity is checked per assertion only on request: `sea-vc --reach`
  reports which checks an execution can reach
  ([Reach](output-formats.md#reach)); an ordinary `sea-vc` run does not.

## Assumptions that are not discharged

- The pointer premises of SeaM (A-SEP: no cell accessed through two
  objects; A-NULL: no access through an object of size 0; A-NOWRAP: no
  access address wraps) are reported but not discharged by sea-mlir.
  Discharging them takes a separate memory-safety (bounds) check of the
  program. `--seam-premises=0` builds the VC without them, more slowly; it
  is a measurement, not a discharge, and the lowering's dropped memhavocs
  still rest on A-SEP (`seam.premises: off (lowering:
  drop-fresh-memhavoc=<n> rests on A-SEP)`).
- Front-end annotations (`align`, and the no-wrap flags of pointer
  arithmetic) are trusted. `--check-align` and `--check-ptr-index` check
  them as separate jobs.
- `--lower` of an abstract program removes memhavocs of fresh objects
  (nothing wrote or read the object since its allocation). A drop rests on
  A-SEP when a run that executes the memhavoc may also access (read or
  write, through any pointer, in any order) the memory as it was before
  it, since after
  placement an access of another object could reach the object's cells, or
  when the memhavoc's range is not provably in its object; it is reported as `assume.a-sep:
  drop-fresh-memhavoc=<n>`, also without `--seam`. The count is coarse:
  an access that provably stays in its object counts too. The static
  out-of-object report (`--seam`) does not see a dropped memhavoc; the
  pass warns `drop-fresh-memhavoc.range` for one whose range it cannot
  show in its object by a cheap syntactic check (best effort: no warning
  is no proof; the count is what the answer rests on). A separate
  memory-safety check of the program would discharge it; sea-mlir has
  none yet.
- Open semantics decision: `sea.memhavoc` of the null pointer is a no-op
  in the interpreter (`--memhavoc-null=noop`, the default; `=error` makes
  it a memory-safety error). It is not part of the formal semantics yet,
  and its consequences are not fully understood, so every run that relies
  on it prints `assume.memhavoc-null-noop: uses=<n>` and warns
  `interp.memhavoc-null-noop`. Only null itself: `null + k` and a non-null
  range outside its object stay errors. The VC follows the same rule: the
  lowering runs a memhavoc whose pointer may be null only when it is not
  (`sea-guard-null-memhavoc`), or drops it when its object is fresh
  (`sea-drop-fresh-memhavoc`); both count in `assume.memhavoc-null-noop:
  vc sites=<n>`. With `=error` neither happens: the VC havocs the cells
  from address 0 on, which may be the first object's in the compact
  layout.
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
- `--lower` places an allocation of non-constant size in
  `--max-symbolic-size` units (default 4096) and assumes that its size
  fits (`assume.symbolic-size-bound`); an `unsat` then holds only for sizes
  up to that room. The object keeps its symbolic size for bounds; the room
  matters for placement. `--symbolic-size=assert` checks the bound instead
  (`premise.failed`, exit 4, when a run exceeds it).

## Running

- `--timeout` bounds the solver only. Query generation (lowering,
  encoding, SeaM rewriting) is not bounded by it, and large exports have
  needed tens of GB. Use an external `timeout` and `--mem-limit`; there is
  no memory limit by default.
- When the static in-bounds check finds an access that leaves its object
  (`vc.seam-out-of-object`), `sea-vc` warns, names the accesses with where
  they land, and keeps the premises: an `unsat` then holds only if those
  accesses do not execute. The check cannot tell whether they do: an
  access it names can lie on an infeasible path, such as an iteration of
  an unrolled loop that cannot execute, or on a path that does execute.
  `--seam-out-of-object=fallback` runs again without the premises.
- A counterexample that leaves its object only into a gap between objects
  keeps A-NULL, and keeps A-SEP unless another provenance reaches the same
  cell (classified `shared`), but `sea-vc` still reports it as a
  memory-safety error (exit 4): its replay check is of bounds, not of
  separation.
- `--seam-check` states every premise firing as the in-bounds conditions of
  its accesses; checks stated per premise are an open follow-up.
- `--seam-stride` (R-STRIDE) is opt-in and has known wrong answers, an
  `unsat` where the program can fail:
  - It can skip a store-map entry that lies outside its object, justified
    only by the signed reading of the no-wrap premise.
  - It can fire after constant offsets are folded mod 2^n into a GEP's
    byte offset, which no reading of the no-wrap premise covers.
  - It uses a GEP promise where `--check-ptr-index` does not reach it
    (through memory, a block argument or an integer round trip), so that check does not cover the
    promise it relied on.
- `--ptr-mode=address` can be much slower on code that aligns pointers by
  integer arithmetic.

- Pass validation (`utils/cover-diff.py`) runs both lowerings on one
  placement, written by one side and read by the other (`--place-out`,
  `--place-in`). It needs one side to have every object of the other, with
  the same size and alignment. A lowering that creates objects, or that
  folds an allocation size on one side only (a symbolic size then gets
  `max-symbolic-size` units there), has no shared placement: the job is
  `unknown` (`reason=layout`).

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
  global initializers that hold pointers or constant expressions, small
  constant tables, `llvm.cttz`, `llvm.ctlz` and `llvm.expect`. Check the
  export's warnings
  ([example](../tutorials/firedancer.md#the-decode-table)). SeaHorn
  before dev18 commit 67474bf6 also dropped initializers over 16 KiB;
  later versions export them as `sea.mem.fill`.
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
