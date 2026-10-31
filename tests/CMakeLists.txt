# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0

# K3SM_SDK_SANITIZER picks the sanitizers the tests build with: "auto" (the
# default) is AddressSanitizer and UndefinedBehaviorSanitizer when the
# toolchain links them, "thread" is ThreadSanitizer (required, for the
# torn-read stress), "none" is neither (for the benchmark).
set(K3SM_SDK_SANITIZER "auto" CACHE STRING "Test sanitizers: auto, thread or none")
set_property(CACHE K3SM_SDK_SANITIZER PROPERTY STRINGS auto thread none)
# K3SM_SDK_STRESS adds the torn-read stress test (ctest label stress) and the
# benchmark (label bench); K3SM_SDK_FUZZ adds the libFuzzer harness. Both are
# off by default; hack/ci.sh --stress turns them on.
option(K3SM_SDK_STRESS "Build the SDK stress test and benchmark" OFF)
option(K3SM_SDK_FUZZ "Build the SDK libFuzzer harness (needs a clang with libFuzzer)" OFF)

set(K3SM_SDK_SANITIZE_FLAGS "")
if(K3SM_SDK_SANITIZER STREQUAL "thread")
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=thread")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=thread")
  check_cxx_source_compiles("int main() { return 0; }" K3SM_SDK_HAVE_TSAN)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(NOT K3SM_SDK_HAVE_TSAN)
    message(FATAL_ERROR "K3SM_SDK_SANITIZER=thread: this compiler cannot build with -fsanitize=thread")
  endif()
  set(K3SM_SDK_SANITIZE_FLAGS -fsanitize=thread)
elseif(K3SM_SDK_SANITIZER STREQUAL "auto")
  # AddressSanitizer and UndefinedBehaviorSanitizer when the toolchain links them.
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_cxx_source_compiles("int main() { return 0; }" K3SM_SDK_HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
  endif()
  if(K3SM_SDK_HAVE_SANITIZERS)
    set(K3SM_SDK_SANITIZE_FLAGS -fsanitize=address,undefined)
  endif()
elseif(NOT K3SM_SDK_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "K3SM_SDK_SANITIZER must be auto, thread or none, not '${K3SM_SDK_SANITIZER}'")
endif()

# k3sm_sdk_test builds one test executable with warnings as errors and,
# where available, the sanitizers, and registers it with ctest.
function(k3sm_sdk_test name)
  cmake_parse_arguments(T "" "" "SOURCES;LABELS;ARGS" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE k3sm::sdk)
  set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_definitions(${name} PRIVATE
    K3SM_SDK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/page"
    K3SM_SDK_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(K3SM_SDK_SANITIZE_FLAGS)
      target_compile_options(${name} PRIVATE
        ${K3SM_SDK_SANITIZE_FLAGS} -fno-omit-frame-pointer -fno-sanitize-recover=all)
      target_link_options(${name} PRIVATE ${K3SM_SDK_SANITIZE_FLAGS})
    endif()
  endif()
  add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
  if(T_LABELS)
    set_tests_properties(${name} PROPERTIES LABELS "${T_LABELS}")
  endif()
endfunction()

k3sm_sdk_test(k3sm_sdk_version_test SOURCES version_test.cpp second_tu.cpp)
k3sm_sdk_test(k3sm_sdk_reader_test SOURCES reader_test.cpp LABELS reader)
k3sm_sdk_test(k3sm_sdk_torn_test SOURCES torn_test.cpp LABELS reader)
k3sm_sdk_test(k3sm_sdk_notifier_test SOURCES notifier_test.cpp LABELS notifier)
k3sm_sdk_test(k3sm_sdk_writer_test SOURCES writer_test.cpp LABELS writer)
k3sm_sdk_test(k3sm_sdk_writer_stress_test SOURCES writer_stress_test.cpp LABELS writer)

if(K3SM_SDK_STRESS)
  k3sm_sdk_test(k3sm_sdk_threads_stress_test SOURCES stress/threads_stress_test.cpp LABELS stress)
  k3sm_sdk_test(k3sm_sdk_page_bench SOURCES bench/page_bench.cpp LABELS bench ARGS --quick)
endif()

if(K3SM_SDK_FUZZ)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer,address,undefined")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer,address,undefined")
  check_cxx_source_compiles([[
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }
]] K3SM_SDK_HAVE_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(NOT K3SM_SDK_HAVE_LIBFUZZER)
    message(FATAL_ERROR "K3SM_SDK_FUZZ: ${CMAKE_CXX_COMPILER} cannot link -fsanitize=fuzzer "
      "(Apple clang ships no libFuzzer runtime; use an LLVM clang, for example Homebrew's llvm)")
  endif()
  add_executable(k3sm_sdk_page_fuzz fuzz/page_fuzz.cpp)
  target_link_libraries(k3sm_sdk_page_fuzz PRIVATE k3sm::sdk)
  set_target_properties(k3sm_sdk_page_fuzz PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_options(k3sm_sdk_page_fuzz PRIVATE -Wall -Wextra -Wpedantic -Werror
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(k3sm_sdk_page_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
