#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace odyssey {

struct SubtitleBitmap {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
    int strideBytes{0};
    // BGRA pixels use straight alpha so the renderer owns premultiplication.
    std::vector<uint8_t> pixels;
};

struct SubtitleCue {
    int64_t startNanoseconds{0};
    std::optional<int64_t> endNanoseconds;
    std::string textUtf8;
    int sourceWidth{0};
    int sourceHeight{0};
    std::vector<SubtitleBitmap> bitmaps;
};

} // namespace odyssey
