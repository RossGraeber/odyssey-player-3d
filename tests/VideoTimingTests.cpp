#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

#include "app/PtsMath.h"
#include "app/VideoTiming.h"

using odyssey::VideoTimingDecision;
using odyssey::decideVideoTiming;

TEST(VideoTiming, WaitsForEarlyFrame) {
    EXPECT_EQ(decideVideoTiming(1'000, 40, 999), VideoTimingDecision::Wait);
}

TEST(VideoTiming, PresentsFrameWhileItIsDue) {
    EXPECT_EQ(decideVideoTiming(1'000, 40, 1'000), VideoTimingDecision::Present);
    EXPECT_EQ(decideVideoTiming(1'000, 40, 1'039), VideoTimingDecision::Present);
}

TEST(VideoTiming, DropsFrameAtEndOfItsDuration) {
    EXPECT_EQ(decideVideoTiming(1'000, 40, 1'040), VideoTimingDecision::Drop);
    EXPECT_EQ(decideVideoTiming(1'000, 40, 1'100), VideoTimingDecision::Drop);
}

TEST(VideoTiming, Preserves24000Over1001CadenceFromAbsolutePts) {
    const int64_t frame23Pts = odyssey::pts::toNs(23, 1001, 24000);
    const int64_t frame24Pts = odyssey::pts::toNs(24, 1001, 24000);

    EXPECT_EQ(frame24Pts, 1'001'000'000);
    EXPECT_EQ(decideVideoTiming(frame24Pts, frame24Pts - frame23Pts, 1'000'000'000),
              VideoTimingDecision::Wait);
    EXPECT_EQ(decideVideoTiming(frame24Pts, frame24Pts - frame23Pts, frame24Pts),
              VideoTimingDecision::Present);
}

TEST(VideoTiming, HandlesNegativeInitialPts) {
    EXPECT_EQ(decideVideoTiming(-50, 40, -51), VideoTimingDecision::Wait);
    EXPECT_EQ(decideVideoTiming(-50, 40, -50), VideoTimingDecision::Present);
    EXPECT_EQ(decideVideoTiming(-50, 40, -10), VideoTimingDecision::Drop);
}

TEST(VideoTiming, PresentsDueFrameWhenDurationIsAbsentOrInvalid) {
    EXPECT_EQ(decideVideoTiming(100, std::nullopt, 1'000), VideoTimingDecision::Present);
    EXPECT_EQ(decideVideoTiming(100, 0, 1'000), VideoTimingDecision::Present);
    EXPECT_EQ(decideVideoTiming(100, -1, 1'000), VideoTimingDecision::Present);
}

TEST(VideoTiming, LateComparisonDoesNotOverflowAtTimestampExtremes) {
    EXPECT_EQ(decideVideoTiming(std::numeric_limits<int64_t>::min(), 1,
                                std::numeric_limits<int64_t>::max()),
              VideoTimingDecision::Drop);
}
