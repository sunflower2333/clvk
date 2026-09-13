// SPDX-License-Identifier: Apache-2.0
#include "../src/calibrated_clock.hpp"
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
int main() {
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
