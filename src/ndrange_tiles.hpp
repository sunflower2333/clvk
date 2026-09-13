// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

// Generation provenance, deliberately not inferred from imported reflection or
// the lossy v2 binary build-options header. Only successful source compilation
// through the configured clspv followed by executable validation can publish it.
class cvk_region_abi {
public:
    void reset() { m_generated = m_validated = false; }
    void generated(bool source, const std::string& effective_options) {
        reset();
        if (!source) return;
        bool regions = false, offsets = false;
        std::istringstream input(effective_options);
        std::string option;
        while (input >> option) {
            // Accept only the known positive spellings of ABI-affecting flags.
            // Reject ambiguous boolean overrides, including explicit false.
            if (option.find("uniform-work") != std::string::npos &&
                option != "-cl-arm-non-uniform-work-group-size") return;
            if (option.find("global-offset") != std::string::npos &&
                option != "-global-offset") return;
            regions |= option == "-cl-arm-non-uniform-work-group-size";
            offsets |= option == "-global-offset";
        }
        m_generated = regions && offsets;
    }
    void validated() { m_validated = m_generated; }
    bool supported() const { return m_validated; }
private:
    bool m_generated = false;
    bool m_validated = false;
};

struct cvk_ndrange_tile {
    std::array<uint32_t, 3> offset{}, gws{}, lws{};
};

// Count the entire original NDRange, including every nonuniform tail group.
// Comparing by division avoids overflowing even a 64-bit group product. A
// disabled budget or empty/invalid geometry retains the ordinary API path.
inline bool cvk_ndrange_exceeds_workgroup_budget(
    const std::array<uint32_t, 3>& gws,
    const std::array<uint32_t, 3>& lws, uint32_t budget) {
    if (!budget) return false;
    for (unsigned d = 0; d < 3; ++d)
        if (!gws[d] || !lws[d]) return false;
    uint32_t remaining = budget;
    for (unsigned d = 0; d < 3; ++d) {
        const uint32_t groups = gws[d] / lws[d] + (gws[d] % lws[d] != 0);
        if (groups > remaining) return true;
        remaining /= groups;
    }
    return false;
}

// O(1) storage. Enumerate at most eight original uniform/tail regions, then
// rectangular whole-workgroup tiles. All positions are relative to the original
// global offset. A tail's group origin is still divided by the original LWS.
class cvk_ndrange_tiles {
public:
    bool init(const std::array<uint32_t, 3>& offset,
              const std::array<uint32_t, 3>& gws,
              const std::array<uint32_t, 3>& lws,
              const std::array<uint32_t, 3>& limits, uint32_t budget) {
        *this = {};
        if (!budget) return false;
        uint64_t groups = 1;
        for (unsigned d = 0; d < 3; ++d) {
            if (!gws[d] || !lws[d] || !limits[d] ||
                uint64_t(offset[d]) + gws[d] - 1 > UINT32_MAX) return false;
            const uint64_t n = (uint64_t(gws[d]) + lws[d] - 1) / lws[d];
            if (groups > UINT64_MAX / n) return false;
            groups *= n;
        }
        m_gws = gws; m_lws = lws; m_limits = limits; m_budget = budget;
        m_valid = true;
        return select_region();
    }

    bool has_next() const { return m_valid; }
    bool next(cvk_ndrange_tile& tile) {
        if (!m_valid) return false;
        tile = m_region;
        for (unsigned d = 0; d < 3; ++d) {
            const auto n = std::min(m_shape[d], m_groups[d] - m_position[d]);
            tile.offset[d] += m_position[d] * tile.lws[d];
            tile.gws[d] = n * tile.lws[d];
        }
        for (unsigned d = 0; d < 3; ++d) {
            const auto n = std::min(m_shape[d], m_groups[d] - m_position[d]);
            m_position[d] += n;
            if (m_position[d] < m_groups[d]) return true;
            m_position[d] = 0;
        }
        ++m_mask;
        select_region();
        return true;
    }

private:
    bool select_region() {
        for (; m_mask < 8; ++m_mask) {
            bool nonempty = true;
            uint32_t remaining = m_budget;
            for (unsigned d = 0; d < 3; ++d) {
                const uint32_t tail = m_gws[d] % m_lws[d];
                const uint32_t full = m_gws[d] - tail;
                const bool tail_region = (m_mask & (1u << d)) != 0;
                m_region.offset[d] = tail_region ? full : 0;
                m_region.gws[d] = tail_region ? tail : full;
                m_region.lws[d] = tail_region ? tail : m_lws[d];
                if (!m_region.gws[d]) { nonempty = false; break; }
                m_groups[d] = m_region.gws[d] / m_region.lws[d];
                m_shape[d] = std::min({m_groups[d], m_limits[d], remaining});
                remaining /= m_shape[d];
            }
            if (nonempty) { m_position = {}; return true; }
        }
        m_valid = false;
        return false;
    }
    bool m_valid = false;
    uint32_t m_mask = 0, m_budget = 0;
    cvk_ndrange_tile m_region;
    std::array<uint32_t, 3> m_gws{}, m_lws{}, m_limits{}, m_groups{},
        m_shape{}, m_position{};
};

// Shared by the real command executor and lifecycle regression. The first
// buffer is already recorded at enqueue. Never prepare another after a failed
// retirement, and never run final processing after an intermediate failure.
template <typename Submit, typename Prepare, typename Finish>
int cvk_run_tile_submissions(Submit submit, Prepare prepare, Finish finish) {
    for (;;) {
        int status = submit();
        if (status != 0) return status;
        bool more = false;
        status = prepare(more);
        if (status != 0) return status;
        if (!more) return finish();
    }
}
