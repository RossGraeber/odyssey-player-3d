#pragma once

#include "SubtitleCue.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
struct AVCodecParameters;
struct AVPacket;
}

namespace odyssey {

struct SubtitleDecodeOutput {
    std::vector<SubtitleCue> cues;
    std::optional<int64_t> clearAtNanoseconds;
};

class SubtitleDecoder {
public:
    SubtitleDecoder();
    ~SubtitleDecoder();

    SubtitleDecoder(const SubtitleDecoder&) = delete;
    SubtitleDecoder& operator=(const SubtitleDecoder&) = delete;

    bool open(const AVCodecParameters* parameters, int timeBaseNumerator,
              int timeBaseDenominator, int fallbackCanvasWidth,
              int fallbackCanvasHeight, std::string& error);
    bool decode(const AVPacket* packet, SubtitleDecodeOutput& output,
                std::string& error);
    void flush();
    void close();

    static bool parseSrt(std::string_view utf8, std::vector<SubtitleCue>& cues,
                         std::string& error);
    static bool parseSrtFile(const std::wstring& path,
                             std::vector<SubtitleCue>& cues, std::string& error);
    static std::vector<SubtitleCue> activeCues(
        const std::vector<SubtitleCue>& cues, int64_t mediaNanoseconds,
        int64_t offsetNanoseconds);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace odyssey
