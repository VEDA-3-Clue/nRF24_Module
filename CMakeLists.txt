################################################################################
#
# nerfnet build
#
################################################################################

# Project Settings #############################################################

cmake_minimum_required(VERSION 3.16)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(nerfnet VERSION 0.2.6 LANGUAGES CXX)

set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install_pack" CACHE PATH "Default install path" FORCE)

# Dependencies #################################################################

find_path(TCLAP_INCLUDE_DIR
  NAMES tclap/CmdLine.h
  HINTS
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/local/include
)

if(NOT TCLAP_INCLUDE_DIR)
  message(FATAL_ERROR
    "Could not find tclap/CmdLine.h. "
    "For cross-compilation, make sure the target sysroot contains TCLAP and "
    "configure with the aarch64 toolchain so CMAKE_SYSROOT is set."
  )
endif()

add_library(tclap INTERFACE)
add_library(tclap::tclap ALIAS tclap)
target_include_directories(tclap INTERFACE
  ${TCLAP_INCLUDE_DIR}
)

if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/run.sh.in")
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/run.sh.in"
    "${CMAKE_BINARY_DIR}/run.sh"
    @ONLY
  )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/install_bundle_deps.cmake.in")
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/install_bundle_deps.cmake.in"
    "${CMAKE_BINARY_DIR}/install_bundle_deps.cmake"
    @ONLY
  )
endif()

# Subdirectories ###############################################################

add_subdirectory(nerfnet)

install(SCRIPT "${CMAKE_BINARY_DIR}/install_bundle_deps.cmake")
install(PROGRAMS "${CMAKE_BINARY_DIR}/run.sh" DESTINATION .)

include(CPack)
set(CPACK_PACKAGE_NAME "nerfnet")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_GENERATOR "TGZ")  # 또는 "DEB", "RPM" 등`