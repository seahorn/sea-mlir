# Variables over sea: the seaimp dialect

`seaimp` (ODS: `include/sea/Dialect/SeaImp/`; verifier and Mem2Reg
interfaces: `lib/Dialect/SeaImp/SeaImpOps.cpp`) is an imperative layer over
sea. It holds what is convenient to write but is not SSA. Today that is
variables: typed slots read and written whole, like LLVM `alloca` +
`load` / `store` of a local. Its lowering `sea-lower-seaimp`
(`lib/Transforms/LowerSeaImp.cpp`) turns them into SSA values and block
arguments with MLIR's Mem2Reg. Tests: `test/Dialect/SeaImp/roundtrip.mlir`,
`test/Dialect/SeaImp/invalid.mlir`, `test/Transforms/lower-seaimp.mlir`,
`test/VC/seaimp.mlir`.

Status: variables of register types (integers, pointers, structs).
Memory regions and allocation in a region come with the region ops; they
are not implemented.

## Ops

```mlir
%x = seaimp.var "x" [7] : !seaimp.var<i32>
seaimp.set %x, %v : !seaimp.var<i32>
%y = seaimp.get %x : !seaimp.var<i32>
```

- `!seaimp.var<T>`: the handle of a variable holding a `T`. `T` is a
  signless integer, a `!sea.ptr` or a `!sea.struct`. `!sea.addr` is refused
  (sea has no nondet address op for the initial value); `!sea.mem` is
  refused until the region ops exist.
- `seaimp.var "<name>" [<site>]` declares a variable. `get` reads the whole
  value, `set` replaces it.
- A `get` that no `set` reaches reads the variable's initial value: one
  nondeterministic value named by the variable's name and site, the same at
  every such read (like LLVM's `freeze undef`, not `undef`; sea has no
  undef). The lowering makes it:
  - integers: `sea.nondet "<name>" [<site>]`;
  - structs: `sea.struct.nondet "<name>" [<site>]` (one bit-vector of the
    integer and address leaves; pointer leaves are null, as for every
    `sea.struct.nondet`);
  - pointers: `sea.ptr.nondet "<name>" [<site>]` (a pointer to a fresh
    object of size 0: not null, never dereferenceable).
- Effects: `var` allocates its result, `get` reads and `set` writes the
  handle. An unused `get` is dead to upstream DCE; CSE does not merge
  `get`s across a `set`.

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
`seaimp.var`). The lowering makes at most one nondet op per variable, with
the variable's site, only if some `get` reads the initial value, and the
variable is gone afterwards: the lowered program's sites stay unique.

## Verifier: promotion cannot fail

Upstream Mem2Reg (`tryToPromoteMemorySlots`) gives up on a slot without a
diagnostic when it meets a use it cannot rewrite. The verifier admits only
variables it promotes:

- `seaimp.var` is in the entry block of a `sea.program`. The initial value
  is made at the start of that block and dominates every use.
- The handle is used only as the variable operand of `seaimp.get` /
  `seaimp.set` (no block argument, no select, no print, no op of another
  dialect).
- `get` / `set` take a handle defined by a `seaimp.var` in their own region
  (not in a nested region such as a `sea.box.all` body).
- The value types of `get` and `set` are the variable's `T`.

The program's region is a CFG of `sea.br` / `sea.cond_br`, which implement
`BranchOpInterface`, so Mem2Reg can add the block arguments it needs at
the joins.

## Lowering

`sea-lower-seaimp` (an `OperationPass<ProgramOp>`) collects the
`seaimp.var` ops and calls `tryToPromoteMemorySlots` once with all of
them. The interfaces:

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

## Not supported

- Memory variables (`!sea.mem`) and the region accesses that would use
  them; allocation in a region comes with those ops.
- Address variables (`!sea.addr`).
- An address-of: a variable whose address is taken is an object
  (`sea.alloca`) in memory, not a `seaimp.var`.
- Uses in nested regions (`sea.box.all` bodies).
