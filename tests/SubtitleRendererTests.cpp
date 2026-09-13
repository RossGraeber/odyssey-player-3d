#include "app/SubtitleRenderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

odyssey::SubtitleBitmap solidBitmap(
    int x,
    int y,
    int width,
    int height,
    std::uint8_t b,
    std::uint8_t g,
    std::uint8_t r,
    std::uint8_t a) {
    odyssey::SubtitleBitmap bitmap;
    bitmap.x = x;
    bitmap.y = y;
    bitmap.width = width;
    bitmap.height = height;
    bitmap.strideBytes = width * 4;
    bitmap.pixels.resize(static_cast<std::size_t>(bitmap.strideBytes) * height);
    for (std::size_t pixel = 0; pixel < bitmap.pixels.size(); pixel += 4) {
        bitmap.pixels[pixel] = b;
        bitmap.pixels[pixel + 1] = g;
        bitmap.pixels[pixel + 2] = r;
        bitmap.pixels[pixel + 3] = a;
    }
    return bitmap;
}

TEST(SubtitleRenderer, RendersMultipleUtf8TextCuesOnAnOpaqueReadableBacking) {
    odyssey::SubtitleCue first;
    first.textUtf8 = "First line";
    odyssey::SubtitleCue second;
    second.textUtf8 = u8"caf\u00e9 second line";
    const RECT movieRect{100, 50, 900, 450};
    odyssey::SubtitleRenderer renderer;

    const auto single = renderer.render({first}, movieRect, 1000, 500);
    const auto combined = renderer.render({first, second}, movieRect, 1000, 500);

    ASSERT_EQ(single.size(), 1u);
    ASSERT_EQ(combined.size(), 1u);
    EXPECT_GT(combined[0].height, single[0].height);
    EXPECT_GE(combined[0].destination.left, movieRect.left);
    EXPECT_LE(combined[0].destination.right, movieRect.right);
    EXPECT_GE(combined[0].destination.top, movieRect.top);
    EXPECT_LE(combined[0].destination.bottom, movieRect.bottom);

    bool hasBacking = false;
    bool hasText = false;
    for (std::size_t pixel = 0; pixel < combined[0].bgra.size(); pixel += 4) {
        EXPECT_EQ(combined[0].bgra[pixel + 3], 255);
        const std::uint8_t maximum = (std::max)({
            combined[0].bgra[pixel],
            combined[0].bgra[pixel + 1],
            combined[0].bgra[pixel + 2],
        });
        hasBacking = hasBacking || maximum == 0;
        hasText = hasText || maximum > 180;
    }
    EXPECT_TRUE(hasBacking);
    EXPECT_TRUE(hasText);
}

TEST(SubtitleRenderer, PreservesPgsCanvasGeometryPixelsAndDrawOrder) {
    odyssey::SubtitleCue cue;
    cue.sourceWidth = 100;
    cue.sourceHeight = 50;
    cue.bitmaps.push_back(solidBitmap(10, 5, 2, 1, 0, 0, 255, 128));
    cue.bitmaps.push_back(solidBitmap(11, 5, 2, 1, 255, 0, 0, 128));
    const RECT movieRect{100, 50, 900, 450};
    odyssey::SubtitleRenderer renderer;

    const auto rendered = renderer.render({cue}, movieRect, 1000, 500);

    ASSERT_EQ(rendered.size(), 2u);
    EXPECT_EQ(rendered[0].width, 2u);
    EXPECT_EQ(rendered[0].height, 1u);
    EXPECT_EQ(rendered[0].destination.left, 180);
    EXPECT_EQ(rendered[0].destination.top, 90);
    EXPECT_EQ(rendered[0].destination.right, 196);
    EXPECT_EQ(rendered[0].destination.bottom, 98);
    ASSERT_EQ(rendered[0].bgra.size(), 8u);

    EXPECT_EQ(rendered[0].bgra[0], 0);
    EXPECT_EQ(rendered[0].bgra[2], 255);
    EXPECT_EQ(rendered[0].bgra[3], 128);
    EXPECT_EQ(rendered[0].bgra[4], 0);
    EXPECT_EQ(rendered[0].bgra[6], 255);
    EXPECT_EQ(rendered[0].bgra[7], 128);

    EXPECT_EQ(rendered[1].width, 2u);
    EXPECT_EQ(rendered[1].height, 1u);
    EXPECT_EQ(rendered[1].destination.left, 188);
    EXPECT_EQ(rendered[1].destination.top, 90);
    EXPECT_EQ(rendered[1].destination.right, 204);
    EXPECT_EQ(rendered[1].destination.bottom, 98);
    ASSERT_EQ(rendered[1].bgra.size(), 8u);
    EXPECT_EQ(rendered[1].bgra[0], 255);
    EXPECT_EQ(rendered[1].bgra[2], 0);
    EXPECT_EQ(rendered[1].bgra[3], 128);
}

TEST(SubtitleRenderer, MovesTextAboveVisibleControlsButLeavesHiddenPlacement) {
    odyssey::SubtitleCue cue;
    cue.textUtf8 = "Caption";
    const RECT movieRect{100, 50, 900, 450};
    odyssey::SubtitleRenderer renderer;

    const auto hidden = renderer.render({cue}, movieRect, 1000, 500, std::nullopt);
    const auto visible = renderer.render({cue}, movieRect, 1000, 500, 350);

    ASSERT_EQ(hidden.size(), 1u);
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_LT(visible[0].destination.bottom, hidden[0].destination.bottom);
    EXPECT_LE(visible[0].destination.bottom, 350);

    const auto beyondMovie = renderer.render({cue}, movieRect, 1000, 500, 900);
    EXPECT_EQ(beyondMovie[0].destination.top, hidden[0].destination.top);
    EXPECT_EQ(beyondMovie[0].destination.bottom, hidden[0].destination.bottom);
}

TEST(SubtitleRenderer, SafeBottomDoesNotMovePgsBitmaps) {
    odyssey::SubtitleCue cue;
    cue.sourceWidth = 100;
    cue.sourceHeight = 50;
    cue.bitmaps.push_back(solidBitmap(10, 5, 2, 1, 0, 0, 255, 128));
    const RECT movieRect{100, 50, 900, 450};
    odyssey::SubtitleRenderer renderer;

    const auto hidden = renderer.render({cue}, movieRect, 1000, 500, std::nullopt);
    const auto visible = renderer.render({cue}, movieRect, 1000, 500, 350);

    ASSERT_EQ(hidden.size(), 1u);
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(hidden[0].destination.left, visible[0].destination.left);
    EXPECT_EQ(hidden[0].destination.top, visible[0].destination.top);
    EXPECT_EQ(hidden[0].destination.right, visible[0].destination.right);
    EXPECT_EQ(hidden[0].destination.bottom, visible[0].destination.bottom);
}

TEST(SubtitleRenderer, RejectsMalformedBitmapStorageAndGeometry) {
    odyssey::SubtitleCue cue;
    cue.sourceWidth = 10;
    cue.sourceHeight = 10;
    cue.bitmaps.push_back(solidBitmap(9, 0, 2, 1, 0, 0, 0, 255));
    odyssey::SubtitleRenderer renderer;
    const RECT movieRect{0, 0, 100, 100};
    EXPECT_THROW(renderer.render({cue}, movieRect, 100, 100), std::invalid_argument);

    cue.bitmaps[0] = solidBitmap(0, 0, 2, 2, 0, 0, 0, 255);
    cue.bitmaps[0].pixels.pop_back();
    EXPECT_THROW(renderer.render({cue}, movieRect, 100, 100), std::invalid_argument);
}

} // namespace
