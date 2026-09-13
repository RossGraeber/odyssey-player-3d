#pragma once

#include "SubtitleCue.h"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace odyssey {

struct RenderedSubtitle {
    std::vector<std::uint8_t> bgra;
    UINT width{0};
    UINT height{0};
    RECT destination{};
};

class SubtitleRenderer {
public:
    std::vector<RenderedSubtitle> render(
        const std::vector<SubtitleCue>& activeCues,
        const RECT& fittedMovieRect,
        UINT canvasWidth,
        UINT canvasHeight,
        std::optional<int> textSafeBottom = std::nullopt) const;
};

} // namespace odyssey
