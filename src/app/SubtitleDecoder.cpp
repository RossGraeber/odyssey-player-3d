#include "SubtitleDecoder.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace odyssey {

namespace {

constexpr int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr size_t kMaximumSrtBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumSrtCues = 100'000;
constexpr size_t kMaximumCueTextBytes = 64 * 1024;
constexpr size_t kMaximumBitmapBytes = 64 * 1024 * 1024;

std::string ffmpegError(const char* operation, int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(error, text.data(), text.size());
    return std::string(operation) + ": " + text.data();
}

int64_t saturatingAdd(int64_t left, int64_t right) {
    if (right > 0 && left > (std::numeric_limits<int64_t>::max)() - right)
        return (std::numeric_limits<int64_t>::max)();
    if (right < 0 && left < (std::numeric_limits<int64_t>::min)() - right)
        return (std::numeric_limits<int64_t>::min)();
    return left + right;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    return value;
}

bool parseUnsigned(std::string_view value, unsigned& result) {
    if (value.empty()) return false;
    const char* end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parseSrtTimestamp(std::string_view value, int64_t& nanoseconds) {
    value = trim(value);
    const size_t firstColon = value.find(':');
    const size_t secondColon = firstColon == std::string_view::npos
        ? std::string_view::npos : value.find(':', firstColon + 1);
    size_t separator = secondColon == std::string_view::npos
        ? std::string_view::npos : value.find(',', secondColon + 1);
    if (separator == std::string_view::npos && secondColon != std::string_view::npos)
        separator = value.find('.', secondColon + 1);
    if (firstColon == std::string_view::npos || secondColon == std::string_view::npos ||
        separator == std::string_view::npos) {
        return false;
    }
    unsigned hours = 0;
    unsigned minutes = 0;
    unsigned seconds = 0;
    unsigned milliseconds = 0;
    if (!parseUnsigned(value.substr(0, firstColon), hours) ||
        !parseUnsigned(value.substr(firstColon + 1, secondColon - firstColon - 1), minutes) ||
        !parseUnsigned(value.substr(secondColon + 1, separator - secondColon - 1), seconds) ||
        value.size() - separator - 1 != 3 ||
        !parseUnsigned(value.substr(separator + 1), milliseconds) ||
        minutes >= 60 || seconds >= 60 || milliseconds >= 1'000) {
        return false;
    }
    constexpr uint64_t kMaximumMilliseconds =
        static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) / 1'000'000;
    const uint64_t totalMilliseconds =
        ((static_cast<uint64_t>(hours) * 60 + minutes) * 60 + seconds) * 1'000 + milliseconds;
    if (totalMilliseconds > kMaximumMilliseconds) return false;
    nanoseconds = static_cast<int64_t>(totalMilliseconds * 1'000'000);
    return true;
}

bool validUtf8(std::string_view text) {
    if (text.empty()) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()), nullptr, 0) > 0;
}

std::string plainText(std::string_view input, bool ass) {
    if (ass) {
        size_t comma = 0;
        for (int field = 0; field < 8; ++field) {
            comma = input.find(',', comma);
            if (comma == std::string_view::npos) break;
            ++comma;
        }
        if (comma < input.size()) input.remove_prefix(comma);
    }

    std::string result;
    result.reserve(input.size());
    bool inAssTag = false;
    bool inHtmlTag = false;
    for (size_t i = 0; i < input.size(); ++i) {
        const char value = input[i];
        if (value == '{') {
            inAssTag = true;
            continue;
        }
        if (inAssTag) {
            if (value == '}') inAssTag = false;
            continue;
        }
        if (value == '<') {
            inHtmlTag = true;
            continue;
        }
        if (inHtmlTag) {
            if (value == '>') inHtmlTag = false;
            continue;
        }
        if (value == '\\' && i + 1 < input.size()) {
            const char escaped = input[i + 1];
            if (escaped == 'N' || escaped == 'n') {
                result.push_back('\n');
                ++i;
                continue;
            }
            if (escaped == 'h') {
                result.push_back(' ');
                ++i;
                continue;
            }
        }
        if (value == '\r') continue;
        result.push_back(value);
    }
    return result;
}

