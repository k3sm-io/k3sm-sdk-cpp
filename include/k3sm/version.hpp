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

#ifndef K3SM_VERSION_HPP
#define K3SM_VERSION_HPP

// The SDK's own version. The client API is still being designed; this header
// is the only one the SDK ships until that design is settled.

#define K3SM_SDK_VERSION_MAJOR 0
#define K3SM_SDK_VERSION_MINOR 0
#define K3SM_SDK_VERSION_PATCH 0

namespace k3sm::sdk {

// The SDK version as major, minor and patch numbers.
struct version_t {
    int major;
    int minor;
    int patch;
};

// The version of the headers this translation unit was compiled against.
inline constexpr version_t version{K3SM_SDK_VERSION_MAJOR, K3SM_SDK_VERSION_MINOR,
                                   K3SM_SDK_VERSION_PATCH};

// The version as "major.minor.patch".
inline constexpr char version_string[] = "0.0.0";

}  // namespace k3sm::sdk

#endif  // K3SM_VERSION_HPP
