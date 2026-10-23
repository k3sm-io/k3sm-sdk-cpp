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

#ifndef K3SM_HELPERS_HPP
#define K3SM_HELPERS_HPP

// Decisions a consumer makes from one snapshot. Each reads only the
// snapshot it is given and never fails: a value the page does not carry
// gives the cautious answer.

#include <cstdint>
#include <optional>

#include <k3sm/snapshot.hpp>

namespace k3sm::sdk {

// rung names one of a signal's five thresholds, in the order a worsening
// signal crosses them.
enum class rung : std::uint8_t { warn, admit_stop, critical, evict_soft, evict_hard };

// admit_ok reports whether the node admits new work: false while any signal
// is at or past its admit-stop rung.
constexpr bool admit_ok(const snapshot& s) noexcept { return s.admit; }

// higher_is_worse reports whether a signal worsens as its value rises
// (memory.compressor and memory.swap); every *.available signal worsens as
// its value falls.
constexpr bool higher_is_worse(signal_id id) noexcept {
    return id == signal_id::memory_compressor || id == signal_id::memory_swap;
}

// bytes_until returns how many bytes the signal can move, in its worsening
// direction, before it reaches rung r: value - threshold for an *.available
// signal, threshold - value for memory.compressor and memory.swap, and 0
// when the signal is at or past the rung. It returns std::nullopt when the page holds no record of
// the signal, the signal is not measured in bytes, or its value or that
// rung's threshold is absent.
constexpr std::optional<std::int64_t> bytes_until(const snapshot& s, signal_id signal,
                                                  rung r) noexcept {
    const signal_record* rec = s.find_signal(signal);
    if (rec == nullptr || rec->unit != unit::bytes || !rec->value) {
        return std::nullopt;
    }
    opt_i64 threshold;
    switch (r) {
    case rung::warn: threshold = rec->warn_threshold; break;
    case rung::admit_stop: threshold = rec->admit_stop_threshold; break;
    case rung::critical: threshold = rec->critical_threshold; break;
    case rung::evict_soft: threshold = rec->evict_soft_threshold; break;
    case rung::evict_hard: threshold = rec->evict_hard_threshold; break;
    }
    if (!threshold) {
        return std::nullopt;
    }
    const std::int64_t value = *rec->value;
    // Both are byte counts of one node; the subtraction of two non-negative
    // i64 values cannot overflow, and a negative one is clamped first.
    const std::int64_t v = value < 0 ? 0 : value;
    const std::int64_t t = *threshold < 0 ? 0 : *threshold;
    const std::int64_t room = higher_is_worse(signal) ? t - v : v - t;
    return room > 0 ? room : 0;
}

// batch_slots returns how many more slots of slot_bytes each the node can
// take now: recommendation.safeAdditionalBytes / slot_bytes, and 0 while
// the node does not admit, when the recommendation is absent, or when
// slot_bytes is not positive. A consumer may only use it to lower its own
// cap, never to raise it.
constexpr std::int64_t batch_slots(const snapshot& s, std::int64_t slot_bytes) noexcept {
    const opt_i64& safe = s.recommendation.safe_additional_bytes;
    if (!s.admit || !safe || *safe <= 0 || slot_bytes <= 0) {
        return 0;
    }
    return *safe / slot_bytes;
}

}  // namespace k3sm::sdk

#endif  // K3SM_HELPERS_HPP
