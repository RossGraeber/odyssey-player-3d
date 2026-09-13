#pragma once

#include <cstdint>

namespace odyssey {

bool mvcSubtitleTimestamp100ns(int64_t segmentStart100ns,
                               int64_t sampleStart100ns,
                               double playbackRate,
                               int64_t& timestamp100ns);

} // namespace odyssey
