/*
Copyright The k3sm Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <k3sm/budget.hpp>
#include <k3sm/version.hpp>

#include <cstdio>
#include <string>

// Compile-time checks: the version macros and the constexpr value agree.
static_assert(k3sm::sdk::version.major == K3SM_SDK_VERSION_MAJOR);
static_assert(k3sm::sdk::version.minor == K3SM_SDK_VERSION_MINOR);
static_assert(k3sm::sdk::version.patch == K3SM_SDK_VERSION_PATCH);

int k3sm_sdk_second_tu_major();

int main() {
    if (k3sm_sdk_second_tu_major() != K3SM_SDK_VERSION_MAJOR) {
        std::fprintf(stderr, "the second translation unit sees another version\n");
        return 1;
    }
    const std::string want = std::to_string(K3SM_SDK_VERSION_MAJOR) + "." +
                             std::to_string(K3SM_SDK_VERSION_MINOR) + "." +
                             std::to_string(K3SM_SDK_VERSION_PATCH);
    if (want != k3sm::sdk::version_string) {
        std::fprintf(stderr, "version_string is %s, want %s\n", k3sm::sdk::version_string,
                     want.c_str());
        return 1;
    }
    return 0;
}
