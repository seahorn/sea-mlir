# Memory, step 2: pointers, allocation, static placement

Status: implemented except address translation in the concrete
interpreter (items 4; the sparse map is correct, translation is for speed)
and `reuse` (an error if requested). Builds on step 1
(`docs/design/Memory.md`).

## Why step 2 is needed now

SeaHorn's exporter will sit at the start of `BmcPass`. There, LLVM pointers
are still abstract: SeaHorn's raw memory manager and its allocator run
inside OpSem, during VC generation, not as LLVM passes. So the exporter
either re-implements them while printing, or emits abstract pointers and
leaves memory layout to sea-mlir. Step 2 provides the abstract level and the
first lowering to the concrete level of step 1.

## Levels

- **Abstract** (exported): `!sea.ptr`, allocation ops, memories indexed by
  pointers. No addresses: where objects live is not decided yet.
- **Concrete** (VC input, step 1): `!sea.addr`, memories indexed by
  addresses, a segment map describing where every object was placed.

The lowering between them is a pass pipeline; this step has one lowering,
*static placement* (SeaHorn's raw memory manager with a static allocator).
Fat pointers, shadow metadata and other pointer kinds come later as other
pointer types and lowerings.

## Abstract level

### Types

- `!sea.ptr` — a pointer. Semantically (object, offset); the null pointer
  is a distinguished value. One kind for now (SeaHorn's raw pointers).
- `!sea.mem<!sea.ptr, iC>` — a memory indexed by pointers (step 1's memory
  type with a pointer index instead of an address). Same ops (load, store,
  memset, memmove, memhavoc), same Mem-SSA chains and `inplace` promise.

### Ops

| op | meaning |
|---|---|
| `%p = sea.alloca <size> align <a> [<site>]` | a stack object of `size` units (constant or `iW` value) |
| `%p = sea.malloc %size align <a> [<site>]` | a heap object |
| `%p = sea.global @g size <s> align <a>` | the object of global `@g` |
| `%p = sea.func_addr @f` | the address of function `@f` (an object of size 0) |
| `sea.free %p` | ends a heap object's lifetime |
| `%q = sea.ptr.add %p, %off : iW` | same object, offset + off |
| `%d = sea.ptr.diff %p, %q` | offset difference (same object; else UB) |
| `%c = sea.ptr.cmp <pred>, %p, %q` | comparison |
| `%n = sea.ptr.null` | null |
| `%i = sea.ptr.to_int %p` / `%p = sea.ptr.from_int %i` | escapes (ptrtoint / inttoptr) |

`site` identifies the allocation op (unique per program, shared with nondet
sites); it names the object in the segment map and in traces.

Each allocation op executed creates a new object. Before lowering, the
program is loop-free in BMC form, so each allocation op creates at most one
object per run; in general the interpreter numbers objects per execution.

### Semantics (well-behaved programs)

Pointer arithmetic stays within an object (one past the end allowed);
comparing and subtracting pointers into different objects is UB; reading
outside an object is UB. As elsewhere, sea verifies well-behaved programs:
UB is not given a meaning, it is instrumented (asserts) or assumed absent.
Memory-safety checking (later) turns these UB cases into asserts before
lowering.

`ptr.to_int` and `ptr.from_int` are where abstract pointers meet integers.
Before placement, `ptr.to_int` has no defined value (the object has no
address yet). Programs that use it (hashing pointers, alignment masks,
tagged pointers) must be lowered before they can be interpreted; the
abstract interpreter stops with an error at such ops.

**Decided: memory cells may hold pointers** at the abstract level (the
interpreter stores the (object, offset) value). Reading such a cell as an
integer (type punning), like `ptr.to_int`, stops the abstract interpreter
with an error; these programs are checked only after placement, where
addresses exist.

**Open decision: `sea.memhavoc` of null is a no-op.** SeaHorn programs
havoc a buffer after an allocation that may fail (`memhavoc(p, n)` with
`p` null). The interpreter runs a memhavoc whose pointer is null itself
(offset 0) as a no-op (`RunOptions::memhavocNullNoop`; `--memhavoc-null=
noop|error` in sea-run and sea-vc, default `noop`). `null + k` is an
address computed from null, a different case, and stays an error, as
does a non-null range outside its object. The choice is not in the formal
semantics yet and is kept visible: each use counts
(`assume.memhavoc-null-noop: uses=<n>`, BRUNCH_STAT
`assume.memhavoc-null-noop-uses` in sea-vc) and warns
`interp.memhavoc-null-noop` once. The VC follows the same rule:
`sea-guard-null-memhavoc` (in the lowering of abstract programs, after
`sea-drop-fresh-memhavoc`) puts a memhavoc whose pointer may be null in a
block of its own, entered only when the pointer is not null, so every
encoding (store chains, lambdas, SeaM's range terms) leaves memory alone
for null; sea-vc reports `assume.memhavoc-null-noop: vc sites=<n>`.
Without the guard (`=error`) the VC would havoc the cells from address 0
on, which in the compact layout (objects from address 1) are the first
object's: a spurious `sat`.

**Decided: the named assumption `addr0-is-null`.** Address 0
is the null pointer. This is almost always true, and false in some
embedded and kernel environments. Under it the abstract interpreter
recovers null where only bytes or an address are known:

- all-zero integer bytes loaded as a pointer are the null pointer (C code
  that zeroes a struct with `memset` and reads a pointer field from it);
- the bytes of a stored null pointer loaded as an integer are 0;
- `ptr.to_int` of the null pointer (offset 0) is 0. `ptr.from_int` of 0
  is null without the assumption.

Every other mix of pointers and integers still stops the run. The run
counts the steps that rested on the assumption (`RunResult::addr0IsNullUses`;
`assume.addr0-is-null: uses=<n>` in `sea-run` and in the replays of
`sea-vc`). It is on by default; `--addr0-is-null=false` turns it off, and
those steps stop the run again. The VC has no switch: placement puts the
null object at address 0 (`sea.segment "null" base 0`), so every placed
program, and every VC, already identifies null with address 0
([SeaM-null](SeaM-null.md)); sea-vc reports it after a lowering as
`assume.addr0-is-null: vc null-uses=<n>`, and `--addr0-is-null=false`
changes its replays only.

### Abstract interpreter

Pointers are (object id, offset) pairs; each object is its own host array
of its size. Bounds are exact: an access outside its object is reported
(error) at the access. This is stronger than the concrete interpreter and
is the reference for the placement pass. The abstract interpreter does not
need a VC; it is a second semantics for differential testing.

## Static placement pass (`sea-place-static`)

Rewrites the abstract program into the concrete one:

- each allocation op becomes `sea.addr.from_int <constant base>`; pointer
  ops become address ops; `!sea.mem<!sea.ptr, iC>` becomes
  `!sea.mem<!sea.addr<W>, iC>`;
- the pass records what it placed in a **segment map** (below).

Placement follows SeaHorn's static allocator by default: stack objects
downwards from `0xC0000000` within a stack window, globals and functions
upwards from `0x08048000`, heap above the globals; every object aligned to
its alignment. Options:

| option | default | meaning |
|---|---|---|
| `width` | 64 | address width W |
| `reuse` | off | let objects with disjoint lifetimes share space (smaller address space; hides use-after-free, so off when checking memory safety) |
| `max-symbolic-size` | 4096 | the room reserved for an allocation whose size is not a constant |
| `symbolic-size` | `assume` | `assume`: insert `sea.assume(size <= max)`; `assert`: insert `sea.assert(size <= max)`; `refuse`: error |
| `layout` | `seahorn` | segment bases and stack window (SeaHorn's constants) |
| `place-out` | none | write the placement to a file, one object per line |
| `place-in` | none | take the bases from such a file instead of computing them |

**External placement** (`place-in`). The pass is still the allocator: it
takes each object's base from the file and checks it as it checks its own
(kind, size and alignment equal to the object's, base aligned, inside the
address width, null at 0 and nothing else there, no overlap); a violation
is an error. Objects are matched by their segment names, which survive
folding (the site attribute). An object the file does not name is an
error; objects of the file the program lacks are holes. Its use is
validation: two lowerings of one program on one file have their common
objects at the same addresses (`utils/cover-diff.py` compares both lowerings on one placement, the
writer's; the reader takes it as an allocator would, so the gate does not
check all placements). The file
format is in [Placement files](../reference/output-formats.md#placement-files).

A symbolic-size allocation gets `max-symbolic-size` units of room, and the
assumption or assertion that makes that sound is explicit in the IR (the
same trust-or-check choice as memcpy -> memmove). Allocations inside loops
are not placeable statically; the pass refuses them (programs are unrolled
before export, so this is an error, not a feature gap).

**Decided: symbolic sizes default to `assume`,** inserted in the IR as
`sea.assume(size <= max)`, with a warning at placement time (SeaHorn assumes
the bound silently, `--horn-opsem-max-symb-alloc`). `assert` stays an
option: in SeaHorn it was hard to insert an assertion at VC-generation
time; in this design the placement pass can simply insert one.

**Decided: functions get proper addresses.** `sea.func_addr @f` is placed
like a size-0 object in the text segment, as SeaHorn's `falloc` does:
unique, comparable, storable. Indirect calls (later, with functions) are
replaced by jump tables that discriminate on these addresses.

### Segment map

The lowered program carries its layout, so every tool (interpreter, VC,
replay, `sea exe` harnesses) reads it from one place:

```mlir
sea.program @main {
  %s3 = sea.segment "stack.3" base 3221225456 size 16 align 8 kind "stack" : !sea.addr<64>
  %g  = sea.segment "global.g" base 134512644 size 4 align 4 kind "global" : !sea.addr<64>
  ...
}
```

`sea.segment` is an op at the start of the program's entry block. Its
result is the object's base address (SeaM step 1.1, docs/design/SeaM.md):
placement replaces an object's uses by that result (plus `sea.addr.add`
for offsets) instead of a numeral, so the object a pointer was derived
from stays visible after placement. It has no folder (sccp must not
replace it), the interpreter evaluates it to its base, and the VC
encodes it as the numeral base (or, with `sea-vc --seam`, as a constant
`obj.<name>` equal to it). A verifier (width only so far) checks that segments do not
overlap (unless `reuse` placed them with disjoint lifetimes, which the
segment records), are aligned, and fit the address width. With `reuse`,
the segment carries the live range it was placed for.

### Validation

- Verifier on the segment map (overlap, alignment, width).
- Differential replay: the abstract interpreter on the program before
  placement and the concrete interpreter after it, on the same inputs,
  must agree on every non-address value and on the verdict. Address values
  differ by design; they are compared through the segment map (base +
  offset).
- The fuzzer generates abstract programs and runs both.

## Concrete interpreter: address translation

Correctness needs no mapping: step 1's buffers are sparse maps keyed by the
full address. Speed does: placed addresses are sparse (stack near
`0xC0000000`, globals near `0x08048000`), and in-place interpretation should
use dense host arrays. So the concrete interpreter translates addresses:

1. **Segments** (from the segment map): one dense host array per segment;
   an address is translated by binary search over segment bases. An access
   outside every segment is out of bounds: an error in strict mode.
2. **Pages** (fallback, non-strict mode): fixed-size pages allocated on
   first touch, two levels like an MMU, for addresses outside every segment
   (programs that compute addresses past their objects, or memory reached
   through `addr.from_int`). Each page keeps a "cell known" bitmap because
   unwritten cells are read lazily from the nondet source or the model.
3. The sparse map stays as the reference implementation; tests compare it
   with the translated one.

Address-space retargeting (word addressing, a smaller address space)
changes the segment map and the address type, not the interpreter.

## VC

Unchanged: the VC only sees the concrete level. Facts that placement makes
true (objects do not overlap) hold by construction with constant bases; the
VC needs no allocator. If a later lowering places objects symbolically
(nondet bases plus assumptions), those assumptions are ordinary
`sea.assume`s in the IR.

## What step 2 does not do

- Fat pointers, shadow memory, tracking memory, ownership (later pointer
  kinds and lowerings).
- Memory-safety instrumentation (an abstract-level pass that inserts
  asserts; needs the object model of this step).
- Function calls, stack frames beyond one program.

## Order of work

1. `!sea.ptr`, abstract memories, allocation and pointer ops; verifiers.
2. Abstract interpreter (objects, exact bounds).
3. `sea-place-static` with the segment map and its verifier.
4. Concrete interpreter: segment translation (pages later, on need).
5. Differential replay before/after placement; fuzzer for abstract
   programs.
