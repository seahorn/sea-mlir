# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

if(NOT LLVM_EXTERNAL_LIT)
  find_program(LLVM_EXTERNAL_LIT NAMES lit llvm-lit REQUIRED)
endif()
find_program(SEA_Z3 z3)
# The command-line solvers of the optional backends' installs (tests that
# read a --smt2 query with them: REQUIRES: yices-bin / bitwuzla-bin); the
# install's own first, as for the libraries
if(YICES2_ROOT)
  find_program(SEA_YICES_SMT2 yices-smt2 HINTS ${YICES2_ROOT}/bin NO_DEFAULT_PATH)
endif()
if(BITWUZLA_ROOT)
  find_program(SEA_BITWUZLA_BIN bitwuzla HINTS ${BITWUZLA_ROOT}/bin NO_DEFAULT_PATH)
endif()

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
  )

set(SEA_TEST_DEPENDS sea-opt sea-reduce sea-run sea-expr-eval sea-vc sea-seam-export sea-expr-rewriter-test sea-addr-range-test sea-log-test sea-timer-test sea-stats-test sea-solver-factory-test sea-expr-core-test sea-expr-api-test sea-small-arith-test sea-join-cases-test sea-int-ranges-test sea-diff-decider-test sea-fold-top-eqs-test sea-canonical-sums-test)
if(SEA_WITH_YICES)
  list(APPEND SEA_TEST_DEPENDS sea-seam-rules-test sea-bv-rules-test sea-store-map-test sea-memops-test sea-ackermann-test)
endif()

add_lit_testsuite(check-sea "Running the sea-mlir regression tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${SEA_TEST_DEPENDS}
  )
add_lit_testsuites(SEA ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS ${SEA_TEST_DEPENDS})
