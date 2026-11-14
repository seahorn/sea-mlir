# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Plain-main test driver of the solver factory (sea/Expr/Smt/SolverFactory.h,
# no gtest); run by test/Tools/solver-factory.test through lit.
add_llvm_executable(sea-solver-factory-test SolverFactoryTest.cpp)
llvm_update_compile_flags(sea-solver-factory-test)
target_link_libraries(sea-solver-factory-test PRIVATE SeaExpr)
