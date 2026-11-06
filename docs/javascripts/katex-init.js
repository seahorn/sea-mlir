// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Render arithmatex output (generic mode) with KaTeX, also after instant navigation.
function renderMath() {
  if (typeof renderMathInElement !== "function") return;
  document.querySelectorAll(".arithmatex").forEach((el) => {
    renderMathInElement(el, {
      delimiters: [
        { left: "\\(", right: "\\)", display: false },
        { left: "\\[", right: "\\]", display: true },
      ],
    });
  });
}
if (typeof document$ !== "undefined") {
  document$.subscribe(renderMath);
} else {
  window.addEventListener("load", renderMath);
}
