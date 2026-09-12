// SPDX-License-Identifier: Apache-2.0
#include "../src/batch_duration.hpp"
#include "../src/dispatch_trace.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { std::fprintf(stderr, "batch duration check %u line %d: %s\n", checks, __LINE__, #c); std::exit(1); } } while (0)
int main() {
    constexpr uint64_t ms = 1000000, budget = 100 * ms;
    cvk_batch_cost_cache cache;
    cvk_batch_cost_key key; key.value(5600); key.value(4200); key.value(16);
    auto cold = cache.find(key);
    // All five commands can be recorded before the first one finishes. Each
    // must form its own first batch, regardless of unrelated prior fast work.
    for (unsigned i = 0; i < 5; ++i) {
        cvk_batch_duration_window batch(budget);
        CHECK(cold->estimate() == 0 && batch.accepts(cold->estimate()));
        batch.add(cold->estimate());
        CHECK(batch.full() && !batch.accepts(cold->estimate()));
    }
    cold->observe(10 * ms, budget, true);
    CHECK(cold->estimate() == 20 * ms);
    cvk_batch_duration_window warm(budget);
    for (unsigned i = 0; i < 5; ++i) {
        CHECK(warm.accepts(cold->estimate())); warm.add(cold->estimate());
        CHECK(warm.full() == (i == 4));
    }
    CHECK(!warm.accepts(cold->estimate()));
    cold->observe(3 * ms, budget, true); CHECK(cold->estimate() == 20 * ms);
    cold->observe(17 * ms, budget, true); CHECK(cold->estimate() == 34 * ms);
    cvk_batch_cost_key changed = key; changed.value(7); // Different scalar key.
    auto unseen = cache.find(changed); CHECK(unseen != cold && unseen->estimate() == 0);
    cvk_batch_duration_window mixed(budget); mixed.add(cold->estimate());
    CHECK(!mixed.accepts(unseen->estimate()));
    cvk_batch_duration_window disabled(0); disabled.add(0);
    CHECK(disabled.accepts(UINT64_MAX) && !disabled.full());
    disabled.add(UINT64_MAX); disabled.add(7); CHECK(!disabled.full());
    cvk_batch_duration_window huge(10); huge.add(UINT64_MAX);
    CHECK(huge.full() && !huge.accepts(1));

    // Exact observed false-success boundary: never divide the 2.019s batch
    // by five first and accidentally learn a short per-command estimate.
    auto sample = cvk_batch_cost_sample(2019395468, 1000 * ms, true, 5, true);
    CHECK(sample == 2019395468);
    unseen->observe(sample, 1000 * ms, true);
    unseen->observe(ms, 1000 * ms, true);
    CHECK(unseen->estimate() == 0); // This key stays isolated after over-budget work.
    cold->observe(1, budget, false); cold->observe(ms, budget, true);
    CHECK(cold->estimate() == 0);
    CHECK(cvk_batch_cost_sample(51, 100, true, 5, true) == 11);
    CHECK(cvk_batch_cost_sample(51, 100, true, 5, false) == 51);
    CHECK(cvk_batch_cost_sample(51, 100, false, 5, true) == 51);
    CHECK(cvk_batch_cost_sample(51, 100, true, 0, true) == 51);
    cvk_batch_cost zero; zero.observe(0, budget, true); CHECK(zero.estimate() == 0);
    zero.observe(1, 0, true); CHECK(zero.estimate() == 0);
    cvk_batch_cost saturating; saturating.observe(UINT64_MAX - 1, UINT64_MAX, true);
    CHECK(saturating.estimate() == UINT64_MAX);

    // The bounded cache may evict a key while older commands still retain it.
    cvk_batch_cost_cache bounded;
    auto retained = bounded.find(key); retained->observe(ms, budget, true);
    for (unsigned i = 0; i < cvk_batch_cost_cache::capacity; ++i) {
        cvk_batch_cost_key other; other.value(i); CHECK(bounded.find(other) != retained);
    }
    CHECK(retained->estimate() == 2 * ms);
    CHECK(bounded.find(key) != retained && bounded.find(key)->estimate() == 0);
    std::array<uint8_t, cvk_batch_cost_key::maximum_bytes + 1> large{};
    cvk_batch_cost_key invalid; invalid.append(large.data(), large.size());
    CHECK(!invalid.valid && invalid.bytes.empty() && !bounded.find(invalid));
    invalid.value(1); CHECK(!invalid.valid && invalid.bytes.empty());
    cvk_batch_cost_key full; full.append(large.data(), large.size() - 1);
    CHECK(full.valid && bounded.find(full)); full.value(1); CHECK(!full.valid);
    std::array<std::shared_ptr<cvk_batch_cost>, 8> parallel;
    std::array<std::thread, 8> threads;
    for (size_t i = 0; i < threads.size(); ++i) threads[i] = std::thread([&, i] {
        parallel[i] = bounded.find(changed); parallel[i]->observe((i + 1) * ms, budget, true);
    });
    for (auto& thread : threads) thread.join();
    for (auto& item : parallel) CHECK(item == parallel[0] && item->estimate() == 16 * ms);

    // Production metadata copies only bounded POD ranges, never buffer data.
    cvk_dispatch_arguments::argument argument;
    const std::array<uint8_t, 8> bytes{0, 1, 2, 3, 0xff, 0x80, 0, 7};
    argument.capture_scalar(bytes.data(), bytes.size(), 4, 4);
    CHECK(argument.scalar_valid && argument.scalar_bytes == 4 && !argument.scalar_truncated);
    CHECK(std::strcmp(argument.scalar_hex().data(), "ff800007") == 0);
    argument.capture_scalar(bytes.data(), bytes.size(), SIZE_MAX, 4);
    CHECK(!argument.scalar_valid && argument.scalar_bytes == 0);
    argument.capture_scalar(bytes.data(), bytes.size(), 7, SIZE_MAX);
    CHECK(!argument.scalar_valid && argument.scalar_bytes == 0);
    argument.capture_scalar(nullptr, 0, 0, 4); CHECK(!argument.scalar_valid);
    argument.capture_scalar(nullptr, 0, 0, 0); CHECK(argument.scalar_valid && argument.scalar_bytes == 0);
    argument.capture_scalar(large.data(), large.size(), 0, large.size());
    CHECK(argument.scalar_valid && argument.scalar_bytes == 32 && argument.scalar_truncated);
    CHECK(std::strlen(argument.scalar_hex().data()) == 64);
    CHECK(cvk_dispatch_arguments::text(argument.name, std::string(100, 'x')));
    CHECK(std::strlen(argument.name.data()) == 63);
    auto metadata = std::make_shared<cvk_dispatch_arguments>(); metadata->total = 6;
    metadata->args[0] = argument;
    std::weak_ptr<const cvk_dispatch_arguments> weak = metadata;
    cvk_dispatch_trace::record record; record.arguments = metadata;
    cvk_dispatch_trace trace(""); trace.dispatch("any_kernel", record);
    metadata.reset(); record.arguments.reset(); CHECK(!weak.expired());
    trace.each([&](const auto& r) { CHECK(r.arguments && r.arguments->total == 6 && r.arguments->args[0].scalar_bytes == 32); });
    for (size_t i = 0; i < cvk_dispatch_trace::capacity; ++i) trace.dispatch("other", record);
    CHECK(weak.expired());
    std::printf("PASS %u production duration/metadata checks: first unseen commands isolated, bounded admission/cache, no over-budget false-success training, copied metadata; no GPU execution\n", checks);
}
