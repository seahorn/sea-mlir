# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_mlir_dialect(SeaImpOps seaimp)
add_mlir_doc(SeaImpOps SeaImpOps SeaImp/ -gen-op-doc)
add_dependencies(MLIRSeaImpOpsIncGen MLIRSeaInterfacesIncGen)
