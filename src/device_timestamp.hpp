// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cmath>
#include <cstdint>

inline bool cvk_timestamp_delta(uint64_t sample, uint64_t anchor,
                                uint32_t valid_bits, bool& negative,
                                uint64_t& magnitude) {
    if (!valid_bits || valid_bits > 64) return false;
    const uint64_t mask = UINT64_MAX >> (64 - valid_bits);
    const uint64_t half = uint64_t(1) << (valid_bits - 1);
    const uint64_t forward = (sample - anchor) & mask;
    // Exactly half a counter cycle has no unambiguous signed interpretation.
    if (forward == half) return false;
    negative = forward > half;
    magnitude = negative ? ((anchor - sample) & mask) : forward;
    return true;
}

inline bool cvk_timestamp_offset_to_host(uint64_t sample, uint64_t anchor,
                                         uint64_t host_anchor, uint32_t bits,
                                         double period, uint64_t& host) {
    bool negative;
    uint64_t ticks;
    if (!cvk_timestamp_delta(sample, anchor, bits, negative, ticks) ||
        !std::isfinite(period) || period <= 0) return false;
    uint64_t nanos;
    if (period == 1.0) {
        nanos = ticks;
    } else {
        // Rescale the short signed interval, not an absolute counter epoch.
        // floor(host_anchor - interval) requires ceil for a negative offset.
        double value = double(ticks) * period;
        value = negative ? std::ceil(value) : std::floor(value);
        if (!std::isfinite(value) || value >= 18446744073709551616.0)
            return false;
        nanos = uint64_t(value);
    }
    if (negative) {
        if (host_anchor < nanos) return false;
        host = host_anchor - nanos;
    } else {
        if (host_anchor > UINT64_MAX - nanos) return false;
        host = host_anchor + nanos;
    }
    return true;
}

// Query pairs and their calibration must be within half a counter cycle.
// For Turnip's48bit19.2MHz timer this is about84.8days. Invalid/reversed or
// ambiguous intervals fail rather than fabricating a duration or clamping.
inline bool cvk_timestamp_pair_to_host(uint64_t start, uint64_t end,
                                       uint64_t anchor, uint64_t host_anchor,
                                       uint32_t bits, double period,
                                       uint64_t& start_host, uint64_t& end_host) {
    bool negative;
    uint64_t duration;
    if (!cvk_timestamp_delta(end, start, bits, negative, duration) || negative)
        return false;
    uint64_t first, last;
    if (!cvk_timestamp_offset_to_host(start, anchor, host_anchor, bits, period, first) ||
        !cvk_timestamp_offset_to_host(end, anchor, host_anchor, bits, period, last) ||
        last < first) return false;
    start_host = first;
    end_host = last;
    return true;
}
