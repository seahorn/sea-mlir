# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_mlir_dialect(SeaOps sea)
add_mlir_doc(SeaOps SeaOps Sea/ -gen-op-doc)

set(LLVM_TARGET_DEFINITIONS SeaOps.td)
mlir_tablegen(SeaEnums.h.inc -gen-enum-decls)
mlir_tablegen(SeaEnums.cpp.inc -gen-enum-defs)
add_public_tablegen_target(MLIRSeaEnumsIncGen)

set(LLVM_TARGET_DEFINITIONS SeaInterfaces.td)
mlir_tablegen(SeaInterfaces.h.inc -gen-op-interface-decls)
mlir_tablegen(SeaInterfaces.cpp.inc -gen-op-interface-defs)
add_public_tablegen_target(MLIRSeaInterfacesIncGen)
add_dependencies(MLIRSeaOpsIncGen MLIRSeaInterfacesIncGen)
