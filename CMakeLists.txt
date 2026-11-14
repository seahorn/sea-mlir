# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.20.0)
# the release number (docs/Release.md); `git describe` is added at build time
project(sea-mlir VERSION 0.1.0 LANGUAGES CXX C)
set(SEA_GIT_DESCRIBE "" CACHE STRING
  "override `git describe` in --version (builds outside a git checkout)")

set(CMAKE_BUILD_WITH_INSTALL_NAME_DIR ON)
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to conform to")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LLVM_LIT_ARGS "-sv -j8" CACHE STRING "Default options for lit")
get_property(LLVM_LIT_ARGS_CACHED CACHE LLVM_LIT_ARGS PROPERTY VALUE)
find_package(MLIR REQUIRED CONFIG)
message(STATUS "Using MLIRConfig.cmake in: ${MLIR_DIR}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# LLVMConfig.cmake (find_package above) sets a normal LLVM_LIT_ARGS "-sv" that
# shadows the cache entry, so -DLLVM_LIT_ARGS was ignored. Restore the cache
# value. Default "-sv -j8" (not one lit worker per core of a shared machine);
# override with -DLLVM_LIT_ARGS=...
set(LLVM_LIT_ARGS "${LLVM_LIT_ARGS_CACHED}")
set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/bin)
set(LLVM_LIBRARY_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/lib)
set(MLIR_BINARY_DIR ${CMAKE_BINARY_DIR})

list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

# An install is the tools, profiles, sea-reduce-test and the docs: no C++
# libraries or headers (no API is released; add_mlir_library would install
# the static libraries otherwise)
set(LLVM_INSTALL_TOOLCHAIN_ONLY ON)
# Installed tools keep the RUNPATH of the solver libraries they linked
# (Z3_ROOT, YICES2_ROOT, BITWUZLA_ROOT outside the system directories), as
# the build tree does: an install runs in place without LD_LIBRARY_PATH.
# -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF for a relocatable install whose
# solvers come from the system or LD_LIBRARY_PATH (docs/Release.md).
if(NOT DEFINED CMAKE_INSTALL_RPATH_USE_LINK_PATH)
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
endif()

include(TableGen)
include(AddLLVM)
include(AddMLIR)
include(HandleLLVMOptions)

set(SEA_SOURCE_DIR ${PROJECT_SOURCE_DIR})
set(SEA_BINARY_DIR ${PROJECT_BINARY_DIR})
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${MLIR_INCLUDE_DIRS})
include_directories(${SEA_SOURCE_DIR}/include)
include_directories(${SEA_BINARY_DIR}/include)
link_directories(${LLVM_BUILD_LIBRARY_DIR})

# Solvers and Expr dependencies. Z3_ROOT / YICES2_ROOT point at installs.
find_package(Boost 1.71 REQUIRED)
find_path(GMP_INCLUDE_DIR gmpxx.h REQUIRED)
find_library(GMP_LIBRARY gmp REQUIRED)
find_library(GMPXX_LIBRARY gmpxx REQUIRED)
find_path(Z3_INCLUDE_DIR z3.h HINTS ${Z3_ROOT}/include NO_DEFAULT_PATH)
find_path(Z3_INCLUDE_DIR z3.h REQUIRED)
find_library(Z3_LIBRARY z3 HINTS ${Z3_ROOT}/lib NO_DEFAULT_PATH)
find_library(Z3_LIBRARY z3 REQUIRED)
message(STATUS "Z3: ${Z3_LIBRARY}")

# Yices 2 (optional): SEA_ENABLE_YICES = AUTO (default: used when found),
# ON (required) or OFF. Found => the backend is compiled (WITH_YICES2).
# Yices is GPLv3 (z3 and Bitwuzla are MIT): a binary that links libyices is
# distributed under GPLv3 terms, so release binaries are built with OFF
# (docs/Release.md, "Runtime requirements"). Without it, --solver=yices and
# the --yices-* options are errors and the tests that need it are
# unsupported (lit feature `yices`).
set(SEA_ENABLE_YICES AUTO CACHE STRING
  "Yices 2 backend: AUTO (if found), ON (required) or OFF")