bool makeBitmap(const AVSubtitleRect& rect, SubtitleBitmap& bitmap,
                std::string& error) {
    if (rect.x < 0 || rect.y < 0 || rect.w <= 0 || rect.h <= 0 ||
        rect.w > (std::numeric_limits<int>::max)() / 4 ||
        rect.linesize[0] < rect.w ||
        !rect.data[0] || !rect.data[1]) {
        error = "subtitle bitmap rectangle is malformed";
        return false;
    }
    const uint64_t pixelCount = static_cast<uint64_t>(rect.w) * rect.h;
    if (pixelCount > (std::numeric_limits<size_t>::max)() / 4 ||
        pixelCount * 4 > kMaximumBitmapBytes) {
        error = "subtitle bitmap rectangle is too large";
        return false;
    }
    bitmap.x = rect.x;
    bitmap.y = rect.y;
    bitmap.width = rect.w;
    bitmap.height = rect.h;
    bitmap.strideBytes = rect.w * 4;
    bitmap.pixels.resize(static_cast<size_t>(pixelCount) * 4);
    const auto* palette = reinterpret_cast<const uint32_t*>(rect.data[1]);
    for (int y = 0; y < rect.h; ++y) {
        const uint8_t* source = rect.data[0] + static_cast<size_t>(y) * rect.linesize[0];
        uint8_t* destination = bitmap.pixels.data() +
            static_cast<size_t>(y) * bitmap.strideBytes;
        for (int x = 0; x < rect.w; ++x) {
            const uint8_t paletteIndex = source[x];
            if (rect.nb_colors > 0 && paletteIndex >= rect.nb_colors) {
                error = "subtitle bitmap palette index is out of range";
                return false;
            }
            const uint32_t color = palette[paletteIndex];
            std::memcpy(destination + static_cast<size_t>(x) * 4, &color, sizeof(color));
        }
    }
    return true;
}

} // namespace

struct SubtitleDecoder::Impl {
    AVCodecContext* codec{nullptr};
    int fallbackCanvasWidth{0};
    int fallbackCanvasHeight{0};
    bool bitmapCodec{false};

    ~Impl() { avcodec_free_context(&codec); }
};

SubtitleDecoder::SubtitleDecoder() : m_impl(std::make_unique<Impl>()) {}
SubtitleDecoder::~SubtitleDecoder() = default;

bool SubtitleDecoder::open(const AVCodecParameters* parameters, int timeBaseNumerator,
                           int timeBaseDenominator, int fallbackCanvasWidth,
                           int fallbackCanvasHeight, std::string& error) {
    close();
    if (!parameters || timeBaseNumerator <= 0 || timeBaseDenominator <= 0) {
        error = "subtitle stream parameters are invalid";
        return false;
    }
    if (parameters->codec_id != AV_CODEC_ID_SUBRIP &&
        parameters->codec_id != AV_CODEC_ID_HDMV_PGS_SUBTITLE) {
        error = "subtitle codec is unsupported";
        return false;
    }
    const AVCodec* decoder = avcodec_find_decoder(parameters->codec_id);
    if (!decoder) {
        error = "subtitle decoder is unavailable";
        return false;
    }
    m_impl->codec = avcodec_alloc_context3(decoder);
    if (!m_impl->codec) {
        error = "avcodec_alloc_context3(subtitle) failed";
        return false;
    }
    int result = avcodec_parameters_to_context(m_impl->codec, parameters);
    m_impl->codec->pkt_timebase = AVRational{timeBaseNumerator, timeBaseDenominator};
    if (result >= 0) result = avcodec_open2(m_impl->codec, decoder, nullptr);
    if (result < 0) {
        error = ffmpegError("avcodec_open2(subtitle)", result);
        close();
        return false;
    }
    m_impl->fallbackCanvasWidth = fallbackCanvasWidth;
    m_impl->fallbackCanvasHeight = fallbackCanvasHeight;
    m_impl->bitmapCodec = parameters->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE;
    error.clear();
    return true;
}

