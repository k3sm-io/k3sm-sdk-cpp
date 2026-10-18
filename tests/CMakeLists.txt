# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0

add_executable(k3sm_sdk_version_test version_test.cpp second_tu.cpp)
target_link_libraries(k3sm_sdk_version_test PRIVATE k3sm::sdk)
set_target_properties(k3sm_sdk_version_test PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(k3sm_sdk_version_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  # AddressSanitizer and UndefinedBehaviorSanitizer when the toolchain links them.
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
  check_cxx_source_compiles("int main() { return 0; }" K3SM_SDK_HAVE_SANITIZERS)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(K3SM_SDK_HAVE_SANITIZERS)
    target_compile_options(k3sm_sdk_version_test PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(k3sm_sdk_version_test PRIVATE -fsanitize=address,undefined)
  endif()
endif()

add_test(NAME version COMMAND k3sm_sdk_version_test)
