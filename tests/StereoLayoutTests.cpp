#include <gtest/gtest.h>

#include "app/StereoLayout.h"

using odyssey::NormalizedRect;
using odyssey::StereoMode;
using odyssey::makeStereoLayout;

namespace {

void expectRect(const NormalizedRect& actual,
                double left,
                double top,
                double right,
                double bottom) {
    EXPECT_DOUBLE_EQ(actual.left, left);
    EXPECT_DOUBLE_EQ(actual.top, top);
    EXPECT_DOUBLE_EQ(actual.right, right);
    EXPECT_DOUBLE_EQ(actual.bottom, bottom);
}

} // namespace

TEST(StereoLayout, FullSbs16By9FillsEachDisplayView) {
    const auto layout = makeStereoLayout(
        StereoMode::FullSbs, 3840, 1080, 1, 1, 1920, 1080, false);

    ASSERT_TRUE(layout);
    expectRect(layout->left.source, 0.0, 0.0, 0.5, 1.0);
    expectRect(layout->right.source, 0.5, 0.0, 1.0, 1.0);
    expectRect(layout->left.destination, 0.0, 0.0, 1.0, 1.0);
    expectRect(layout->right.destination, 0.0, 0.0, 1.0, 1.0);
}

TEST(StereoLayout, WideFullSbsLetterboxesAgainstPhysicalDisplayAspect) {
    const auto layout = makeStereoLayout(
        StereoMode::FullSbs, 3840, 804, 1, 1, 1920, 1080, false);

    ASSERT_TRUE(layout);
    EXPECT_NEAR(layout->left.destination.top, 23.0 / 180.0, 1e-12);
    EXPECT_NEAR(layout->left.destination.bottom, 157.0 / 180.0, 1e-12);
    EXPECT_DOUBLE_EQ(layout->left.destination.left, 0.0);
    EXPECT_DOUBLE_EQ(layout->left.destination.right, 1.0);
    EXPECT_NEAR(layout->right.destination.top, layout->left.destination.top, 1e-12);
    EXPECT_NEAR(layout->right.destination.bottom, layout->left.destination.bottom, 1e-12);
}

TEST(StereoLayout, HalfSbsRestoresEachEyeTo16By9) {
    const auto layout = makeStereoLayout(
        StereoMode::HalfSbs, 1280, 720, 1, 1, 1920, 1080, false);

    ASSERT_TRUE(layout);
    expectRect(layout->left.destination, 0.0, 0.0, 1.0, 1.0);
    expectRect(layout->right.destination, 0.0, 0.0, 1.0, 1.0);
}

TEST(StereoLayout, AppliesNonSquareSampleAspectRatio) {
    const auto layout = makeStereoLayout(
        StereoMode::FullSbs, 2880, 1080, 4, 3, 1920, 1080, false);

    ASSERT_TRUE(layout);
    expectRect(layout->left.destination, 0.0, 0.0, 1.0, 1.0);
}

TEST(StereoLayout, SwapEyesExchangesOnlySourceCrops) {
    const auto layout = makeStereoLayout(
        StereoMode::FullSbs, 3840, 1080, 1, 1, 1920, 1080, true);

    ASSERT_TRUE(layout);
    expectRect(layout->left.source, 0.5, 0.0, 1.0, 1.0);
    expectRect(layout->right.source, 0.0, 0.0, 0.5, 1.0);
    expectRect(layout->left.destination, 0.0, 0.0, 1.0, 1.0);
    expectRect(layout->right.destination, 0.0, 0.0, 1.0, 1.0);
}

TEST(StereoLayout, MonoDuplicatesTheFullSource) {
    const auto layout = makeStereoLayout(
        StereoMode::Mono2D, 1920, 1080, 1, 1, 1920, 1080, true);

    ASSERT_TRUE(layout);
    expectRect(layout->left.source, 0.0, 0.0, 1.0, 1.0);
    expectRect(layout->right.source, 0.0, 0.0, 1.0, 1.0);
    expectRect(layout->left.destination, 0.0, 0.0, 1.0, 1.0);
}

TEST(StereoLayout, NarrowMonoPillarboxesAgainstPhysicalDisplayAspect) {
    const auto layout = makeStereoLayout(
        StereoMode::Mono2D, 1440, 1080, 1, 1, 1920, 1080, false);

    ASSERT_TRUE(layout);
    expectRect(layout->left.destination, 0.125, 0.0, 0.875, 1.0);
    expectRect(layout->right.destination, 0.125, 0.0, 0.875, 1.0);
}

TEST(StereoLayout, RejectsInvalidGeometry) {
    EXPECT_FALSE(makeStereoLayout(StereoMode::FullSbs, 0, 1080, 1, 1, 1920, 1080, false));
    EXPECT_FALSE(makeStereoLayout(StereoMode::FullSbs, 3840, -1, 1, 1, 1920, 1080, false));
    EXPECT_FALSE(makeStereoLayout(StereoMode::FullSbs, 3840, 1080, 0, 1, 1920, 1080, false));
    EXPECT_FALSE(makeStereoLayout(StereoMode::FullSbs, 3840, 1080, 1, -1, 1920, 1080, false));
    EXPECT_FALSE(makeStereoLayout(StereoMode::FullSbs, 3840, 1080, 1, 1, 0, 1080, false));
    EXPECT_FALSE(makeStereoLayout(StereoMode::FullSbs, 3840, 1080, 1, 1, 1920, 0, false));
    EXPECT_FALSE(makeStereoLayout(StereoMode::FullSbs, 3839, 1080, 1, 1, 1920, 1080, false));
    EXPECT_TRUE(makeStereoLayout(StereoMode::Mono2D, 3839, 1080, 1, 1, 1920, 1080, false));
}