bool SubtitleDecoder::decode(const AVPacket* packet, SubtitleDecodeOutput& output,
                             std::string& error) {
    output = {};
    if (!m_impl->codec || !packet) {
        error = "subtitle decoder is not open";
        return false;
    }
    AVSubtitle subtitle{};
    int gotSubtitle = 0;
    const int result = avcodec_decode_subtitle2(
        m_impl->codec, &subtitle, &gotSubtitle, packet);
    if (result < 0) {
        error = ffmpegError("avcodec_decode_subtitle2", result);
        return false;
    }
    if (!gotSubtitle) {
        error.clear();
        return true;
    }

    struct SubtitleGuard {
        AVSubtitle* value;
        ~SubtitleGuard() { avsubtitle_free(value); }
    } guard{&subtitle};

    int64_t baseNs = subtitle.pts;
    if (baseNs == AV_NOPTS_VALUE && packet->pts != AV_NOPTS_VALUE) {
        baseNs = av_rescale_q(packet->pts, m_impl->codec->pkt_timebase,
                              AVRational{1, static_cast<int>(kNanosecondsPerSecond)});
    } else if (baseNs != AV_NOPTS_VALUE) {
        baseNs = av_rescale_q(baseNs, AV_TIME_BASE_Q,
                              AVRational{1, static_cast<int>(kNanosecondsPerSecond)});
    }
    if (baseNs == AV_NOPTS_VALUE) {
        error = "decoded subtitle is missing a timestamp";
        return false;
    }
    const int64_t startNs = saturatingAdd(
        baseNs, static_cast<int64_t>(subtitle.start_display_time) * 1'000'000);
    std::optional<int64_t> endNs;
    if (subtitle.end_display_time != (std::numeric_limits<uint32_t>::max)() &&
        subtitle.end_display_time > subtitle.start_display_time) {
        endNs = saturatingAdd(
            baseNs, static_cast<int64_t>(subtitle.end_display_time) * 1'000'000);
    }
    if (m_impl->bitmapCodec) output.clearAtNanoseconds = startNs;
    if (subtitle.num_rects == 0) {
        output.clearAtNanoseconds = startNs;
        error.clear();
        return true;
    }

    SubtitleCue cue;
    cue.startNanoseconds = startNs;
    cue.endNanoseconds = endNs;
    cue.sourceWidth = m_impl->codec->width > 0
        ? m_impl->codec->width : m_impl->fallbackCanvasWidth;
    cue.sourceHeight = m_impl->codec->height > 0
        ? m_impl->codec->height : m_impl->fallbackCanvasHeight;
    for (unsigned i = 0; i < subtitle.num_rects; ++i) {
        const AVSubtitleRect* rect = subtitle.rects[i];
        if (!rect) continue;
        if (rect->type == SUBTITLE_BITMAP) {
            SubtitleBitmap bitmap;
            if (!makeBitmap(*rect, bitmap, error)) return false;
            if (cue.sourceWidth > 0 && cue.sourceHeight > 0 &&
                (bitmap.x > cue.sourceWidth - bitmap.width ||
                 bitmap.y > cue.sourceHeight - bitmap.height)) {
                error = "subtitle bitmap lies outside its source canvas";
                return false;
            }
            cue.bitmaps.push_back(std::move(bitmap));
            continue;
        }
        const char* text = rect->type == SUBTITLE_ASS ? rect->ass : rect->text;
        if (!text) continue;
        const std::string converted = plainText(text, rect->type == SUBTITLE_ASS);
        if (!converted.empty()) {
            if (!cue.textUtf8.empty()) cue.textUtf8.push_back('\n');
            cue.textUtf8 += converted;
        }
    }
    if (!cue.textUtf8.empty() || !cue.bitmaps.empty())
        output.cues.push_back(std::move(cue));
    error.clear();
    return true;
}

void SubtitleDecoder::flush() {
    if (m_impl->codec) avcodec_flush_buffers(m_impl->codec);
}

void SubtitleDecoder::close() {
    avcodec_free_context(&m_impl->codec);
    m_impl->fallbackCanvasWidth = 0;
    m_impl->fallbackCanvasHeight = 0;
    m_impl->bitmapCodec = false;
}

