# Firedancer: HPACK

Firedancer's HTTP/2 client decodes HPACK headers. Header strings are
Huffman-coded, and Firedancer decodes them with nghttp2's decoder. This
page proves the decoder's contract for inputs of any length, then breaks
the decoder and reads the counterexample, first as text and then as JSON.

## The question

For every input of length $n$ and every valid starting state, does
`nghttp2_hd_huff_decode` write at most $2n$ bytes, leave a valid state,
and return $n$, or an error only on a final call?

## The contract

The harness calls the decoder once, from an arbitrary valid state, on an
input whose length $n$ is arbitrary up to `NMAX`, and asserts four claims:

- the decoder writes at most $2n$ bytes;
- the state after the call is valid;
- the call returns $n$, or an error only if it is the final call;
- on an empty input the state is unchanged.

## Cutting the loop

The decoder loops over its input, and the input has any length up to
`NMAX`, so unrolling the loop to the export bound would not cover it. The
proof jobs replace the loop by an
[inductive cut](../guide/proof-controls.md#inductive-cuts). With $r$ the
read position, $w$ the write position, and $r_0$, $w_0$ their values at
entry, the invariant is

$$
r_0 \le r \le \mathit{end}, \qquad
w \ge w_0, \qquad
w - w_0 \le 2\,(r - r_0),
$$

the decoder state is valid, and it is the initial state while $r = r_0$.
The code after the loop sees an arbitrary state that satisfies the
invariant and has $r = \mathit{end}$.

## The proof

```console
$ sea-vc firedancer/hpack-huff-proof.mlir --lower --seam --widen-memory=4
...
object: name=obj.global.hp_in order=6 kind=global base=134512672 size=4096 align=16
object: name=obj.global.hp_out order=7 kind=global base=134516768 size=8192 align=16
object: name=obj.global.huff_decode_table order=8 kind=global base=134524960 size=16448 align=16
vc-time: 0.021
...
seam.inb: assumed accesses=51 conditions=9
solve-time: 0.424
result: unsat
```

The contract holds for every input length up to `NMAX`, under the
assumption that the 51 reported accesses stay in their objects.

## A mutant

The mutant `hpack-huff-mut-extra` makes the decoder write an extra zero
byte for each input byte, three bytes where the original writes at most
two.

```console
$ sea-vc firedancer/hpack-huff-mut-extra.mlir --lower --seam --widen-memory=4
...
seam.inb: assumed accesses=52 conditions=10
solve-time: 0.210
result: sat
...
memread: site=5 addr=134512672 width=8 value=192 obj=&global.hp_in[0]
...
nondet: site=9 callee=nd_ulong width=64 value=1
...
replay: assert-failed
replay: assert-at: OUT/nghttp2_hd_huffman.c:59:7
...
validate: checked=157 mismatches=0 dontcare=1
```

One input byte is enough: $n = 1$ and `hp_in[0]` = `0xc0`. The replay
fails inside the decoder's loop, in the copy of `nghttp2_hd_huffman.c`
that carries the cut (`OUT/` is the export's output directory, where the
modified sources are written). The claims are in the harness, so the
failing check is the cut's invariant: after one input byte the mutant has
written three, and $w - w_0 \le 2\,(r - r_0)$ no longer holds. This is a
run of the cut program, which starts the iteration from a havocked state
that satisfies the invariant.
[Counterexamples](../guide/counterexamples.md#reading-a-counterexample)
reads this output line by line.

## The same run as JSON

An agent asks for JSON and selects what it needs. `--config=seam` loads
the shipped profile that sets `--lower --seam`:

```console
$ sea-vc firedancer/hpack-huff-mut-extra.mlir --config=seam --widen-memory=4 --format=jsonl > run.jsonl
$ jq -r 'select(.kind=="stat" and .key=="vc.config").value' run.jsonl
.../profiles/seam.cfg
$ jq -c 'select(.kind=="stat" and .key=="seam.inb") | {value, accesses, conditions}' run.jsonl
{"value":"assumed","accesses":52,"conditions":10}
$ jq -r 'select(.kind=="result").value' run.jsonl
sat
$ jq -c 'select(.kind=="replay") | {status, at}' run.jsonl
{"status":"assert-failed","at":"OUT/nghttp2_hd_huffman.c:59:7"}
$ jq -c 'select(.kind=="validate") | {checked, mismatches}' run.jsonl
{"checked":157,"mismatches":0}
```

The run records which profile file it loaded (`vc.config`).

## The decode table

The model in the counterexample also chose entries of
`huff_decode_table`:

```text
memread: site=7 addr=134541392 width=8 value=0 obj=&global.huff_decode_table[16432]
```

The table is a constant in nghttp2, but the exporter drops initializers
over 16 KiB, and the table has 16448 bytes. The exported program leaves
the table unconstrained. The proof assumes, at each read, the one property
of an entry that it needs: the entry's next state is valid. This property
was checked natively over all entries of the real table. The proof
therefore holds for any table with that property, and a counterexample
may use entries that differ from nghttp2's. This is an exporter gap
([Known limitations](../reference/limitations.md)).

## The other HPACK jobs

The family has 22 HPACK jobs: the Huffman decoder (`hpack-huff-*`), the
skip function `fd_hpack_skip` (`hpack-skip-*`), and the reader
`fd_hpack_rd_next` (`hpack-rd-*`), each with a proof, reach jobs and
mutants. Two jobs revert parts of Firedancer commit `791176436`; both give
counterexamples. The `fd_hpack_rd_next` proof took 16 s when the
expectations were established.
