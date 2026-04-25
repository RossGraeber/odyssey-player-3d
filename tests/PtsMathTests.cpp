#include <gtest/gtest.h>

#include <cstdint>
#include "app/PtsMath.h"

using odyssey::pts::toNs;
using odyssey::pts::fromNs;

TEST(PtsMath, MpegTsTimebase90kHzOneSecond) {
    // 1/90000 s tick. 90000 ticks == 1 second == 1e9 ns.
    EXPECT_EQ(toNs(90000, 1, 90000), 1'000'000'000LL);
    EXPECT_EQ(toNs(0,     1, 90000), 0LL);
    EXPECT_EQ(toNs(-90000, 1, 90000), -1'000'000'000LL);
}

TEST(PtsMath, MatroskaMillisecondTimebase) {
    // 1/1000 s. pts=1500 -> 1.5 s -> 1.5e9 ns.
    EXPECT_EQ(toNs(1500, 1, 1000), 1'500'000'000LL);
}

TEST(PtsMath, NtscFrameDurationRoundsSensibly) {
    // 30000/1001 fps -> one-frame tick = 1001/30000 s = 33366666.66... ns.
    // We accept integer truncation.
    const int64_t tick = toNs(1, 1001, 30000);
    EXPECT_EQ(tick, 33'366'666LL);
}

TEST(PtsMath, RoundTripExactWhenTickIsIntegerNs) {
    // toNs truncates sub-ns remainder, so round-trip is exact only when the
    // tick duration is an integer number of ns. For 1/1000 (MKV ms), a tick
    // is exactly 1 000 000 ns → perfect round-trip for all values.
    for (int64_t pts : {0LL, 1LL, 1500LL, 3'600LL * 1000LL}) {
        EXPECT_EQ(fromNs(toNs(pts, 1, 1000), 1, 1000), pts) << "pts=" << pts;
    }
    // For 1/90000 (MPEG-TS) the tick is 11111.11... ns; sub-tick precision
    // is lost. Round-trip is still exact at multiples of 90000 where the
    // nanosecond value hits a whole second.
    for (int64_t pts : {0LL, 90000LL, 3'600LL * 90000LL}) {
        EXPECT_EQ(fromNs(toNs(pts, 1, 90000), 1, 90000), pts) << "pts=" << pts;
    }
}

TEST(PtsMath, FromNsTruncatesTowardZero) {
    // 33.4 ms -> less than one tick at 30000/1001 fps (33.366 ms/tick).
    EXPECT_EQ(fromNs(33'000'000LL, 1001, 30000), 0LL);
    // Exactly one tick boundary.
    EXPECT_EQ(fromNs(33'366'666LL, 1001, 30000), 0LL); // truncation leaves <1
    EXPECT_EQ(fromNs(33'366'667LL, 1001, 30000), 1LL);
}

TEST(PtsMath, LargePtsNoOverflow) {
    // 3-hour stream at 1/90000 timebase.
    const int64_t threeHoursTicks = 3LL * 3600LL * 90000LL;  // 9.72e8
    const int64_t ns = toNs(threeHoursTicks, 1, 90000);
    EXPECT_EQ(ns, 3LL * 3600LL * 1'000'000'000LL);
}
