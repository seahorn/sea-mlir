# sea-seam-export

Exports the SeaM terms of a straight-line `sea.program` (raw, per `sea-vc`
configuration, after the solver boundary) and the rewriter's rule firings,
as JSON for the Lean conformance checks of seam-lean. Format, lanes and
status codes: [docs/SeamExport.md](../../docs/SeamExport.md); `--agent`.
Tests: `test/SeamExport/`.
