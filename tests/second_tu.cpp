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

// A second translation unit that includes the SDK headers. Linked beside
// version_test.cpp, and merged with it when the target builds as one unity
// source, it proves the headers carry no per-file definitions that collide.

#include <k3sm/version.hpp>

int k3sm_sdk_second_tu_major() { return k3sm::sdk::version.major; }
