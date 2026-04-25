#pragma once

#include <cstdint>

namespace odyssey::pts {

// Converts an FFmpeg PTS expressed in stream timebase (num/den, seconds) to
// nanoseconds. Integer math only: no fp rounding, no overflow for any
// timebase FFmpeg emits on typical content (1/90000 for TS, 1/1000 for MKV,
// 1001/30000 for NTSC). Worst case: pts up to ~10h at 1/90000 is 3.24e9;
// * 1e9 * num fits in int64.
constexpr int64_t toNs(int64_t pts, int num, int den) {
    // pts * num / den seconds, then × 1e9 ns/s — multiply first to keep
    // precision, divide last.
    return pts * 1'000'000'000LL * num / den;
}

// Inverse: nanoseconds back to stream-timebase PTS. Truncates toward zero
// (matches integer division semantics); callers needing frame-accurate round
// trips should supply ns values that are exact multiples of the tick length.
constexpr int64_t fromNs(int64_t ns, int num, int den) {
    return ns * den / (1'000'000'000LL * num);
}

} // namespace odyssey::pts