set_property(CACHE SEA_ENABLE_YICES PROPERTY STRINGS AUTO ON OFF)
set(SEA_WITH_YICES OFF)
if(SEA_ENABLE_YICES)  # ON, AUTO (a non-false string)
  find_path(YICES2_INCLUDE_DIR yices.h HINTS ${YICES2_ROOT}/include)
  find_library(YICES2_LIBRARY yices HINTS ${YICES2_ROOT}/lib)
  if(YICES2_INCLUDE_DIR AND YICES2_LIBRARY)
    set(SEA_WITH_YICES ON)
    message(STATUS "Yices2: ${YICES2_LIBRARY} (GPLv3: a binary linking it "
                   "is distributed under GPLv3 terms)")
  elseif(SEA_ENABLE_YICES STREQUAL "AUTO")
    message(STATUS "Yices2: not found (YICES2_ROOT=${YICES2_ROOT}); "
                   "--solver=yices disabled")
  else()
    message(FATAL_ERROR "SEA_ENABLE_YICES=${SEA_ENABLE_YICES} but Yices 2 "
                        "is not found: set YICES2_ROOT=<install>")
  endif()
else()
  message(STATUS "Yices2: SEA_ENABLE_YICES=OFF; --solver=yices disabled")
endif()

# Bitwuzla (optional): BITWUZLA_ROOT is an install prefix (include/bitwuzla,
# lib[/<multiarch>]/libbitwuzla.so); only that prefix is searched (a system
# Bitwuzla is not picked up), and the search is redone at every configure.
# Found => the backend is compiled (WITH_BITWUZLA) and the library directory
# goes into the RPATH of the tools. The C++ API needs a TermManager (>= 0.5).
set(SEA_WITH_BITWUZLA OFF)
unset(BITWUZLA_INCLUDE_DIR CACHE)
unset(BITWUZLA_LIBRARY CACHE)
if(BITWUZLA_ROOT)
  find_path(BITWUZLA_INCLUDE_DIR bitwuzla/cpp/bitwuzla.h
    HINTS ${BITWUZLA_ROOT}/include NO_DEFAULT_PATH)
  find_library(BITWUZLA_LIBRARY bitwuzla
    HINTS ${BITWUZLA_ROOT}/lib ${BITWUZLA_ROOT}/lib64
          ${BITWUZLA_ROOT}/lib/${CMAKE_LIBRARY_ARCHITECTURE}
          ${BITWUZLA_ROOT}/lib/x86_64-linux-gnu
    NO_DEFAULT_PATH)
  if(BITWUZLA_INCLUDE_DIR AND BITWUZLA_LIBRARY)
    file(READ ${BITWUZLA_INCLUDE_DIR}/bitwuzla/cpp/bitwuzla.h SEA_BZLA_HEADER)
    string(FIND "${SEA_BZLA_HEADER}" "class TermManager\n" SEA_BZLA_TM)
    if(SEA_BZLA_TM EQUAL -1)
      message(WARNING "Bitwuzla in ${BITWUZLA_ROOT} is too old (no TermManager): "
                      "--solver=bitwuzla disabled")
    else()
      set(SEA_WITH_BITWUZLA ON)
      message(STATUS "Bitwuzla: ${BITWUZLA_LIBRARY}")
    endif()
  else()
    message(WARNING "Bitwuzla not found in BITWUZLA_ROOT=${BITWUZLA_ROOT}: "
                    "--solver=bitwuzla disabled")
  endif()
else()
  message(STATUS "Bitwuzla: BITWUZLA_ROOT not set; --solver=bitwuzla disabled")
endif()

add_subdirectory(include)
add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(unittests)
add_subdirectory(test)

# sea-vc --config profiles (docs/Profiles.md): example config files
install(DIRECTORY profiles/ DESTINATION share/sea-mlir/profiles)
# sea-reduce's interestingness tests (docs/Reducer.md), for minimizing bug
# reports; finds the tools next to itself. The other utils/ scripts are
# development tools of the source tree (docs/Release.md).
install(PROGRAMS utils/reduce-test.py DESTINATION bin RENAME sea-reduce-test)
# documentation
install(FILES README.md CHANGELOG.md LICENSE DESTINATION share/doc/sea-mlir
  OPTIONAL)
install(DIRECTORY docs/ DESTINATION share/doc/sea-mlir/docs)
