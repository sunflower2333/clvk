// SPDX-License-Identifier: Apache-2.0
#include "../src/ndrange_tiles.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { std::fprintf(stderr, "tile check %u line %d: %s\n", checks, __LINE__, #c); std::exit(1); } } while (0)

static unsigned coverage(std::array<uint32_t, 3> gws,
                         std::array<uint32_t, 3> lws, uint32_t budget,
                         std::array<uint32_t, 3> limits = {65535,65535,65535}) {
    const std::array<uint32_t, 3> offset{13,19,23};
    cvk_ndrange_tiles plan;
    CHECK(plan.init(offset, gws, lws, limits, budget));
    std::vector<unsigned char> seen(size_t(gws[0]) * gws[1] * gws[2]);
    cvk_ndrange_tile tile;
    unsigned count = 0;
    while (plan.next(tile)) {
        ++count;
        uint64_t groups = 1;
        for (unsigned d = 0; d < 3; ++d) {
            CHECK(tile.gws[d] && tile.lws[d] && tile.gws[d] % tile.lws[d] == 0);
            CHECK(uint64_t(tile.offset[d]) + tile.gws[d] <= gws[d]);
            CHECK(tile.offset[d] % lws[d] == 0);
            CHECK(tile.lws[d] == lws[d] ||
                (tile.lws[d] == gws[d] % lws[d] && tile.gws[d] == tile.lws[d]));
            const auto n = tile.gws[d] / tile.lws[d];
            CHECK(n <= limits[d]); groups *= n;
        }
        CHECK(groups <= budget);
        for (uint32_t z = tile.offset[2]; z < tile.offset[2] + tile.gws[2]; ++z)
            for (uint32_t y = tile.offset[1]; y < tile.offset[1] + tile.gws[1]; ++y)
                for (uint32_t x = tile.offset[0]; x < tile.offset[0] + tile.gws[0]; ++x) {
                    const size_t i = (size_t(z) * gws[1] + y) * gws[0] + x;
                    CHECK(seen[i]++ == 0);
                    CHECK((x - tile.offset[0]) / tile.lws[0] + tile.offset[0] / lws[0] == x / lws[0]);
                }
    }
    CHECK(!plan.has_next() && !plan.next(tile));
    for (auto n : seen) CHECK(n == 1);
    return count;
}

int main() {
    CHECK(coverage({64,1,1},{16,1,1},4) == 1);
    CHECK(coverage({64,1,1},{16,1,1},1) == 4);
    for (unsigned tail = 0; tail < 8; ++tail)
        for (unsigned budget : {1u,2u,7u,100u})
            coverage({uint32_t(16+(tail&1)), uint32_t(6+((tail>>1)&1)),
                      uint32_t(4+((tail>>2)&1))}, {4,2,2}, budget, {3,2,2});
    coverage({2,1,3},{4,2,4},1); // all full regions empty, one tail group
    cvk_ndrange_tiles plan; cvk_ndrange_tile tile;
    const std::array<uint32_t,3> one{1,1,1}, zero{}, limits{65535,65535,65535};
    CHECK(!plan.init(zero,one,one,limits,0));
    CHECK(!plan.init(zero,zero,one,limits,1));
    CHECK(!plan.init(zero,one,zero,limits,1));
    CHECK(!plan.init(zero,one,one,zero,1));
    CHECK(!plan.init({UINT32_MAX,0,0},{2,1,1},one,limits,1));
    CHECK(!plan.init(zero,{UINT32_MAX,UINT32_MAX,UINT32_MAX},one,limits,1));
    CHECK(plan.init({UINT32_MAX,0,0},one,one,limits,1));
    CHECK(plan.next(tile) && !plan.next(tile));
    CHECK(plan.init(zero,{5600,4200,1},{16,1,1},limits,4096));
    unsigned count = 0; uint64_t groups = 0;
    while (plan.next(tile)) { ++count; groups += uint64_t(tile.gws[0]/16)*tile.gws[1]; }
    CHECK(count == 382 && groups == 1470000);
    CHECK(plan.init(zero,{UINT32_MAX,1,1},one,limits,UINT32_MAX));
    CHECK(plan.next(tile) && tile.gws[0] == 65535); // bounded without eager storage

    const std::string good = "-cl-std=CL1.2 -global-offset -cl-arm-non-uniform-work-group-size";
    cvk_region_abi abi; CHECK(!abi.supported());
    abi.generated(true,good); CHECK(!abi.supported()); abi.validated(); CHECK(abi.supported());
    abi.reset(); abi.validated(); CHECK(!abi.supported());
    abi.generated(false,good); abi.validated(); CHECK(!abi.supported());
    for (const auto* bad : {"", "-global-offset", "-cl-arm-non-uniform-work-group-size",
             "-cl-uniform-work-group-size", "-uniform-workgroup-size",
             "-cl-arm-non-uniform-work-group-size=false", "-global-offset=false"}) {
        abi.generated(true,std::string(bad)); abi.validated(); CHECK(!abi.supported());
        if (*bad) {
            const std::string option(bad);
            if (option.find("=false") != std::string::npos ||
                option == "-uniform-workgroup-size" || option == "-cl-uniform-work-group-size") {
                abi.generated(true,good+" "+bad); abi.validated(); CHECK(!abi.supported());
            }
        }
    }

    // Production executor ordering, with failure injected at every record and
    // every submit boundary. No later tile, replay or success callback is legal.
    for (int fail_submit = -1; fail_submit < 4; ++fail_submit)
        for (int fail_record = -1; fail_record < 3; ++fail_record) {
            int submitted = 0, prepared = 0, finished = 0;
            const int status = cvk_run_tile_submissions(
                [&]() { int n = submitted++; return n == fail_submit ? -5 : 0; },
                [&](bool& more) { more = submitted < 4; if (!more) return 0;
                    CHECK(prepared + 1 == submitted);
                    int n = prepared++; return n == fail_record ? -6 : 0; },
                [&]() { ++finished; return 0; });
            const bool submit_first = fail_submit >= 0 &&
                (fail_record < 0 || fail_submit <= fail_record);
            if (submit_first) CHECK(status == -5 && submitted == fail_submit+1 && !finished);
            else if (fail_record >= 0) CHECK(status == -6 && submitted == fail_record+1 && !finished);
            else CHECK(status == 0 && submitted == 4 && prepared == 3 && finished == 1);
        }
    CHECK(cvk_run_tile_submissions([](){return 0;},[](bool& more){more=false;return 0;},[](){return -7;}) == -7);
    std::printf("PASS %u production tile coverage/provenance/lifecycle checks; no GPU execution\n",checks);
}
