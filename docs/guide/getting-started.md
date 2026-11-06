# First proof

This page runs `sea-run` and `sea-vc` on one small program: a proof, a
counterexample, and the query the solver sees. It assumes the tools are on
`PATH` ([Build and install](build.md)).

## A program

Save this as `double.mlir`:

```mlir
sea.program @main {
  %x = sea.nondet "nondet_u32" [0] : i32
  %c10 = sea.const 10 : i32
  %lt = sea.icmp ult, %x, %c10 : i32
  sea.assume %lt
  %y = sea.add %x, %x : i32
  %c20 = sea.const 20 : i32
  %ok = sea.icmp ult, %y, %c20 : i32
  sea.assert %ok
  sea.halt
}
```

The claim is that if $x < 10$ then $2x < 20$, for 32-bit unsigned $x$.

The parts of a program:

- `sea.nondet "<callee>" [<site>]` is an input. The site number names the
  input everywhere: in the solver's query (`nd.0_0`), in the model, and on
  the command line of `sea-run`. Sites are unique in a program.
- `sea.assume` restricts the executions. An execution whose assumption is
  false is not considered.
- `sea.assert` states the property.
- Values are bit-vectors (`i32`). Every operation is total and follows
  SMT-LIB bit-vector semantics, including the cases C leaves undefined:
  unsigned division by zero gives all ones, and a shift by at least the
  width gives 0 or the sign fill. There is no poison and no undef.

`sea-vc` reads a program in BMC form: no loops, one `sea.halt`, exactly one
`sea.assert` right before it, and no critical edges. `sea-vc --lower`
establishes the parts of the form that a front end does not. Input that is
not in this form is an error.

## Run it

`sea-run` executes the program on given input values. It defines what a
program does.

```console
$ sea-run double.mlir --input=0=5
program: @main
nondet: site=0 callee=nondet_u32 width=32 value=5 origin=input
status: halted
at: loc("double.mlir":10:3)
steps: 9
```

`--input=0=5` gives site 0 the value 5. The run reaches `sea.halt`.

## Verify it

`sea-vc` asks whether any input makes the assertion fail while every
assumption holds.

```console
$ sea-vc double.mlir
tool.version: sea-mlir 0.1.0 (...)
vc.typecheck-time: 0.000
program: @main
...
vc-time: 0.006
...
solve-time: 0.012
result: unsat
```

`unsat` means that no such input exists: the assertion holds for every
$x < 10$. The exit status is 0. The lines left out are statistics;
[Reading the output](output.md) explains them.

## Break it

Change the bound from 10 to 11 and save the program as `double-bad.mlir`.
Now $x = 10$ satisfies the assumption and $2x = 20$ fails the assertion.

```console
$ sea-vc double-bad.mlir
...
result: sat
vc.ackermann-model: defaulted=0 inconsistent=0 stride-broken=0
nondet: site=0 callee=nondet_u32 width=32 value=10
replay: assert-failed
replay: assert-at: double-bad.mlir:8:9
at: loc("double-bad.mlir":9:3)
replay: assert-op: double-bad.mlir:9:3
validate: checked=6 mismatches=0 dontcare=0
```

The solver found $x = 10$. `sea-vc` then ran the program in the interpreter
on that input. The run failed the assertion (`replay: assert-failed`), and
all six values it computed were equal to the solver's model
(`mismatches=0`). Only then is the counterexample reported, with exit
status 0. A model that does not replay gives exit status 2, which is a bug
in sea-mlir. [Counterexamples](counterexamples.md) describes the replay in
detail.

The interpreter reproduces the run from the input alone:

```console
$ sea-run double-bad.mlir --input=0=10
program: @main
nondet: site=0 callee=nondet_u32 width=32 value=10 origin=input
status: assert-failed
at: loc("double-bad.mlir":9:3)
assert-at: double-bad.mlir:8:9
assert-op: double-bad.mlir:9:3
steps: 8
```

## See the query

`--smt2` prints the query instead of solving it. The statistics go to
stderr.

```console
$ sea-vc double.mlir --smt2 2>/dev/null
(declare-fun nd.0_0 () (_ BitVec 32))
(declare-fun ^bb0_0 () Bool)

(assert (let ((a!1 (= (ite (bvult (bvadd nd.0_0 nd.0_0) #x00000014) #b1 #b0) #b1)))
  (and (= (ite (bvult nd.0_0 #x0000000a) #b1 #b0) #b1) ^bb0_0 (not a!1))))
(check-sat)
```

The query names each input by its site (`nd.0_0`) and each block by its
label (`^bb0_0`). The names match the program, so a model can be read
back into it.

## Programs from C

Programs exported from C have memory, pointers and many blocks. They need
the lowering (`--lower`). They usually also run with SeaM (`--seam`), the
object-aware rewriting of loads and stores. The C applications on
this site run with

```bash
sea-vc <job>.mlir --lower --seam --widen-memory=4
```

`--widen-memory=4` turns byte memories into 4-byte words where every
access is an aligned word. These options are not the defaults; see
[Profiles](../agents/profiles.md) for why. The [Applications](../tutorials/index.md)
run this configuration on Firedancer, Stellar and NKI programs.
