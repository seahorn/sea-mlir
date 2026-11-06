# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_subdirectory(sea-opt)
add_subdirectory(sea-run)
add_subdirectory(sea-expr-eval)
add_subdirectory(sea-vc)
add_subdirectory(sea-reduce)
add_subdirectory(sea-seam-export)

# The release installs the user-facing tools (docs/Release.md; sea-seam-export:
# docs/SeamExport.md); sea-expr-eval and the unittests/ drivers are test
# utilities, built but not installed.
install(TARGETS sea-opt sea-run sea-vc sea-reduce sea-seam-export RUNTIME DESTINATION bin)
