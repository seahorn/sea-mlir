# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

if(NOT LLVM_EXTERNAL_LIT)
  find_program(LLVM_EXTERNAL_LIT NAMES lit llvm-lit REQUIRED)
endif()
find_program(SEA_Z3 z3)

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
  )

set(SEA_TEST_DEPENDS sea-opt sea-reduce sea-run sea-expr-eval sea-vc sea-seam-export sea-expr-rewriter-test sea-addr-range-test sea-log-test sea-expr-core-test sea-expr-api-test sea-join-cases-test)
if(SEA_WITH_YICES)
  list(APPEND SEA_TEST_DEPENDS sea-seam-rules-test sea-bv-rules-test sea-store-map-test sea-memops-test sea-ackermann-test)
endif()

add_lit_testsuite(check-sea "Running the sea-mlir regression tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${SEA_TEST_DEPENDS}
  )
add_lit_testsuites(SEA ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS ${SEA_TEST_DEPENDS})
