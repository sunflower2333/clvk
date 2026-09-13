// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "batch_duration.hpp"
#include "ndrange_tiles.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// Worst observed wall-clock submission time per global work item for one kernel
// and argument shape. Samples include queueing behind other GPU clients, so the
// estimate is an upper bound; it is only ever raised. Not a timeout.
struct cvk_dispatch_cost {
    static constexpr unsigned fraction_bits = 16;

    void observe(uint64_t elapsed_ns, uint64_t items) {
        if (!elapsed_ns || !items) return;
        const uint64_t scaled = elapsed_ns > (UINT64_MAX >> fraction_bits)
                                    ? UINT64_MAX
                                    : elapsed_ns << fraction_bits;
        const uint64_t sample = scaled / items + (scaled % items != 0);
        auto old = m_scaled_ns_per_item.load(std::memory_order_relaxed);
        while (old < sample &&
               !m_scaled_ns_per_item.compare_exchange_weak(
                   old, sample, std::memory_order_relaxed)) {
        }
    }

    // Rounded-up, saturating estimate; 0 while nothing has been observed.
    uint64_t estimate_ns(uint64_t items) const {
        const uint64_t scaled =
            m_scaled_ns_per_item.load(std::memory_order_relaxed);
        const uint64_t mask = (uint64_t(1) << fraction_bits) - 1;
        const uint64_t whole = scaled >> fraction_bits, fraction = scaled & mask;
        if (whole && items > UINT64_MAX / whole) return UINT64_MAX;
        const uint64_t integral = whole * items;
        // fraction < 2^16, so each partial product below fits in 64 bits.
        const uint64_t high = items >> fraction_bits, low = items & mask;
        const uint64_t partial =
            fraction * high + ((fraction * low + mask) >> fraction_bits);
        return integral > UINT64_MAX - partial ? UINT64_MAX : integral + partial;
    }

    // Largest whole-workgroup tile estimated within target_ns (at least one
    // group); 0 when no sample exists or the inputs are empty.
    uint32_t tile_budget(uint64_t target_ns, uint64_t items_per_group) const {
        const uint64_t scaled =
            m_scaled_ns_per_item.load(std::memory_order_relaxed);
        if (!scaled || !target_ns || !items_per_group) return 0;
        const uint64_t per_group = scaled > UINT64_MAX / items_per_group
                                       ? UINT64_MAX
                                       : scaled * items_per_group;
        const uint64_t scaled_target = target_ns > (UINT64_MAX >> fraction_bits)
                                           ? UINT64_MAX
                                           : target_ns << fraction_bits;
        return uint32_t(std::clamp<uint64_t>(scaled_target / per_group, 1,
                                             UINT32_MAX));
    }

    // Report each distinct duration-derived budget once per shape.
    bool first_report(uint32_t budget) {
        return m_reported_budget.exchange(budget, std::memory_order_relaxed) !=
               budget;
    }

private:
    std::atomic<uint64_t> m_scaled_ns_per_item{0};
    std::atomic<uint32_t> m_reported_budget{0};
};

// Owned by one kernel object. In-flight commands keep their state alive even
// if the slot is replaced by a newer shape.
struct cvk_dispatch_cost_cache {
    static constexpr size_t capacity = 32;
    std::shared_ptr<cvk_dispatch_cost> find(const cvk_batch_cost_key& key) {
        if (!key.valid) return {};
        std::lock_guard<std::mutex> lock(m_lock);
        for (auto& entry : m_entries)
            if (entry.cost && entry.key == key.bytes) return entry.cost;
        auto result = std::make_shared<cvk_dispatch_cost>();
        m_entries[m_next] = {key.bytes, result};
        m_next = (m_next + 1) % capacity;
        return result;
    }

private:
    struct entry {
        std::vector<uint8_t> key;
        std::shared_ptr<cvk_dispatch_cost> cost;
    };
    std::array<entry, capacity> m_entries;
    size_t m_next = 0;
    std::mutex m_lock;
};

inline uint64_t cvk_ndrange_items(const std::array<uint32_t, 3>& gws) {
    return uint64_t(gws[0]) * gws[1] * gws[2];
}

// Tile budget for one NDRange: a geometry budget the range exceeds, lowered to
// a duration-derived budget when the measured estimate exceeds target_ns.
// Returns 0 when the range should be submitted untiled.
inline uint32_t cvk_select_tile_budget(const std::array<uint32_t, 3>& gws,
                                       const std::array<uint32_t, 3>& lws,
                                       uint32_t geometry_budget,
                                       const cvk_dispatch_cost* cost,
                                       uint64_t target_ns,
                                       bool* duration_limited = nullptr) {
    if (duration_limited) *duration_limited = false;
    uint32_t budget =
        cvk_ndrange_exceeds_workgroup_budget(gws, lws, geometry_budget)
            ? geometry_budget
            : 0;
    if (!cost || !target_ns) return budget;
    const uint64_t items = cvk_ndrange_items(gws);
    const uint64_t per_group = cvk_ndrange_items(lws);
    if (!items || !per_group || cost->estimate_ns(items) <= target_ns)
        return budget;
    const uint32_t duration = cost->tile_budget(target_ns, per_group);
    if (!duration || !cvk_ndrange_exceeds_workgroup_budget(gws, lws, duration))
        return budget;
    if (!budget || duration < budget) {
        budget = duration;
        if (duration_limited) *duration_limited = true;
    }
    return budget;
}

// Commands that share one submission. Unknown estimates count as zero so the
// ordinary batching of unmeasured kernels is unchanged.
struct cvk_dispatch_duration_window {
    explicit cvk_dispatch_duration_window(uint64_t target) : target_ns(target) {}
    bool accepts(uint64_t estimate) const {
        if (!target_ns || !m_commands) return true;
        return m_total_ns < target_ns && estimate <= target_ns - m_total_ns;
    }
    void add(uint64_t estimate) {
        ++m_commands;
        m_total_ns = estimate > UINT64_MAX - m_total_ns ? UINT64_MAX
                                                        : m_total_ns + estimate;
    }
    const uint64_t target_ns;

private:
    uint64_t m_commands = 0, m_total_ns = 0;
};
