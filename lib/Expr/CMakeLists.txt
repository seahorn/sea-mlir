# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Expr and its SMT backends, ported from SeaHorn (include/sea/Expr/README.md).
# Expr is used for SMT terms only.
# The Bitwuzla backend is optional (BITWUZLA_ROOT, see the top-level
# CMakeLists.txt): its sources are always listed (LLVM's CMake wants every
# source of the directory listed) but are empty without WITH_BITWUZLA.
set(bitwuzla_srcs BitwuzlaModelImpl.cpp BitwuzlaSolverImpl.cpp
    MarshalBitwuzla.cpp)
if(SEA_WITH_BITWUZLA)
  # Bitwuzla reports errors by exceptions (LLVM builds without them): these
  # files catch them at the solver / model boundary and report through
  # sea::diag, so none escapes into the rest of sea-mlir.
  set_source_files_properties(${bitwuzla_srcs} PROPERTIES
    COMPILE_FLAGS "-fexceptions -frtti")
endif()

add_mlir_library(SeaExpr
  ExprCore.cpp
  ExprToZ.cpp
  ArithShape.cpp
  ExprUtil.cpp
  ConstArrayDefs.cpp
  MarshalYices.cpp
  TypeChecker.cpp
  Yices2ModelImpl.cpp
  Yices2SolverImpl.cpp
  ZToExpr.cpp
  ZToExprNoRec.cpp
  ${bitwuzla_srcs}

  LINK_LIBS PUBLIC
  SeaSupport
  LLVMSupport
  )
target_link_libraries(SeaExpr PUBLIC
  ${Z3_LIBRARY} ${GMPXX_LIBRARY} ${GMP_LIBRARY})
# The object library compiles the sources; give both the include paths.
foreach(t SeaExpr obj.SeaExpr)
  target_include_directories(${t} SYSTEM PUBLIC
  ${Boost_INCLUDE_DIRS} ${GMP_INCLUDE_DIR} ${Z3_INCLUDE_DIR})
endforeach()

# The Yices backend is optional (SEA_ENABLE_YICES, top-level CMakeLists.txt):
# its sources are empty without WITH_YICES2.
if(SEA_WITH_YICES)
  target_link_libraries(SeaExpr PUBLIC ${YICES2_LIBRARY})
  foreach(t SeaExpr obj.SeaExpr)
    target_include_directories(${t} SYSTEM PUBLIC ${YICES2_INCLUDE_DIR})
    target_compile_definitions(${t} PUBLIC WITH_YICES2)
  endforeach()
endif()

if(SEA_WITH_BITWUZLA)
  # tools run without LD_LIBRARY_PATH
  get_filename_component(BITWUZLA_LIBRARY_DIR ${BITWUZLA_LIBRARY} DIRECTORY)
  target_link_libraries(SeaExpr PUBLIC ${BITWUZLA_LIBRARY})
  target_link_options(SeaExpr PUBLIC "LINKER:-rpath,${BITWUZLA_LIBRARY_DIR}")
  foreach(t SeaExpr obj.SeaExpr)
    target_include_directories(${t} SYSTEM PUBLIC ${BITWUZLA_INCLUDE_DIR})
    target_compile_definitions(${t} PUBLIC WITH_BITWUZLA)
  endforeach()
endif()
