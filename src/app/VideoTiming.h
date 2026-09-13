#pragma once

#include <cstdint>
#include <optional>

namespace odyssey {

enum class VideoTimingDecision {
    Wait,
    Present,
    Drop,
};

constexpr VideoTimingDecision decideVideoTiming(
    int64_t framePtsNs,
    std::optional<int64_t> frameDurationNs,
    int64_t mediaClockNs) noexcept {
    if (mediaClockNs < framePtsNs) {
        return VideoTimingDecision::Wait;
    }

    if (!frameDurationNs || *frameDurationNs <= 0) {
        return VideoTimingDecision::Present;
    }

    const auto elapsedNs = static_cast<uint64_t>(mediaClockNs)
        - static_cast<uint64_t>(framePtsNs);
    return elapsedNs >= static_cast<uint64_t>(*frameDurationNs)
        ? VideoTimingDecision::Drop
        : VideoTimingDecision::Present;
}

} // namespace odyssey
