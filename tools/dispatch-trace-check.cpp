// SPDX-License-Identifier: Apache-2.0
#include "../src/dispatch_trace.hpp"
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
#include <vector>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { std::fprintf(stderr, "dispatch trace check %u line %d: %s\n", checks, __LINE__, #c); std::exit(1); } } while (0)
int main() {
    cvk_dispatch_trace trace("hough_scores");
    cvk_dispatch_trace::record input;
    input.command = 0x11; input.event = 0x22; input.kernel = 0x33; input.program = 0x44;
    input.dimensions = 2;
    input.global = {1024, 13, 1}; input.local = {16, 1, 1}; input.offset = {3, 4, 0};
    input.region_global = {512, 13, 1}; input.region_local = {16, 1, 1}; input.region_offset = {512, 0, 0};
    for (unsigned i = 0; i < 100; ++i) {
        trace.command(); trace.dispatch(i % 2 ? "hough_scores" : "unrelated", input);
    }
    input.global = {0, 0, 0}; // Recorder owns its metadata values.
    CHECK(trace.commands == 100 && trace.dispatches == 100 && trace.matched == 50);
    unsigned index = 0;
    trace.each([&](const cvk_dispatch_trace::record& r) {
        CHECK(r.ordinal == 38 + index * 2); ++index;
        CHECK(r.command == 0x11 && r.event == 0x22 && r.kernel == 0x33 && r.program == 0x44);
        CHECK(r.dimensions == 2 && r.global[0] == 1024 && r.global[1] == 13);
        CHECK(r.local[0] == 16 && r.offset[0] == 3);
        CHECK(r.region_global[0] == 512 && r.region_offset[0] == 512);
        CHECK(std::strcmp(r.name.data(), "hough_scores") == 0 && !r.name_truncated);
    });
    CHECK(index == cvk_dispatch_trace::capacity);
    cvk_dispatch_trace empty("absent"); empty.command(); empty.dispatch("hough_scores", input);
    empty.each([](const auto&) { CHECK(false); });
    CHECK(empty.commands == 1 && empty.dispatches == 1 && empty.matched == 0);
    cvk_dispatch_trace all("");
    const std::string long_name(200, 'x'); all.dispatch(long_name.c_str(), input);
    all.each([&](const auto& r) { CHECK(r.name_truncated && std::strlen(r.name.data()) == 95); });
    std::array<uint64_t, 256> ids{};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < 8; ++i) threads.emplace_back([&, i] {
        for (size_t j = 0; j < 32; ++j) { cvk_dispatch_trace local(""); ids[i * 32 + j] = local.id; }
    });
    for (auto& thread : threads) thread.join();
    const std::set<uint64_t> unique(ids.begin(), ids.end());
    CHECK(unique.size() == ids.size() && !unique.count(0));
    const uint64_t before = cvk_dispatch_trace::monotonic_ns();
    CHECK(cvk_dispatch_trace::monotonic_ns() >= before && cvk_dispatch_trace::utc_ns() > 0);
    std::printf("PASS %u production dispatch recorder checks; bounded tail, complete counts, independent geometry and concurrent identities; no GPU execution\n", checks);
}
