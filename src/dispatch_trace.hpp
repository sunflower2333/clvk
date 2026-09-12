// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

// Diagnostic metadata only. Recording never changes dispatch geometry,
// dependencies, submission boundaries, waits or completion status.
struct cvk_dispatch_trace {
    struct record {
        uint64_t ordinal = 0;
        uintptr_t command = 0, event = 0, kernel = 0, program = 0;
        std::array<char, 96> name{};
        bool name_truncated = false;
        uint32_t dimensions = 0;
        std::array<uint32_t, 3> global{}, local{}, offset{};
        std::array<uint32_t, 3> region_global{}, region_local{}, region_offset{};
    };
    static constexpr size_t capacity = 32;
    explicit cvk_dispatch_trace(std::string filter)
        : id(next_id.fetch_add(1, std::memory_order_relaxed)),
          kernel_filter(std::move(filter)) {}

    void command() { ++commands; }
    void dispatch(const char* name, record value) {
        value.ordinal = ++dispatches;
        if (!kernel_filter.empty() &&
            std::strstr(name, kernel_filter.c_str()) == nullptr) return;
        const size_t length = std::strlen(name);
        const size_t copied = std::min(length, value.name.size() - 1);
        std::memcpy(value.name.data(), name, copied);
        value.name[copied] = 0;
        value.name_truncated = length > copied;
        records[matched % capacity] = value;
        ++matched;
    }
    template <typename Emit> void each(Emit&& emit) const {
        const auto count = std::min<uint64_t>(matched, capacity);
        for (uint64_t index = matched - count; index < matched; ++index)
            emit(records[index % capacity]);
    }
    static uint64_t monotonic_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static uint64_t utc_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    const uint64_t id;
    uint64_t commands = 0, dispatches = 0, matched = 0;
private:
    inline static std::atomic<uint64_t> next_id{1};
    const std::string kernel_filter;
    std::array<record, capacity> records{};
};
