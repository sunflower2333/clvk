// SPDX-License-Identifier: Apache-2.0
#include "../src/calibrated_clock.hpp"
#include "../src/device_timestamp.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#ifdef _WIN32
#include <windows.h>
#endif

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { std::fprintf(stderr,"clock check line %d: %s\n",__LINE__,#c); std::exit(1); } } while (0)
static void expected(uint64_t ticks,uint64_t frequency,uint64_t value) {
    uint64_t result=123;CHECK(cvk_clock_to_ns(ticks,frequency,result));CHECK(result==value);
}
static void check_device_timestamps() {
    constexpr uint64_t wrap = uint64_t(1) << 48;
    uint64_t first = 17, last = 23;
    // One physical command straddles wrap, with calibration on either side.
    CHECK(cvk_timestamp_pair_to_host(wrap-3, 5, 10, 1000, 48, 1, first, last));
    CHECK(first == 987 && last == 995);
    CHECK(cvk_timestamp_pair_to_host(wrap-3, 5, wrap-10, 1000, 48, 1, first, last));
    CHECK(first == 1007 && last == 1015);
    CHECK(cvk_timestamp_pair_to_host(UINT64_MAX-2, 5, 10, 1000, 64, 1, first, last));
    CHECK(first == 987 && last == 995);
    CHECK(cvk_timestamp_pair_to_host(wrap-3, 5, 10, 1000, 48, 2.5, first, last));
    CHECK(first == 967 && last == 987);
    // Unused upper query bits must not contaminate48bit time differences.
    CHECK(cvk_timestamp_pair_to_host(UINT64_MAX-2, wrap+5, 10, 1000, 48, 1, first, last));
    CHECK(first == 987 && last == 995);
    CHECK(!cvk_timestamp_pair_to_host(5, 4, 10, 1000, 48, 1, first, last));
    CHECK(!cvk_timestamp_pair_to_host(0, wrap/2, 10, 1000, 48, 1, first, last));
    CHECK(!cvk_timestamp_pair_to_host(wrap/2, wrap/2+1, 0, 1000, 48, 1, first, last));
    CHECK(!cvk_timestamp_pair_to_host(0, 1, 10, 5, 48, 1, first, last));
    CHECK(!cvk_timestamp_pair_to_host(1, 2, 0, UINT64_MAX, 48, 1, first, last));
    CHECK(!cvk_timestamp_pair_to_host(0, 1, 0, 1000, 0, 1, first, last));
    CHECK(!cvk_timestamp_pair_to_host(0, 1, 0, 1000, 65, 1, first, last));
    CHECK(!cvk_timestamp_pair_to_host(0, 1, 0, 1000, 48, 0, first, last));
    CHECK(!cvk_timestamp_pair_to_host(0, 1, 0, 1000, 48, INFINITY, first, last));
    CHECK(!cvk_timestamp_pair_to_host(0, 1, 0, 1000, 48, NAN, first, last));
    // Exact full-width1ns counters must preserve low bits above2^53.
    const uint64_t large = (uint64_t(1) << 54) + 1;
    CHECK(cvk_timestamp_pair_to_host(large, large+1, 0, 0, 64, 1, first, last));
    CHECK(first == large && last == large+1);
    // Independent signed-rational oracle: generate unwrapped nearby coordinates
    // and mask only the public counter representation. Includes raw wrap, events
    // before/after calibration, adjacent commands and fractional-ns periods.
    uint64_t state = 0xfedcba9876543210ull;
    for (uint32_t bits : {8u, 16u, 32u, 48u, 64u}) {
        const uint64_t mask = UINT64_MAX >> (64-bits);
        for (unsigned i=0; i<2000; ++i) {
            state = state*6364136223846793005ull + 1;
            const uint64_t anchor = state & mask;
            const int64_t begin = int64_t((state >> 10)%81)-40;
            const int64_t end = begin + int64_t((state >> 20)%20);
            const auto expected_ns = [](int64_t offset) {
                const int64_t numerator = offset*5;
                const int64_t scaled = numerator>=0 ? numerator/2 : -((-numerator+1)/2);
                return uint64_t(1000000+scaled);
            };
            CHECK(cvk_timestamp_pair_to_host(anchor+uint64_t(begin), anchor+uint64_t(end),
                anchor, 1000000, bits, 2.5, first, last));
            CHECK(first == expected_ns(begin) && last == expected_ns(end));
            uint64_t next_first, next_last;
            CHECK(cvk_timestamp_pair_to_host(anchor+uint64_t(end), anchor+uint64_t(end+1),
                anchor, 1000000, bits, 2.5, next_first, next_last));
            CHECK(last == next_first && next_first <= next_last);
        }
    }
    std::puts("DEVICE_COUNTER_WRAP_PASS bits=8,16,32,48,64 raw-pairs=20000; no GPU execution");
}
int main() {
    check_device_timestamps();
    uint64_t result=42;
    CHECK(!cvk_clock_to_ns(1,0,result)&&result==42);
    CHECK(!cvk_clock_to_ns(UINT64_MAX,1,result)&&result==42);
    expected(0,1,0);expected(1,3,333333333);expected(2,3,666666666);
    expected(10000001,10000000,1000000100);
    expected(UINT64_MAX,1000000000,UINT64_MAX);
    expected(UINT64_MAX-1,UINT64_MAX,999999999);
    expected(UINT64_MAX,UINT64_MAX,1000000000);
    expected(UINT64_MAX,UINT64_MAX/2,2000000000);
    expected(UINT64_MAX,UINT64_MAX/2+1,1999999999);
    expected(18446744073000000000ull,1000000000,18446744073000000000ull);
    // Independent exact oracle where the full multiply is representable.
    for(uint64_t frequency : {3ull,10000000ull,19200000ull,2400000000ull})
        for(uint64_t ticks=0;ticks<10000000;ticks+=9973)
            expected(ticks,frequency,ticks*1000000000/frequency);
#if defined(__SIZEOF_INT128__)
    // Full-width independent oracle for the portable no-128-bit production
    // implementation, including large remainders that take its slow path.
    uint64_t state=0x123456789abcdef0ull;
    for(unsigned i=0;i<10000;++i) {
        state=state*6364136223846793005ull+1;const uint64_t ticks=state;
        state=state*6364136223846793005ull+1;const uint64_t frequency=state|1;
        const auto oracle=static_cast<unsigned __int128>(ticks)*1000000000/frequency;
        uint64_t ns=0;const bool okay=cvk_clock_to_ns(ticks,frequency,ns);
        CHECK(okay==(oracle<=UINT64_MAX));
        if(okay) CHECK(ns==uint64_t(oracle));
    }
#endif
#ifdef _WIN32
    LARGE_INTEGER frequency{};
    CHECK(QueryPerformanceFrequency(&frequency)&&frequency.QuadPart>0);
    uint64_t previous=0;
    for(unsigned i=0;i<1000;++i) {
        const auto start=std::chrono::steady_clock::now();
        LARGE_INTEGER sample{};CHECK(QueryPerformanceCounter(&sample));
        const auto end=std::chrono::steady_clock::now();
        uint64_t ns=0;CHECK(cvk_clock_to_ns(uint64_t(sample.QuadPart),uint64_t(frequency.QuadPart),ns));
        const auto first=std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count();
        const auto last=std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count();
        CHECK(ns>=uint64_t(first)&&ns<=uint64_t(last)&&ns>=previous);
        previous=ns;
    }
    std::printf("QPC_HOST_EPOCH_PASS frequency=%llu bits=%zu samples=1000\n",(unsigned long long)frequency.QuadPart,sizeof(void*)*8);
#endif
    std::printf("PASS %u production calibrated clock conversion checks; no GPU execution\n",checks);
}
