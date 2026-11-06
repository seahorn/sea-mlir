# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_mlir_library(SeaSupport
  BoostException.cpp
  Diag.cpp
  Output.cpp
  SeaDebug.cpp
  SeaLog.cpp
  Version.cpp

  LINK_LIBS PUBLIC
  LLVMSupport
  MLIRIR
  )

# the build-time version (sea/Support/Version.h): regenerated at every build,
# rewritten only when it changes
find_package(Git QUIET)
set(SEA_VERSION_INC ${SEA_BINARY_DIR}/include/sea/Support/VersionInfo.inc)
add_custom_target(sea-version
  COMMAND ${CMAKE_COMMAND} -DSRC=${SEA_SOURCE_DIR} -DOUT=${SEA_VERSION_INC}
          -DVERSION=${PROJECT_VERSION} -DLLVM_REV_FILE=${SEA_SOURCE_DIR}/LLVM_REVISION
          -DDESCRIBE=${SEA_GIT_DESCRIBE} -DGIT=${GIT_EXECUTABLE}
          -P ${SEA_SOURCE_DIR}/cmake/SeaVersion.cmake
  BYPRODUCTS ${SEA_VERSION_INC}
  COMMENT "sea-mlir version"
  VERBATIM)
add_dependencies(SeaSupport sea-version)
set_property(SOURCE Version.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  SEA_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
if(TARGET obj.SeaSupport)
  add_dependencies(obj.SeaSupport sea-version)
endif()
