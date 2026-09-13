#pragma once

#include <optional>

namespace odyssey {

enum class StereoMode {
    FullSbs,
    HalfSbs,
    Mono2D,
};

struct NormalizedRect {
    double left;
    double top;
    double right;
    double bottom;
};

struct EyeLayout {
    NormalizedRect source;
    NormalizedRect destination;
};

struct StereoLayout {
    EyeLayout left;
    EyeLayout right;
};

inline std::optional<StereoLayout> makeStereoLayout(
    StereoMode mode,
    int sourceWidth,
    int sourceHeight,
    int sampleAspectNumerator,
    int sampleAspectDenominator,
    int displayWidth,
    int displayHeight,
    bool swapEyes) {
    if (sourceWidth <= 0 || sourceHeight <= 0
        || sampleAspectNumerator <= 0 || sampleAspectDenominator <= 0
        || displayWidth <= 0 || displayHeight <= 0) {
        return std::nullopt;
    }

    const NormalizedRect fullSource{0.0, 0.0, 1.0, 1.0};
    const NormalizedRect firstSource{0.0, 0.0, 0.5, 1.0};
    const NormalizedRect secondSource{0.5, 0.0, 1.0, 1.0};
    NormalizedRect leftSource = fullSource;
    NormalizedRect rightSource = fullSource;
    double contentAspect = 0.0;

    switch (mode) {
    case StereoMode::FullSbs:
    case StereoMode::HalfSbs:
        if ((sourceWidth % 2) != 0) {
            return std::nullopt;
        }
        leftSource = swapEyes ? secondSource : firstSource;
        rightSource = swapEyes ? firstSource : secondSource;
        contentAspect = static_cast<double>(sourceWidth) * sampleAspectNumerator
            / (sourceHeight * static_cast<double>(sampleAspectDenominator));
        if (mode == StereoMode::FullSbs) {
            contentAspect /= 2.0;
        }
        break;
    case StereoMode::Mono2D:
        contentAspect = static_cast<double>(sourceWidth) * sampleAspectNumerator
            / (sourceHeight * static_cast<double>(sampleAspectDenominator));
        break;
    default:
        return std::nullopt;
    }

    const double displayAspect = static_cast<double>(displayWidth) / displayHeight;
    NormalizedRect destination{0.0, 0.0, 1.0, 1.0};
    if (contentAspect > displayAspect) {
        const double height = displayAspect / contentAspect;
        destination.top = (1.0 - height) / 2.0;
        destination.bottom = destination.top + height;
    } else if (contentAspect < displayAspect) {
        const double width = contentAspect / displayAspect;
        destination.left = (1.0 - width) / 2.0;
        destination.right = destination.left + width;
    }

    // These are geometric crop edges. The renderer must clamp sample
    // coordinates half a source texel inside each eye instead of moving the seam.
    return StereoLayout{{leftSource, destination}, {rightSource, destination}};
}

} // namespace odyssey
