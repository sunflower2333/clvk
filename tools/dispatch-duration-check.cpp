// SPDX-License-Identifier: Apache-2.0
#include "../src/dispatch_duration.hpp"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { std::fprintf(stderr, "dispatch duration check %u line %d: %s\n", checks, __LINE__, #c); std::exit(1); } } while (0)

// Independent portable 128-bit oracle (MSVC has no __int128).
struct u128 { uint64_t hi, lo; };
static u128 mul(uint64_t a, uint64_t b) {
    const uint64_t a0 = uint32_t(a), a1 = a >> 32, b0 = uint32_t(b), b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t middle = (p00 >> 32) + uint32_t(p01) + uint32_t(p10);
    return {p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32), (middle << 32) | uint32_t(p00)};
}
static bool fits(u128 v) { return v.hi == 0; }
// ceil(scaled * items / 2^16), saturated to 64 bits.
static uint64_t oracle_estimate(uint64_t scaled, uint64_t items) {
    u128 p = mul(scaled, items);
    const bool round = (p.lo & 0xffff) != 0;
    u128 shifted{p.hi >> 16, (p.lo >> 16) | (p.hi << 48)};
    if (round) { if (++shifted.lo == 0) ++shifted.hi; }
    return fits(shifted) ? shifted.lo : UINT64_MAX;
}
static uint64_t oracle_scaled(uint64_t elapsed, uint64_t items) {
    if (elapsed > (UINT64_MAX >> 16)) return UINT64_MAX / items + (UINT64_MAX % items != 0);
    const uint64_t s = elapsed << 16;
    return s / items + (s % items != 0);
}

