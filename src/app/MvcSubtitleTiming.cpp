#include "MvcSubtitleTiming.h"

#include <cmath>
#include <limits>

namespace odyssey {

bool mvcSubtitleTimestamp100ns(int64_t segmentStart100ns,
                               int64_t sampleStart100ns,
                               double playbackRate,
                               int64_t& timestamp100ns) {
    if (!std::isfinite(playbackRate) || playbackRate != 1.0 ||
        (sampleStart100ns > 0 &&
         segmentStart100ns > (std::numeric_limits<int64_t>::max)() - sampleStart100ns) ||
        (sampleStart100ns < 0 &&
         segmentStart100ns < (std::numeric_limits<int64_t>::min)() - sampleStart100ns)) {
        return false;
    }
    timestamp100ns = segmentStart100ns + sampleStart100ns;
    return true;
}

} // namespace odyssey
