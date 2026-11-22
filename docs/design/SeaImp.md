# Variables over sea: the seaimp dialect

`seaimp` (ODS: `include/sea/Dialect/SeaImp/`; verifier and Mem2Reg
interfaces: `lib/Dialect/SeaImp/SeaImpOps.cpp`) is an imperative layer over
sea. It holds what is convenient to write but is not SSA. Today that is
variables: typed slots read and written whole, like LLVM `alloca` +
`load` / `store` of a local. A variable may hold a memory: it is then a
region, accessed by region ops that are the sea memory ops without the
Mem-SSA plumbing. Its lowering `sea-lower-seaimp`
(`lib/Transforms/LowerSeaImp.cpp`) turns variables into SSA values and
block arguments with MLIR's Mem2Reg, and regions into Mem-SSA chains. The
pass `sea-erase-mem-ssa` (`lib/Transforms/EraseMemSsa.cpp`) goes the other
way for a program's one memory. Tests: `test/Dialect/SeaImp/roundtrip.mlir`,
`test/Dialect/SeaImp/invalid.mlir`, `test/Transforms/lower-seaimp.mlir`,
`test/Transforms/lower-seaimp-mem.mlir`, `test/Transforms/erase-mem-ssa.mlir`,
`test/Transforms/erase-mem-ssa-invalid.mlir`, `test/VC/seaimp.mlir`,
`test/VC/seaimp-mem.mlir`, `test/VC/erase-mem-ssa-corpus.test`.

Status: variables of register types (integers, pointers, structs) and of
memory type (regions) with the region accesses (including modification
tracking and counterexample prints) and allocation in a region (stack,
heap, global); erasing the memory SSA of a program with one memory. Not
yet: see the end of this page.

## Ops

```mlir
%x = seaimp.var "x" [7] : !seaimp.var<i32>
seaimp.set %x, %v : !seaimp.var<i32>
%y = seaimp.get %x : !seaimp.var<i32>
```

- `!seaimp.var<T>`: the handle of a variable holding a `T`. `T` is a
  signless integer, a `!sea.ptr`, a `!sea.struct` or a `!sea.mem` (a
  region, below). `!sea.addr` is refused (sea has no nondet address op for
  the initial value).
- `seaimp.var "<name>" [<site>]` declares a variable. `get` reads the whole
  value, `set` replaces it. A memory variable (a region) has no `get` /
  `set` (the verifier refuses them): seaimp code never holds a memory
  version, and memory is reached only through the region ops below.
- A `get` that no `set` reaches reads the variable's initial value, the
  same at every such read (like LLVM's `freeze undef`, not `undef`; sea has
  no undef). The lowering makes it:
  - integers: `sea.nondet "<name>" [<site>]`;
  - structs: `sea.struct.nondet "<name>" [<site>]` (one bit-vector of the
    integer and address leaves; pointer leaves are null, as for every
    `sea.struct.nondet`);
  - pointers: `sea.ptr.null`;
  - memories: `sea.mem.nondet "<name>" [<site>]` (unconstrained contents,
    as SeaHorn's export starts each region).

  Integers start nondet; pointers start null. Programs are expected to set
  a variable before they get it; a pointer read before it is set is not
  given a meaning beyond null.
- Effects: `var` allocates its result, `get` reads and `set` writes the
  handle. An unused `get` is dead to upstream DCE; CSE does not merge
  `get`s across a `set`.

## Regions

A variable of memory type is a region. The region ops name the variable
that holds the memory and have the operands and attributes of their sea
counterparts, minus the memory operand, the memory result and `inplace`:

```mlir
!m = !sea.mem<!sea.ptr<64>, i8>
%mem = seaimp.var "mem" [0] : !seaimp.var<!m>
%p = seaimp.alloca %mem, %c4 align 4 [1] : !seaimp.var<!m>, i64 -> !sea.ptr<64>
seaimp.store %mem[%p], %v align 4 : !seaimp.var<!m>, i32
%x = seaimp.load %mem[%p] align 4 : !seaimp.var<!m> -> i32
seaimp.memset %mem[%buf], %z8, %c16 : !seaimp.var<!m>
seaimp.memmove %mem[%dst], %mem[%buf], %c8 : !seaimp.var<!m>
seaimp.memhavoc %mem[%buf], %c16 [5] : !seaimp.var<!m>
%f = seaimp.is_modified %mem[%p] : !seaimp.var<!m>
seaimp.reset_modified %mem[%p] : !seaimp.var<!m>
seaimp.print.mem "v" %mem[%p], %n fmt "x" : !seaimp.var<!m>, i64 -> i8
```

- Each op is defined by a local expansion: `get` the memory, apply the sea
  op `inplace`, `set` the result (the reads `seaimp.load`, `is_modified`,
  `print.mem`: `get`, then the sea op). The index, length and unit types
  are the region's (`TypesMatchWith` on the variable's memory type); a
  loaded or stored value spans whole cells and `align` is a power of two,
  as for the sea ops.