int main() {
    constexpr uint64_t ms = 1000000, target = 100 * ms;
    std::array<uint32_t, 3> gws{65536, 1, 1}, lws{64, 1, 1};

    // Unknown shapes keep the geometry policy and ordinary batching.
    cvk_dispatch_cost unknown;
    CHECK(unknown.estimate_ns(1u << 30) == 0);
    CHECK(unknown.tile_budget(target, 64) == 0);
    bool limited = true;
    CHECK(cvk_select_tile_budget(gws, lws, 16384, &unknown, target, &limited) == 0 && !limited);
    CHECK(cvk_select_tile_budget({5600, 4200, 1}, {16, 1, 1}, 16384, &unknown, target, &limited) == 16384 && !limited);
    CHECK(cvk_select_tile_budget(gws, lws, 16384, nullptr, target) == 0);
    CHECK(cvk_select_tile_budget(gws, lws, 16384, &unknown, 0) == 0);

    // Measured ~665ms whole-range dispatch that fits the 16384-group geometry
    // budget (1024 groups): later enqueues are tiled to the 100ms target.
    cvk_dispatch_cost nbody;
    nbody.observe(665 * ms, cvk_ndrange_items(gws));
    const uint64_t whole = nbody.estimate_ns(cvk_ndrange_items(gws));
    CHECK(whole >= 665 * ms && whole <= 665 * ms + 2);
    const uint32_t budget = cvk_select_tile_budget(gws, lws, 16384, &nbody, target, &limited);
    CHECK(limited && budget >= 1 && budget < 1024);
    CHECK(nbody.estimate_ns(uint64_t(budget) * 64) <= target);
    CHECK(nbody.estimate_ns(uint64_t(budget + 1) * 64) > target);
    CHECK(budget == nbody.tile_budget(target, 64));
    CHECK(cvk_ndrange_exceeds_workgroup_budget(gws, lws, budget));
    // Estimates under the target keep an untiled fitting range.
    CHECK(cvk_select_tile_budget({4096, 1, 1}, lws, 16384, &nbody, target, &limited) == 0 && !limited);

    // A single workgroup cannot be split further; keep the ordinary path.
    CHECK(cvk_select_tile_budget({256, 1, 1}, {256, 1, 1}, 16384, &nbody, 1, &limited) == 0 && !limited);
    // A tighter geometry budget wins over a looser duration budget.
    cvk_dispatch_cost hough;
    hough.observe(18000 * ms, 1470000ull * 16);
    const uint32_t hough_duration = hough.tile_budget(target, 16);
    CHECK(hough_duration > 1 && hough_duration < 16384);
    CHECK(cvk_select_tile_budget({5600, 4200, 1}, {16, 1, 1}, 16384, &hough, target, &limited) == hough_duration && limited);
    CHECK(cvk_select_tile_budget({5600, 4200, 1}, {16, 1, 1}, hough_duration / 2, &hough, target, &limited) == hough_duration / 2 && !limited);

    // Peaks are monotonic: a later fast sample never lowers the estimate.
    nbody.observe(1, cvk_ndrange_items(gws));
    CHECK(nbody.estimate_ns(cvk_ndrange_items(gws)) == whole);
    nbody.observe(0, 5); nbody.observe(5, 0);
    CHECK(nbody.estimate_ns(cvk_ndrange_items(gws)) == whole);

    // Saturation and clamping.
    cvk_dispatch_cost huge;
    huge.observe(UINT64_MAX, 1);
    CHECK(huge.estimate_ns(2) == oracle_estimate(UINT64_MAX, 2));
    CHECK(huge.estimate_ns(1ull << 17) == UINT64_MAX);
    CHECK(huge.estimate_ns(UINT64_MAX) == UINT64_MAX);
    CHECK(huge.tile_budget(target, 1) == 1);
    CHECK(huge.tile_budget(UINT64_MAX, UINT64_MAX) == 1);
    cvk_dispatch_cost tiny;
    tiny.observe(1, UINT64_MAX);
    CHECK(tiny.estimate_ns(1) == 1);
    CHECK(tiny.tile_budget(target, 1) == UINT32_MAX);

    // Reports fire once per distinct budget.
    CHECK(nbody.first_report(budget) && !nbody.first_report(budget) && nbody.first_report(budget + 1));

    // Randomized arithmetic against the independent oracle, including the
    // tile boundary property used by real submissions.
    std::mt19937_64 random(0x5eed);
    for (unsigned i = 0; i < 200000; ++i) {
        const uint64_t elapsed = i % 7 == 0 ? random() : random() % (4000 * ms) + 1;
        const uint64_t items = i % 11 == 0 ? random() | 1 : random() % (1ull << 32) + 1;
        const uint64_t query = i % 5 == 0 ? random() : random() % (1ull << 34) + 1;
        cvk_dispatch_cost cost;
        cost.observe(elapsed, items);
        const uint64_t scaled = oracle_scaled(elapsed, items);
        CHECK(cost.estimate_ns(query) == oracle_estimate(scaled, query));
        const uint64_t per_group = random() % 4096 + 1;
        const uint64_t goal = random() % (1000 * ms) + 1;
        const uint32_t b = cost.tile_budget(goal, per_group);
        CHECK(b >= 1);
        u128 group_cost = mul(scaled, per_group);
        if (fits(group_cost) && b > 1 && b < UINT32_MAX) {
            // floor((goal * 2^16) / (scaled * per_group)) == b
            CHECK(fits(mul(group_cost.lo, b)) && mul(group_cost.lo, b).lo <= (goal << 16));
            CHECK(!fits(mul(group_cost.lo, uint64_t(b) + 1)) || mul(group_cost.lo, uint64_t(b) + 1).lo > (goal << 16));
        }
    }

    // Batch window: unknown estimates never split batches; known ones bound
    // the shared submission to the target; one long command stays alone.
    cvk_dispatch_duration_window disabled(0);
    disabled.add(UINT64_MAX);
    CHECK(disabled.accepts(UINT64_MAX));
    cvk_dispatch_duration_window window(target);
    CHECK(window.accepts(UINT64_MAX));
    window.add(0); window.add(0);
    CHECK(window.accepts(0) && window.accepts(target));
    window.add(60 * ms);
    CHECK(window.accepts(40 * ms) && !window.accepts(40 * ms + 1));
    window.add(40 * ms);
    CHECK(!window.accepts(0) && !window.accepts(1));
    cvk_dispatch_duration_window long_first(target);
    CHECK(long_first.accepts(665 * ms));
    long_first.add(665 * ms);
    CHECK(!long_first.accepts(0));

    // Cache identity, invalid keys and eviction with retained in-flight state.
    cvk_dispatch_cost_cache cache;
    cvk_batch_cost_key a, b;
    a.value(1); b.value(2);
    auto first = cache.find(a);
    CHECK(first && cache.find(a) == first && cache.find(b) != first);
    cvk_batch_cost_key invalid;
    invalid.append(nullptr, cvk_batch_cost_key::maximum_bytes + 1);
    CHECK(!invalid.valid && !cache.find(invalid));
    first->observe(665 * ms, 1024);
    for (uint64_t k = 3; k < 3 + cvk_dispatch_cost_cache::capacity; ++k) {
        cvk_batch_cost_key key; key.value(k); cache.find(key);
    }
    CHECK(first->estimate_ns(1024) >= 665 * ms);
    CHECK(cache.find(a) != first && cache.find(a)->estimate_ns(1024) == 0);

    // Concurrent observers converge to the maximum sample.
    cvk_dispatch_cost shared;
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t)
        threads.emplace_back([&shared, t] {
            for (uint64_t n = 1; n <= 5000; ++n) shared.observe(n * (t + 1), 1000);
        });
    for (auto& thread : threads) thread.join();
    CHECK(shared.estimate_ns(1000) == oracle_estimate(oracle_scaled(40000, 1000), 1000));

    std::printf("PASS dispatch duration estimator/tile selection/window/cache: %u checks\n", checks);
    return 0;
}
