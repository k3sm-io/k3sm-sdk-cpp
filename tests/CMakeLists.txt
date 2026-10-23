# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0

# AddressSanitizer and UndefinedBehaviorSanitizer when the toolchain links them.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
  check_cxx_source_compiles("int main() { return 0; }" K3SM_SDK_HAVE_SANITIZERS)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
endif()

# k3sm_sdk_test builds one test executable with warnings as errors and,
# where available, the sanitizers, and registers it with ctest.
function(k3sm_sdk_test name)
  cmake_parse_arguments(T "" "" "SOURCES;LABELS" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE k3sm::sdk)
  set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_definitions(${name} PRIVATE
    K3SM_SDK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/page"
    K3SM_SDK_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(K3SM_SDK_HAVE_SANITIZERS)
      target_compile_options(${name} PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
      target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
  add_test(NAME ${name} COMMAND ${name})
  if(T_LABELS)
    set_tests_properties(${name} PROPERTIES LABELS "${T_LABELS}")
  endif()
endfunction()

k3sm_sdk_test(k3sm_sdk_version_test SOURCES version_test.cpp second_tu.cpp)
k3sm_sdk_test(k3sm_sdk_reader_test SOURCES reader_test.cpp LABELS reader)
k3sm_sdk_test(k3sm_sdk_torn_test SOURCES torn_test.cpp LABELS reader)
