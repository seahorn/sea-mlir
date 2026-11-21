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
`test/VC/seaimp-mem.mlir`.

Status: variables of register types (integers, pointers, structs) and of
memory type (regions) with the region accesses and allocation in a region;
erasing the memory SSA of a program with one memory. Not yet: several
regions from a partition, a global form of allocation, `is_modified` /
`reset_modified` / whole-region havoc as region ops.

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
  value, `set` replaces it.
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
```

- Each op is defined by a local expansion: `get` the memory, apply the sea
  op `inplace`, `set` the result (`seaimp.load`: `get`, then load). The
  index, length and unit types are the region's (`TypesMatchWith` on the
  variable's memory type); a loaded or stored value spans whole cells and
  `align` is a power of two, as for the sea ops.
- `seaimp.memmove` names a destination and a source variable of the same
  type. With one region they are the same variable (one `get`); with two
  the source's version is read and only the destination's chain changes.
- `seaimp.memhavoc` keeps its site; it is unique with the other nondet
  sites.
- `tracked` is not written in seaimp: `sea.tracking_on` / `tracking_off`
  are positional and `sea-apply-tracking` sets it after the lowering.
  `is_modified`, `reset_modified` and whole-region havoc have no region
  form; a program writes them on a `get` (and `set`s the result).
- `inplace`: a chain made of region ops is linear by construction, so every
  lowered update is `inplace`. A `get` of a memory whose value is used
  after a later region op on the same variable breaks that promise, as
  hand-written sea can; seaimp has no checker for it (the interpreter
  checks the lowered program on the path it runs).

### Allocation names its region

`seaimp.alloca %region, %n align A [site] (label L)?` allocates a stack
object whose cells are in `%region`; its result is the pointer type that
indexes the region (so the region is pointer-indexed). It lowers to
`sea.alloca` with the same size, alignment, site and label: sea allocation
takes no memory operand (pointers are object + offset, not tied to a
memory), so the region is dropped. The region is the information a later
partition into several regions checks accesses against (the premise that
every object is accessed through its region only). `sea.alloca`,
`sea.malloc` and `sea.global` stay usable in seaimp programs as they are;
a global form of `seaimp.alloca` comes when a program needs one.

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
  `set`, the region ops, `alloca`; no block argument, no select, no print,
  no op of another dialect).
- seaimp ops take a handle defined by a `seaimp.var` in their own region
  (not in a nested region such as a `sea.box.all` body).
- The value types of `get` and `set` are the variable's `T`.

The program's region is a CFG of `sea.br` / `sea.cond_br`, which implement
`BranchOpInterface`, so Mem2Reg can add the block arguments it needs at
the joins.

## Lowering

`sea-lower-seaimp` (an `OperationPass<ProgramOp>`) first expands every
region op into `get` / the sea op `inplace` / `set` and every
`seaimp.alloca` into `sea.alloca`. Then it collects the `seaimp.var` ops
and calls `tryToPromoteMemorySlots` once with all of them. The interfaces:

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
a version, and every branch operand, uses the current one. Refused, with
an error and no change:

- `erase-mem-ssa.non-linear`: a load of an earlier version, a version
  consumed twice on one path, a version read after a join whose
  predecessors leave different versions with no block argument, a block
  with two memory arguments;
- `erase-mem-ssa.regions`: more than one memory (several regions come
  from a partition, later);
- `erase-mem-ssa.unsupported`: a memory value made by an op that is no
  root or update, or in a nested region.

The rewrite:

- `sea.load` / `store` / `memset` / `memmove` / `memhavoc` -> the region
  op (`inplace` dropped; the lowering puts it back, and linearity is what
  makes it true);
- a `tracked` update and `sea.mem.reset_modified` -> `seaimp.get`, the op
  on that value, `seaimp.set` of the result (no region form keeps
  `tracked`);
- any other reader (`sea.mem.is_modified`, a strong `sea.print.mem`, a
  `sea.load` in a `sea.box.all` body) reads a `seaimp.get` put before it
  (before the top-level op that holds it: the body only reads);
- the root: if the only root is a `sea.mem.nondet` in the entry block, the
  variable takes its name and site (the lowering makes the same op at the
  start of the entry block). Otherwise every root (`sea.mem.const`, a
  `sea.mem.nondet` in a loop, several roots) stays and is followed by
  `seaimp.set`; the variable is `"mem"` with a fresh site and its initial
  value is not read;
- `sea.alloca` whose pointer type indexes the memory -> `seaimp.alloca
  %mem`. `sea.malloc` and `sea.global` stay (no region form yet).

Unreachable blocks are rewritten but not checked. Postcondition
(`sea-erase-mem-ssa.postcondition`): no memory block argument is left, and
every remaining memory value is a `seaimp.get` result or is used only by
`seaimp.set`.

## Not supported

- Address variables (`!sea.addr`).
- An address-of: a variable whose address is taken is an object in a
  region (`seaimp.alloca`).
- Uses in nested regions (`sea.box.all` bodies).
- Several regions in `sea-erase-mem-ssa`; a global or heap form of
  allocation in a region.
