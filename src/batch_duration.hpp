// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

// Admission estimates only. These types never provide completion or a timeout.
struct cvk_batch_cost_key {
    static constexpr size_t maximum_bytes = 1024;
    bool valid = true;
    std::vector<uint8_t> bytes;
    void append(const void* data, size_t size) {
        if (!valid) return;
        if (size > maximum_bytes - bytes.size()) {
            valid = false; bytes.clear(); return;
        }
        if (size) {
            const auto* first = static_cast<const uint8_t*>(data);
            bytes.insert(bytes.end(), first, first + size);
        }
    }
    void value(uint64_t value) { append(&value, sizeof(value)); }
};

struct cvk_batch_cost {
    uint64_t estimate() const {
        return m_isolated.load(std::memory_order_acquire) ? 0 :
            m_peak_ns.load(std::memory_order_relaxed);
    }
    void observe(uint64_t elapsed_ns, uint64_t budget_ns, bool api_success) {
        // An over-budget API success is not trustworthy retirement evidence
        // (e.g. a backend reset waking a queue-idle wait). Keep this key alone.
        if (!api_success || (budget_ns && elapsed_ns >= budget_ns)) {
            m_isolated.store(true, std::memory_order_release);
            return;
        }
        if (!budget_ns || !elapsed_ns || m_isolated.load()) return;
        const uint64_t guarded = elapsed_ns > UINT64_MAX / 2 ? UINT64_MAX :
            elapsed_ns * 2;
        auto old = m_peak_ns.load(std::memory_order_relaxed);
        while (old < guarded && !m_peak_ns.compare_exchange_weak(old, guarded,
                std::memory_order_relaxed)) {}
    }
private:
    std::atomic<uint64_t> m_peak_ns{0};
    std::atomic<bool> m_isolated{false};
};

inline uint64_t cvk_batch_cost_sample(uint64_t elapsed_ns, uint64_t budget_ns,
                                     bool success, size_t commands, bool homogeneous) {
    if (!success || !budget_ns || !elapsed_ns || elapsed_ns >= budget_ns ||
        !commands || !homogeneous) return elapsed_ns;
    return elapsed_ns / commands + (elapsed_ns % commands != 0);
}

// Owned by one kernel object, avoiding stale reuse of pointer identities.
// In-flight commands retain their state even if its cache slot is replaced.
struct cvk_batch_cost_cache {
    static constexpr size_t capacity = 16;
    std::shared_ptr<cvk_batch_cost> find(const cvk_batch_cost_key& key) {
        if (!key.valid) return {};
        std::lock_guard<std::mutex> lock(m_lock);
        for (auto& entry : m_entries)
            if (entry.cost && entry.key == key.bytes) return entry.cost;
        auto result = std::make_shared<cvk_batch_cost>();
        m_entries[m_next] = {key.bytes, result};
        m_next = (m_next + 1) % capacity;
        return result;
    }
private:
    struct entry {
        std::vector<uint8_t> key;
        std::shared_ptr<cvk_batch_cost> cost;
    };
    std::array<entry, capacity> m_entries;
    size_t m_next = 0;
    std::mutex m_lock;
};

struct cvk_batch_duration_window {
    explicit cvk_batch_duration_window(uint64_t budget) : budget_ns(budget) {}
    bool accepts(uint64_t cost) const {
        if (!budget_ns || !m_commands) return true;
        if (!cost || m_unknown || m_total_ns >= budget_ns) return false;
        return cost <= budget_ns - m_total_ns;
    }
    void add(uint64_t cost) {
        ++m_commands;
        m_unknown |= cost == 0;
        m_total_ns = cost > UINT64_MAX - m_total_ns ? UINT64_MAX :
            m_total_ns + cost;
    }
    bool full() const {
        return budget_ns && (m_unknown || m_total_ns >= budget_ns);
    }
    const uint64_t budget_ns;
private:
    uint64_t m_commands = 0, m_total_ns = 0;
    bool m_unknown = false;
};
