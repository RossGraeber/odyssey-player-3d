#include "app/MvcSubtitleTiming.h"

#include <limits>

#include <gtest/gtest.h>

namespace odyssey {

TEST(MvcSubtitleTiming, AddsSegmentAndSampleTimesAtNormalRate) {
    int64_t timestamp = 0;
    ASSERT_TRUE(mvcSubtitleTimestamp100ns(120'000'000, 5'000'000, 1.0, timestamp));
    EXPECT_EQ(timestamp, 125'000'000);
}

TEST(MvcSubtitleTiming, RejectsUnsupportedRateAndOverflow) {
    int64_t timestamp = 0;
    EXPECT_FALSE(mvcSubtitleTimestamp100ns(0, 1, 0.5, timestamp));
    EXPECT_FALSE(mvcSubtitleTimestamp100ns(
        (std::numeric_limits<int64_t>::max)(), 1, 1.0, timestamp));
    EXPECT_FALSE(mvcSubtitleTimestamp100ns(
        (std::numeric_limits<int64_t>::min)(), -1, 1.0, timestamp));
}

} // namespace odyssey
