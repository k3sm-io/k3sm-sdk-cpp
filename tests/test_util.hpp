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

#ifndef K3SM_TEST_UTIL_HPP
#define K3SM_TEST_UTIL_HPP

// Plain-assert helpers shared by the SDK tests: a check that records a
// failure and continues, file reading, and the few JSON lookups the tests
// make on the vendored manifest. No test framework, no dependency.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef K3SM_SDK_FIXTURE_DIR
#error "K3SM_SDK_FIXTURE_DIR must name tests/fixtures/page"
#endif

namespace k3sm_test {

inline int& failures() {
    static int n = 0;
    return n;
}

inline void fail(const char* file, int line, const std::string& what) {
    std::fprintf(stderr, "%s:%d: FAIL: %s\n", file, line, what.c_str());
    ++failures();
}

#define K3SM_CHECK(cond)                                                   \
    do {                                                                   \
        if (!(cond)) ::k3sm_test::fail(__FILE__, __LINE__, #cond);         \
    } while (0)

#define K3SM_CHECK_EQ(a, b)                                                \
    do {                                                                   \
        if (!((a) == (b))) ::k3sm_test::fail(__FILE__, __LINE__, #a " == " #b); \
    } while (0)

// fixture returns the path of a vendored fixture file.
inline std::string fixture(std::string_view name) {
    return std::string(K3SM_SDK_FIXTURE_DIR) + "/" + std::string(name);
}

// read_file returns a file's bytes, or an empty string when it cannot be read.
inline std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// json_value returns the raw text of the first `"key": <value>` at or after
// from in text: a string's contents without quotes, or a number or literal
// as written. It is enough for the flat manifest; it is not a JSON parser.
inline std::string json_value(std::string_view text, std::string_view key, std::size_t from = 0,
                              std::size_t* end = nullptr) {
    const std::string needle = "\"" + std::string(key) + "\":";
    std::size_t at = text.find(needle, from);
    if (at == std::string_view::npos) {
        return {};
    }
    at += needle.size();
    while (at < text.size() && text[at] == ' ') {
        ++at;
    }
    std::size_t stop = 0;
    std::string out;
    if (at < text.size() && text[at] == '"') {
        stop = text.find('"', at + 1);
        out = std::string(text.substr(at + 1, stop - at - 1));
        ++stop;
    } else {
        stop = text.find_first_of(",\n}", at);
        out = std::string(text.substr(at, stop - at));
    }
    if (end != nullptr) {
        *end = stop;
    }
    return out;
}

// json_strings returns the strings of the array `"key": [ ... ]` at or after
// from in text.
inline std::vector<std::string> json_strings(std::string_view text, std::string_view key,
                                             std::size_t from = 0) {
    std::vector<std::string> out;
    const std::string needle = "\"" + std::string(key) + "\": [";
    std::size_t at = text.find(needle, from);
    if (at == std::string_view::npos) {
        return out;
    }
    const std::size_t close = text.find(']', at);
    at += needle.size();
    while (true) {
        const std::size_t open = text.find('"', at);
        if (open == std::string_view::npos || open > close) {
            break;
        }
        const std::size_t shut = text.find('"', open + 1);
        out.emplace_back(text.substr(open + 1, shut - open - 1));
        at = shut + 1;
    }
    return out;
}

// json_objects returns, for each object of the array `"key": [ {...}, ... ]`,
// the text of that object.
inline std::vector<std::string> json_objects(std::string_view text, std::string_view key) {
    std::vector<std::string> out;
    const std::string needle = "\"" + std::string(key) + "\": [";
    std::size_t at = text.find(needle);
    if (at == std::string_view::npos) {
        return out;
    }
    at += needle.size();
    while (true) {
        const std::size_t open = text.find_first_of("{]", at);
        if (open == std::string_view::npos || text[open] == ']') {
            break;
        }
        const std::size_t shut = text.find('}', open);
        out.emplace_back(text.substr(open, shut - open + 1));
        at = shut + 1;
    }
    return out;
}

}  // namespace k3sm_test

#endif  // K3SM_TEST_UTIL_HPP
