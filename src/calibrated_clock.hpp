// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <limits>

// Exact floor(ticks * 1e9 / frequency), without overflowing the intermediate
// product and without floating-point loss after long host uptimes. QPC returns
// raw ticks; Vulkan CLOCK_MONOTONIC timestamps already have frequency 1e9.
inline bool cvk_clock_to_ns(uint64_t ticks, uint64_t frequency, uint64_t& ns) {
    constexpr uint64_t scale = 1000000000;
    if (!frequency) return false;
    const uint64_t seconds = ticks / frequency;
    if (seconds > UINT64_MAX / scale) return false;
    const uint64_t remainder = ticks % frequency;
    uint64_t fraction;
    if (remainder <= UINT64_MAX / scale) {
        fraction = remainder * scale / frequency;
    } else {
        // Binary multiply/divide for the uncommon huge-frequency case. Both
        // addends stay below the divisor; subtraction comparisons avoid carry.
        fraction = 0;
        uint64_t rem = 0;
        for (int bit = 29; bit >= 0; --bit) {
            fraction *= 2;
            if (rem >= frequency - rem) {
                rem -= frequency - rem;
                ++fraction;
            } else {
                rem += rem;
            }
            if ((scale >> bit) & 1) {
                if (rem >= frequency - remainder) {
                    rem -= frequency - remainder;
                    ++fraction;
                } else {
                    rem += remainder;
                }
            }
        }
    }
    const uint64_t whole = seconds * scale;
    if (whole > UINT64_MAX - fraction) return false;
    ns = whole + fraction;
    return true;
}
