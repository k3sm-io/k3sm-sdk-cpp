# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.24)

project(k3sm_sdk
  VERSION 0.0.0
  DESCRIPTION "C++ client for the k3sm node pressure and headroom API"
  HOMEPAGE_URL "https://github.com/k3sm-io/k3sm-sdk-cpp"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Header-only: one INTERFACE target, consumed as k3sm::sdk whether the SDK
# arrives through FetchContent (add_subdirectory) or an installed package
# (find_package(k3sm-sdk CONFIG)).
add_library(k3sm_sdk INTERFACE)
add_library(k3sm::sdk ALIAS k3sm_sdk)
set_target_properties(k3sm_sdk PROPERTIES EXPORT_NAME sdk)
target_compile_features(k3sm_sdk INTERFACE cxx_std_20)
target_include_directories(k3sm_sdk INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Tests build only when this is the top-level project, so a consumer that
# pulls the SDK in with FetchContent needs no options to keep them out.
option(K3SM_SDK_BUILD_TESTS "Build the k3sm SDK tests" ${PROJECT_IS_TOP_LEVEL})
option(K3SM_SDK_INSTALL "Generate the k3sm SDK install rules" ${PROJECT_IS_TOP_LEVEL})

if(K3SM_SDK_INSTALL)
  set(K3SM_SDK_CMAKE_DIR "${CMAKE_INSTALL_DATADIR}/cmake/k3sm-sdk")
  install(TARGETS k3sm_sdk EXPORT k3sm-sdk-targets)
  install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(EXPORT k3sm-sdk-targets
    NAMESPACE k3sm::
    DESTINATION "${K3SM_SDK_CMAKE_DIR}")
  configure_package_config_file(cmake/k3sm-sdk-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/k3sm-sdk-config.cmake"
    INSTALL_DESTINATION "${K3SM_SDK_CMAKE_DIR}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/k3sm-sdk-config-version.cmake"
    COMPATIBILITY SameMinorVersion
    ARCH_INDEPENDENT)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/k3sm-sdk-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/k3sm-sdk-config-version.cmake"
    DESTINATION "${K3SM_SDK_CMAKE_DIR}")
endif()

if(K3SM_SDK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