bool SubtitleDecoder::parseSrt(std::string_view utf8,
                               std::vector<SubtitleCue>& cues,
                               std::string& error) {
    cues.clear();
    if (utf8.size() > kMaximumSrtBytes || !validUtf8(utf8)) {
        error = utf8.size() > kMaximumSrtBytes
            ? "SRT file exceeds the 16 MiB limit" : "SRT file is not valid UTF-8";
        return false;
    }
    if (utf8.size() >= 3 && static_cast<uint8_t>(utf8[0]) == 0xEF &&
        static_cast<uint8_t>(utf8[1]) == 0xBB && static_cast<uint8_t>(utf8[2]) == 0xBF) {
        utf8.remove_prefix(3);
    }

    std::vector<std::string_view> lines;
    size_t position = 0;
    while (position <= utf8.size()) {
        const size_t newline = utf8.find('\n', position);
        const size_t end = newline == std::string_view::npos ? utf8.size() : newline;
        std::string_view line = utf8.substr(position, end - position);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        if (newline == std::string_view::npos) break;
        position = newline + 1;
    }

    size_t lineIndex = 0;
    while (lineIndex < lines.size()) {
        while (lineIndex < lines.size() && trim(lines[lineIndex]).empty()) ++lineIndex;
        if (lineIndex == lines.size()) break;
        size_t timingLine = lineIndex;
        if (lines[timingLine].find("-->") == std::string_view::npos) ++timingLine;
        if (timingLine >= lines.size()) {
            error = "SRT cue is missing a timing line";
            cues.clear();
            return false;
        }
        const std::string_view timing = lines[timingLine];
        const size_t arrow = timing.find("-->");
        int64_t startNs = 0;
        int64_t endNs = 0;
        if (arrow == std::string_view::npos ||
            !parseSrtTimestamp(timing.substr(0, arrow), startNs) ||
            !parseSrtTimestamp(timing.substr(arrow + 3), endNs) || endNs <= startNs) {
            error = "SRT cue has an invalid time range";
            cues.clear();
            return false;
        }
        lineIndex = timingLine + 1;
        std::string text;
        while (lineIndex < lines.size() && !trim(lines[lineIndex]).empty()) {
            if (!text.empty()) text.push_back('\n');
            text += plainText(lines[lineIndex], false);
            if (text.size() > kMaximumCueTextBytes) {
                error = "SRT cue text exceeds the 64 KiB limit";
                cues.clear();
                return false;
            }
            ++lineIndex;
        }
        if (text.empty()) {
            error = "SRT cue has no text";
            cues.clear();
            return false;
        }
        SubtitleCue cue;
        cue.startNanoseconds = startNs;
        cue.endNanoseconds = endNs;
        cue.textUtf8 = std::move(text);
        cues.push_back(std::move(cue));
        if (cues.size() > kMaximumSrtCues) {
            error = "SRT file exceeds the cue limit";
            cues.clear();
            return false;
        }
    }
    if (cues.empty()) {
        error = "SRT file contains no cues";
        return false;
    }
    std::stable_sort(cues.begin(), cues.end(), [](const SubtitleCue& left,
                                                  const SubtitleCue& right) {
        return left.startNanoseconds < right.startNanoseconds;
    });
    error.clear();
    return true;
}

bool SubtitleDecoder::parseSrtFile(const std::wstring& path,
                                   std::vector<SubtitleCue>& cues,
                                   std::string& error) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!input) {
        error = "unable to open external SRT file";
        return false;
    }
    const std::streamoff size = input.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > kMaximumSrtBytes) {
        error = "SRT file exceeds the 16 MiB limit";
        return false;
    }
    std::string bytes(static_cast<size_t>(size), '\0');
    input.seekg(0);
    if (!bytes.empty() && !input.read(bytes.data(), size)) {
        error = "unable to read external SRT file";
        return false;
    }
    return parseSrt(bytes, cues, error);
}

std::vector<SubtitleCue> SubtitleDecoder::activeCues(
    const std::vector<SubtitleCue>& cues, int64_t mediaNanoseconds,
    int64_t offsetNanoseconds) {
    std::vector<SubtitleCue> active;
    for (const SubtitleCue& source : cues) {
        const int64_t start = saturatingAdd(source.startNanoseconds, offsetNanoseconds);
        const std::optional<int64_t> end = source.endNanoseconds
            ? std::optional<int64_t>(saturatingAdd(*source.endNanoseconds, offsetNanoseconds))
            : std::nullopt;
        if (mediaNanoseconds < start || (end && mediaNanoseconds >= *end)) continue;
        SubtitleCue adjusted = source;
        adjusted.startNanoseconds = start;
        adjusted.endNanoseconds = end;
        active.push_back(std::move(adjusted));
    }
    return active;
}

} // namespace odyssey
