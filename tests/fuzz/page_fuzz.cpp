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

// The libFuzzer harness over the page reader: arbitrary bytes, as a page
// of exactly that many bytes, into the same read the SDK makes on a
// mapping. The reader must either return a snapshot that passes the
// helpers, or one of the reader contract's typed errors; it must never
// crash, read out of bounds or hit undefined behaviour (the harness is
// built with AddressSanitizer and UndefinedBehaviorSanitizer).
//
// hack/ci.sh --stress builds it with -fsanitize=fuzzer and runs a bounded
// smoke seeded from tests/fuzz/corpus and the vendored page fixtures. The
// corpus holds fixture pages edited at one field each, so the fuzzer starts
// on every reader path: header-only (192 bytes), short-body (one byte short
// of the body), unknown-level (a structural code past its table),
// name-len-254 (a node name above the limit), gpu-present-2 (a field
// outside its domain), minor-0 (the first minor's body size), future-minor
// (a newer minor, read as this reader's newest) and odd-seq (a write in
// progress, so torn).

#include <k3sm/budget.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace sdk = k3sm::sdk;

namespace page_fuzz {

// reader_error reports whether e is an error the reader may return.
inline bool reader_error(const sdk::error& e) {
    switch (e.code()) {
    case sdk::error::abi_mismatch:
    case sdk::error::truncated:
    case sdk::error::corrupt:
    case sdk::error::torn:
    case sdk::error::stale:
        return e.sys_errno() == 0;
    default:
        return false;
    }
}

// read_one reads the page held in [base, base + size) checked against now.
inline void read_one(const unsigned char* base, std::size_t size, std::int64_t now) {
    const auto r = sdk::detail::read_page(sdk::detail::mapped_source{base, size}, now);
    if (!r) {
        if (!reader_error(r.error())) {
            std::abort();
        }
        return;
    }
    // An accepted snapshot keeps the contract's own bounds.
    if (r->node_name_len > sdk::max_node_name_bytes || r->signal_count > sdk::max_signals ||
        r->counter_count > sdk::max_counters || r->seq % 2 != 0) {
        std::abort();
    }
    volatile std::size_t sink = r->node_name().size();
    (void)sink;
}

}  // namespace page_fuzz

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // The page is 8-byte aligned, as a mapping is, and exactly size bytes
    // long, so AddressSanitizer catches a load past its end.
    const std::size_t bytes = size;
    if (bytes == 0) {
        page_fuzz::read_one(nullptr, 0, 0);
        return 0;
    }
    auto* page = static_cast<unsigned char*>(::operator new(bytes, std::align_val_t{8}));
    std::memcpy(page, data, bytes);
    page_fuzz::read_one(page, bytes, 0);
    // The staleness check at both ends of the clock, from the input itself
    // when it is long enough.
    std::int64_t now = 0;
    std::memcpy(&now, data, sizeof now <= size ? sizeof now : 0);
    page_fuzz::read_one(page, bytes, now);
    page_fuzz::read_one(page, bytes, INT64_MAX);
    page_fuzz::read_one(page, bytes, INT64_MIN);
    ::operator delete(page, std::align_val_t{8});
    return 0;
}
