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

// File scope: MSVC does not treat enclosing-function constants as constant
// expressions inside capturing lambdas (C2131).
static constexpr uint64_t ms = 1000000, target = 100 * ms;

int main() {
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

    // Shared submissions never create estimates. Over the target they make an
    // unmeasured member run alone once; any attributable sample, even one too
    // short to learn from, ends that and prevents later re-isolation.
    cvk_dispatch_cost member;
    member.observe_shared(100 * ms, target);
    CHECK(!member.isolate());
    member.observe_shared(250 * ms, 0);
    CHECK(!member.isolate());
    member.observe_shared(250 * ms, target);
    CHECK(member.isolate() && member.estimate_ns(1 << 20) == 0);
    member.observe(4 * ms, 800, cvk_dispatch_learning_minimum_ns(target));
    CHECK(!member.isolate() && member.estimate_ns(1 << 20) == 0);
    member.observe_shared(900 * ms, target);
    CHECK(!member.isolate());
    cvk_dispatch_cost learned;
    learned.observe(24 * ms, 1000, 25 * ms);
    CHECK(learned.estimate_ns(1000) == 0);
    learned.observe(25 * ms, 1000, 25 * ms);
    CHECK(learned.estimate_ns(1000) == 25 * ms);

    // Closed loop as on a real queue: enqueue, select tiles through the
    // production planner, submit each tile with a fixed 3ms overhead plus a
    // 40ms stall on every seventh submission, learn once per command from the
    // summed time, repeat. Steady state must stay near the target instead of
    // collapsing toward single workgroups. per_tile reproduces the old
    // per-tile learning as a control.
    struct loop_result { uint64_t worst_ns, last_submissions; };
    auto run_loop = [&](uint64_t item_ps, std::array<uint32_t, 3> g,
                        std::array<uint32_t, 3> l, uint32_t geometry,
                        bool per_tile = false) {
        constexpr uint64_t overhead = 3 * ms, stall = 40 * ms;
        const uint64_t minimum = cvk_dispatch_learning_minimum_ns(target);
        cvk_dispatch_cost cost;
        loop_result result{0, 0};
        uint64_t submitted = 0;
        for (unsigned iteration = 0; iteration < 40; ++iteration) {
            const uint32_t b = cvk_select_tile_budget(g, l, geometry, &cost, target);
            uint64_t submissions = 0, total = 0;
            auto submit = [&](uint64_t items) {
                const uint64_t work = items * item_ps / 1000;
                const uint64_t elapsed = overhead + work + (++submitted % 7 ? 0 : stall);
                if (per_tile) cost.observe(elapsed, items, minimum);
                total += elapsed;
                if (iteration) result.worst_ns = std::max(result.worst_ns, overhead + work);
                ++submissions;
            };
            if (!b) {
                submit(cvk_ndrange_items(g));
            } else {
                cvk_ndrange_tiles tiles;
                cvk_ndrange_tile tile;
                CHECK(tiles.init({0, 0, 0}, g, l, {65535, 65535, 65535}, b));
                while (tiles.next(tile)) submit(cvk_ndrange_items(tile.gws));
            }
            if (!per_tile) cost.observe(total, cvk_ndrange_items(g), minimum);
            result.last_submissions = submissions;
        }
        return result;
    };
    const uint64_t minimum = cvk_dispatch_learning_minimum_ns(target);
    CHECK(minimum == 25 * ms);
    // Costs are picoseconds per work item. ~665ms Particle Physics/Fluid-like
    // range that fits the geometry budget: a handful of tiles under 100ms work.
    auto particle = run_loop(10147000, {65536, 1, 1}, {64, 1, 1}, 16384);
    CHECK(particle.worst_ns <= target + 3 * ms);
    CHECK(particle.last_submissions >= 7 && particle.last_submissions <= 10);
    // Cheap kernels below the learning minimum are never tiled.
    auto cheap = run_loop(1000, {1u << 14, 1, 1}, {256, 1, 1}, 16384);
    CHECK(cheap.last_submissions == 1);
    // A cheap (~52ms work) geometry-tiled grid keeps its 92 geometry tiles even
    // though overhead and stalls make the summed command time exceed 100ms.
    auto grayscale = run_loop(2200, {5600, 4200, 1}, {16, 1, 1}, 16384);
    CHECK(grayscale.last_submissions == 92);
    // An 18s Hough NDRange converges near the target, not to single groups.
    auto hough_loop = run_loop(765000, {5600, 4200, 1}, {16, 1, 1}, 16384);
    CHECK(hough_loop.worst_ns <= target + 3 * ms);
    CHECK(hough_loop.last_submissions >= 180 && hough_loop.last_submissions <= 260);
    // Control: per-tile learning lets stalls on small tiles inflate the
    // estimate, multiplying submissions for the same Hough range.
    auto collapsed = run_loop(765000, {5600, 4200, 1}, {16, 1, 1}, 16384, true);
    CHECK(collapsed.last_submissions > 4 * hough_loop.last_submissions);

    // Batched loop: a long and a cheap kernel share each submission. The
    // over-target shared sample isolates both once; afterwards the long one is
    // tiled near the target and the cheap one keeps sharing untiled.
    {
        constexpr uint64_t overhead = 3 * ms;
        cvk_dispatch_cost slow, fast;
        std::array<uint32_t, 3> slow_g{65536, 1, 1}, slow_l{64, 1, 1};
        std::array<uint32_t, 3> fast_g{1u << 16, 1, 1}, fast_l{256, 1, 1};
        constexpr uint64_t slow_ps = 10147000, fast_ps = 50;
        uint64_t worst = 0, isolations = 0, last_shared_fast = 0;
        for (unsigned iteration = 0; iteration < 20; ++iteration) {
            const uint32_t sb = cvk_select_tile_budget(slow_g, slow_l, 16384, &slow, target);
            const uint32_t fb = cvk_select_tile_budget(fast_g, fast_l, 16384, &fast, target);
            CHECK(fb == 0);
            // Mirror cvk_command_kernel::can_be_batched().
            const bool slow_batched = !sb && !slow.isolate();
            const bool fast_batched = !fast.isolate();
            isolations += !slow_batched && !sb;
            isolations += !fast_batched;
            auto alone = [&](cvk_dispatch_cost& cost, uint64_t items, uint64_t ps) {
                const uint64_t elapsed = overhead + items * ps / 1000;
                cost.observe(elapsed, items, minimum);
                if (iteration > 1) worst = std::max(worst, elapsed);
            };
            if (sb) {
                cvk_ndrange_tiles tiles; cvk_ndrange_tile tile;
                CHECK(tiles.init({0, 0, 0}, slow_g, slow_l, {65535, 65535, 65535}, sb));
                uint64_t total = 0;
                while (tiles.next(tile)) {
                    const uint64_t elapsed = overhead + cvk_ndrange_items(tile.gws) * slow_ps / 1000;
                    total += elapsed;
                    if (iteration > 1) worst = std::max(worst, elapsed);
                }
                slow.observe(total, cvk_ndrange_items(slow_g), minimum);
            } else if (!slow_batched) {
                alone(slow, cvk_ndrange_items(slow_g), slow_ps);
            }
            if (slow_batched && fast_batched) {
                const uint64_t elapsed = overhead + cvk_ndrange_items(slow_g) * slow_ps / 1000 +
                                         cvk_ndrange_items(fast_g) * fast_ps / 1000;
                slow.observe_shared(elapsed, target);
                fast.observe_shared(elapsed, target);
            } else if (fast_batched) {
                fast.observe_shared(overhead + cvk_ndrange_items(fast_g) * fast_ps / 1000, target);
                last_shared_fast = iteration;
            } else {
                alone(fast, cvk_ndrange_items(fast_g), fast_ps);
            }
        }
        CHECK(isolations == 2);
        CHECK(worst <= target + 3 * ms);
        CHECK(last_shared_fast == 19 && !fast.isolate() && fast.estimate_ns(1 << 16) == 0);
        bool slow_limited = false;
        const uint32_t final_budget = cvk_select_tile_budget(slow_g, slow_l, 16384, &slow, target, &slow_limited);
        CHECK(slow_limited && final_budget >= 120 && final_budget < 160);
        CHECK(slow.estimate_ns(uint64_t(final_budget) * 64) <= target);
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
