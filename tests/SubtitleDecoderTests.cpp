#include "app/SubtitleDecoder.h"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace odyssey {
namespace {

constexpr int64_t kSecond = 1'000'000'000;

TEST(SubtitleDecoderTests, ParsesOverlappingUtf8SrtAndAppliesOffset) {
    constexpr std::string_view source =
        "1\r\n00:00:01,000 --> 00:00:03,000\r\nFirst\r\n\r\n"
        "2\n00:00:02.000 --> 00:00:04.000\n<b>Second</b>\nline\n";
    std::vector<SubtitleCue> cues;
    std::string error;

    ASSERT_TRUE(SubtitleDecoder::parseSrt(source, cues, error)) << error;
    ASSERT_EQ(cues.size(), 2u);
    EXPECT_EQ(cues[1].textUtf8, "Second\nline");

    const auto active = SubtitleDecoder::activeCues(cues, 2 * kSecond + 500'000'000,
                                                     500'000'000);
    ASSERT_EQ(active.size(), 2u);
    EXPECT_EQ(active[0].startNanoseconds, 1 * kSecond + 500'000'000);
    EXPECT_EQ(active[1].endNanoseconds, 4 * kSecond + 500'000'000);
}

TEST(SubtitleDecoderTests, RejectsMalformedRangesAndInvalidUtf8) {
    std::vector<SubtitleCue> cues;
    std::string error;
    EXPECT_FALSE(SubtitleDecoder::parseSrt(
        "1\n00:00:02,000 --> 00:00:01,000\nbackwards\n", cues, error));
    EXPECT_TRUE(cues.empty());

    const std::string invalidUtf8{"1\n00:00:00,000 --> 00:00:01,000\n\xC3\x28\n"};
    EXPECT_FALSE(SubtitleDecoder::parseSrt(invalidUtf8, cues, error));
    EXPECT_TRUE(cues.empty());

    EXPECT_FALSE(SubtitleDecoder::parseSrt(
        "1\n00:00:00,1 --> 00:00:01,000\nambiguous fraction\n", cues, error));
    EXPECT_TRUE(cues.empty());
}

TEST(SubtitleDecoderTests, KeepsIndefiniteBitmapCueUntilAConsumerClearsIt) {
    SubtitleCue cue;
    cue.startNanoseconds = kSecond;
    cue.bitmaps.emplace_back();
    const std::vector<SubtitleCue> cues{cue};

    EXPECT_TRUE(SubtitleDecoder::activeCues(cues, kSecond - 1, 0).empty());
    EXPECT_EQ(SubtitleDecoder::activeCues(cues, 60 * kSecond, 0).size(), 1u);
}

TEST(SubtitleDecoderTests, DecodesEmbeddedSubRipAssPayloadToPlainText) {
    AVCodecParameters* parameters = avcodec_parameters_alloc();
    ASSERT_NE(parameters, nullptr);
    parameters->codec_type = AVMEDIA_TYPE_SUBTITLE;
    parameters->codec_id = AV_CODEC_ID_SUBRIP;

    SubtitleDecoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(parameters, 1, 1'000, 1920, 1080, error)) << error;
    avcodec_parameters_free(&parameters);

    constexpr char payload[] = "{\\i1}Hello{\\i0}\\Nworld";
    AVPacket* packet = av_packet_alloc();
    ASSERT_NE(packet, nullptr);
    ASSERT_GE(av_new_packet(packet, static_cast<int>(std::strlen(payload))), 0);
    std::memcpy(packet->data, payload, std::strlen(payload));
    packet->pts = 1'000;
    packet->duration = 2'000;

    SubtitleDecodeOutput output;
    ASSERT_TRUE(decoder.decode(packet, output, error)) << error;
    av_packet_free(&packet);
    ASSERT_EQ(output.cues.size(), 1u);
    EXPECT_EQ(output.cues[0].textUtf8, "Hello\nworld");
    EXPECT_EQ(output.cues[0].startNanoseconds, kSecond);
    EXPECT_EQ(output.cues[0].endNanoseconds, 3 * kSecond);
}

} // namespace
} // namespace odyssey