- `seaimp.memmove` names a destination and a source variable of the same
  type. With one region they are the same variable (one `get`); with two
  the source's version is read and only the destination's chain changes.
- `seaimp.memhavoc` keeps its site; it is unique with the other nondet
  sites.
- Modification tracking is part of the specification language (SeaHorn's
  `sea_is_modified` / `sea_reset_modified`; SeaHorn exports produce
  them): `seaimp.is_modified` reads the region, `seaimp.reset_modified`
  updates it (the contents and the other objects' state stay). `tracked`
  on `store` / `memset` / `memmove` / `memhavoc` is kept as written; a
  program usually marks tracking with `sea.tracking_on` / `tracking_off`,
  which are positional, and `sea-apply-tracking` sets `tracked` after the
  lowering.
- `seaimp.print.mem` is the strong `sea.print.mem` (docs/design/Print.md)
  on the region: it shows the contents at its point. `fmt`, `count` and
  `offset` are keywords here; the weak `sea.print.mem` names memories,
  not versions, and is used as it is.
- `seaimp.load` and `seaimp.is_modified` may stand in a `sea.box.all` body
  directly in the variable's region (the body only reads). Their lowering
  puts the `get` right before the box op, in the variable's region: the
  body cannot write memory, so the version before the box is the one
  every index reads, and Mem2Reg sees no access in a nested region. A box
  admits them through the sea interface `BoxBodyReadOpInterface`.
- Whole-region havoc has no region form.

### Every version is used linearly

`inplace` on an update promises that the consumed version is dead
afterwards. The lowering writes it on every update it makes, and it is
true by construction: seaimp code never holds a memory version (no `get` /
`set` of a memory variable in input), so the only memory values in the
expanded program are the ones the expansion makes. Each `get` it makes is
used by exactly one op, the sea op made with it, right after it (or in
the box body right after it), with no `set` between; each `set` stores the
result of a fresh update. Mem2Reg then replaces each `get` by the reaching
definition at its point. If an update at point P consumes version V, the
`set` right after P makes the new version the reaching definition on
every path from P, so no `get` reached from P sees V again, and no branch
operand passes V on (a branch forwards the reaching definition at its
end). A memmove reads its source before it writes, as the sea op does.
A hand-written `get` used after a later region update would have broken
this; that is why `get` / `set` of a memory variable are refused.

### Allocation names its region

`seaimp.alloca %region, %n align A [site] (label L)?` allocates a stack
object whose cells are in `%region`; `seaimp.malloc` (same operands) a
heap object; `seaimp.global %region @g size N align A (label L)?` denotes
the object of global `@g`. The result is the pointer type that indexes the
region (so the region is pointer-indexed). They lower to `sea.alloca` /
`sea.malloc` / `sea.global` with the same operands and attributes: sea
allocation takes no memory operand (pointers are object + offset, not
tied to a memory), so the region is dropped. The region is the
information a later partition into several regions checks accesses
against (the premise that every object is accessed through its region
only). The verifiers are the sea ops' (size of the pointer width,
alignment a power of 2, sites unique); the uses of one global name agree
in size and alignment over `sea.global` and `seaimp.global` alike
(`sea.program`'s verifier, through `GlobalObjectOpInterface`).
`sea.alloca`, `sea.malloc` and `sea.global` stay usable in seaimp
programs as they are.

### Why an expansion, not Mem2Reg interfaces on the region ops

The region ops could implement `PromotableMemOpInterface` themselves, as
`LLVM::StoreOp::getStored` builds an insert on the reaching definition: a
store's `getStored` would build `sea.store` on the reaching memory, a
load's `removeBlockingUses` a `sea.load`. That puts Mem2Reg code on every
region op, and it does not cover a memmove between two regions: its source
is a load from a second slot, whose reaching definition Mem2Reg hands to
`removeBlockingUses` of that slot, not to the destination's `getStored`.
The expansion keeps the region ops free of interface code, gives the
two-region memmove for free (two `get`s), and leaves Mem2Reg seeing only
`get` and `set`, whose promotion the verifier already makes total. The
cost is one `get` / `set` pair per access before Mem2Reg removes them.

## No runtime semantics

seaimp has meaning only through its lowering to sea. The interpreter,
`Semantics.cpp` and `utils/check-semantics.py` do not know seaimp ops; this
is an exception to the project's rule that a new op gets interpreter
semantics and an oracle entry, on purpose: the lowering is the definition,
and the lowered program is what runs and what the VC encodes.

- `sea-run` refuses a module with a seaimp op (`run.seaimp`): lower it
  first with `sea-opt --sea-lower-seaimp`.
- `sea-vc` without `--lower` refuses it (`vc.seaimp`). With `--lower`, it
  runs `sea-lower-seaimp` on every program right after parsing, before it
  reads the program for anything else (the pointer mode, the copies kept
  for `--replay-original` and for the replay of `--seam` answers are taken
  from the lowered program). `sea-lower-seaimp` is also the first pass of
  the VC lowering (`sea-opt --sea-vc-lower`); in sea-vc it then finds
  nothing to do.

A counterexample names the initial value of a variable by its site, as for
any nondet: `nondet: site=<site> callee=<name> ...`.

## Sites

The site of a `seaimp.var` is unique together with the program's other
nondet sites: `sea.program`'s verifier checks every op of the
`NondetSiteOpInterface` (the sea nondet ops, memhavoc, allocations and
`seaimp.var`). The lowering makes at most one nondet op per integer or struct
variable, with the variable's site, only if some `get` reads the initial value, and the
variable is gone afterwards: the lowered program's sites stay unique.

## Verifier: promotion cannot fail

Upstream Mem2Reg (`tryToPromoteMemorySlots`) gives up on a slot without a
diagnostic when it meets a use it cannot rewrite. The verifier admits only
variables it promotes:

- `seaimp.var` is in the entry block of a `sea.program`. The initial value
  is made at the start of that block and dominates every use.
- The handle is used only as the variable operand of seaimp ops (`get`,
  `set`, the region ops, the allocations; no block argument, no select,
  no print, no op of another dialect).
- seaimp ops take a handle defined by a `seaimp.var` in their own region
  (not in a nested region), except the region reads `seaimp.load` /
  `is_modified` in a `sea.box.all` body directly in it, which the
  expansion moves out of the body.
- The value types of `get` and `set` are the variable's `T`, which is not
  a memory.

The program's region is a CFG of `sea.br` / `sea.cond_br`, which implement
`BranchOpInterface`, so Mem2Reg can add the block arguments it needs at
the joins.

## Lowering

`sea-lower-seaimp` (an `OperationPass<ProgramOp>`) first expands every
region op into `get` / the sea op `inplace` / `set` (a read in a
`sea.box.all` body: its `get` before the box) and every allocation into
its sea op. These `get` / `set` of memory exist only inside the pass.
Then it collects the `seaimp.var` ops and calls `tryToPromoteMemorySlots`
once with all of them. The interfaces:

- `seaimp.var`: `PromotableAllocationOpInterface`, one slot `{handle, T}`;
  `getDefaultValue` makes the initial value above;
  `handlePromotionComplete` erases an unused initial value and the
  variable.
- `seaimp.get`: `PromotableMemOpInterface`, loads; its result is replaced
  by the reaching definition.
- `seaimp.set`: `PromotableMemOpInterface`, stores its operand.

Joins get one block argument per variable that differs on the incoming
edges (the iterated dominance frontier of the sets; Mem2Reg removes the
arguments it does not need). Loops work: the header gets an argument fed
by the entry edge and the back edge.

```mlir
// x = a; if (a < b) x = b; assert(x >= a)
sea.program @max {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  %a = sea.nondet "nd_int" [2] : i32
  %b = sea.nondet "nd_int" [3] : i32
  seaimp.set %x, %a : !seaimp.var<i32>
  %lt = sea.icmp slt, %a, %b : i32
  sea.cond_br %lt, ^bb1, ^bb2
^bb1:
  seaimp.set %x, %b : !seaimp.var<i32>
  sea.br ^bb2
^bb2:
  %m = seaimp.get %x : !seaimp.var<i32>
  %ok = sea.icmp sge, %m, %a : i32
  sea.assert %ok
  sea.halt
}
```

lowers to

```mlir
  sea.cond_br %lt, ^bb1, ^bb2(%a : i32)
^bb1:
  sea.br ^bb2(%b : i32)
^bb2(%m : i32):
  %ok = sea.icmp sge, %m, %a : i32
```

Contract: any verified program is accepted; no seaimp op is left. The
pass checks it (`sea-lower-seaimp.postcondition`: a refusal of Mem2Reg on a
verified program is a bug in the verifier or the pass, reported as an
error, never a half-lowered program). A program without seaimp ops is
unchanged.

Cost: Mem2Reg analyzes and rewrites each variable on its own and walks
the program's ops per variable: O(variables x ops).

A memory variable lowers to the Mem-SSA chain SeaHorn's export makes: one
`sea.mem.nondet` with the variable's name and site, a linear chain of
`inplace` updates, memory block arguments at the joins:

```mlir
// int o; int *p = &o; *p = 5; if (c) *p = 7; assert(*p >= 5)
  %m0 = sea.mem.nondet "mem" [0] : !m
  %p = sea.alloca %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %m1 = sea.store %m0[%p], %v5 inplace align 4 : !m, i32
  sea.cond_br %c, ^bb1, ^bb2(%m1 : !m)
^bb1:
  %m2 = sea.store %m1[%p], %v7 inplace align 4 : !m, i32
  sea.br ^bb2(%m2 : !m)
^bb2(%m3 : !m):
  %x = sea.load %m3[%p] align 4 : !m -> i32
```

## Erasing memory SSA

`sea-erase-mem-ssa` (sea -> seaimp) is the inverse of the region part of
the lowering, for a program with one memory (one class of memory values
connected by def-use: an update and the version it consumes, a branch
operand and its block argument). The chain becomes the variable `%mem` and
region ops on it; the memory block arguments and the branch operands that
feed them go away. `sea-erase-mem-ssa` followed by `sea-lower-seaimp` is
an equivalent program: every access reads and writes the version it did
before (with `inplace` on every update). Every existing program with one
memory is then an input for testing the lowering, and the pass is the
reference for what an export with erased memory SSA looks like.

The chain must be linear. The pass computes the current version at every
point (a forward dataflow over the CFG: a block's memory argument, else
the version all its reachable predecessors leave, else none; a root or an
update makes a new one) and requires that every op that reads or consumes
a version (also in a `sea.box.all` body), and every branch operand, uses
the current one. Refused, with an error and no change:

- `erase-mem-ssa.non-linear`: a load (or is_modified, strong print.mem)
  of an earlier version, a version consumed twice on one path, a version
  read after a join whose predecessors leave different versions with no
  block argument, an earlier version passed to a block argument, a block
  with two memory arguments;
- `erase-mem-ssa.regions`: more than one memory (several regions come
  from a partition, later);
- `erase-mem-ssa.unsupported`: a memory that does not start at one
  `sea.mem.nondet` in the entry block (a `sea.mem.const` root, a second
  root, no root: a region starts as its variable's nondet initial value,
  and seaimp code cannot set a memory), a memory value made by an op that
  is no root or update or in a nested region, a reader without a region
  form.

The rewrite gives every reader and update its region op, so the output
holds no memory value:

- `sea.load` / `store` / `memset` / `memmove` / `memhavoc` (`tracked`
  kept), `sea.mem.is_modified` / `reset_modified`, a strong
  `sea.print.mem` -> the region op (`inplace` dropped; the lowering puts
  it back, and linearity is what makes it true);
- a `sea.load` / `sea.mem.is_modified` in a `sea.box.all` body -> the
  region op in the body;
- the root, a `sea.mem.nondet` in the entry block: the variable takes its
  name and site (the lowering makes the same op at the start of the entry
  block);
- `sea.alloca` whose pointer type indexes the memory -> `seaimp.alloca
  %mem`. `sea.malloc` and `sea.global` stay as they are for now; erase may
  turn them into `seaimp.malloc` / `seaimp.global` later, when the stack /
  heap distinction is supported.

Unreachable blocks are rewritten but not checked. Postcondition
(`sea-erase-mem-ssa.postcondition`): no memory value is left.

`test/VC/erase-mem-ssa-corpus.test` runs a fixed sample of the VC tests
with one memory through the pass and compares the queries with
`utils/encoding-diff.py` (identical).

## Not supported

- Address variables (`!sea.addr`).
- An address-of: a variable whose address is taken is an object in a
  region (`seaimp.alloca`).
- `get` / `set` of a memory variable.
- Region accesses in nested regions other than reads in a `sea.box.all`
  body.

## Not yet

- Several regions in `sea-erase-mem-ssa` (from a partition, later).
- A region on `sea.alloca` itself (the region of an object in sea; from
  the memory SSA form this is direct).
- Whole-region havoc, and a region form for a memory that starts as
  `sea.mem.const` (the erase pass refuses such programs).
- `sea-erase-mem-ssa` turning `sea.malloc` / `sea.global` into
  `seaimp.malloc` / `seaimp.global`.
