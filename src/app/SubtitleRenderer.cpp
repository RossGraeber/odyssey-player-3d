#include "SubtitleRenderer.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace odyssey {
namespace {

class MemoryDc {
public:
    MemoryDc() : m_dc(CreateCompatibleDC(nullptr)) {
        if (!m_dc) {
            throw std::runtime_error("CreateCompatibleDC failed for subtitle text");
        }
    }

    ~MemoryDc() {
        DeleteDC(m_dc);
    }

    HDC get() const noexcept { return m_dc; }

private:
    HDC m_dc{nullptr};
};

struct GdiObjectDeleter {
    void operator()(void* object) const noexcept {
        DeleteObject(object);
    }
};

using UniqueGdiObject = std::unique_ptr<void, GdiObjectDeleter>;

class SelectedObject {
public:
    SelectedObject(HDC dc, HGDIOBJ object) : m_dc(dc), m_previous(SelectObject(dc, object)) {
        if (!m_previous || m_previous == HGDI_ERROR) {
            throw std::runtime_error("SelectObject failed for subtitle rendering");
        }
    }

    ~SelectedObject() {
        SelectObject(m_dc, m_previous);
    }

private:
    HDC m_dc{nullptr};
    HGDIOBJ m_previous{nullptr};
};

void validateCanvas(const RECT& movieRect, UINT canvasWidth, UINT canvasHeight) {
    if (canvasWidth == 0 || canvasHeight == 0
        || movieRect.left < 0 || movieRect.top < 0
        || movieRect.right <= movieRect.left || movieRect.bottom <= movieRect.top
        || static_cast<std::uint64_t>(movieRect.right) > canvasWidth
        || static_cast<std::uint64_t>(movieRect.bottom) > canvasHeight) {
        throw std::invalid_argument("Subtitle canvas geometry is invalid");
    }
}

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::invalid_argument("Subtitle text is too large");
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0);
    if (length <= 0) {
        throw std::invalid_argument("Subtitle text is not valid UTF-8");
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
            wide.data(), length) != length) {
        throw std::runtime_error("Subtitle UTF-8 conversion failed");
    }
    return wide;
}

void validateBitmap(const SubtitleCue& cue, const SubtitleBitmap& bitmap) {
    const std::int64_t right = static_cast<std::int64_t>(bitmap.x) + bitmap.width;
    const std::int64_t bottom = static_cast<std::int64_t>(bitmap.y) + bitmap.height;
    const std::int64_t rowBytes = static_cast<std::int64_t>(bitmap.width) * 4;
    if (cue.sourceWidth <= 0 || cue.sourceHeight <= 0
        || bitmap.x < 0 || bitmap.y < 0 || bitmap.width <= 0 || bitmap.height <= 0
        || right > cue.sourceWidth || bottom > cue.sourceHeight
        || bitmap.strideBytes < rowBytes) {
        throw std::invalid_argument("Subtitle bitmap geometry is invalid");
    }
    const std::uint64_t required = static_cast<std::uint64_t>(bitmap.strideBytes)
            * static_cast<std::uint64_t>(bitmap.height - 1)
        + static_cast<std::uint64_t>(rowBytes);
    if (required > bitmap.pixels.size()) {
        throw std::invalid_argument("Subtitle bitmap storage is incomplete");
    }
}

LONG mapCoordinate(LONG movieStart, LONG movieExtent, int source, int sourceExtent) {
    const std::int64_t scaled = static_cast<std::int64_t>(source) * movieExtent;
    return movieStart + static_cast<LONG>(
        (scaled + sourceExtent / 2) / sourceExtent);
}

RenderedSubtitle renderBitmap(
    const SubtitleCue& cue, const SubtitleBitmap& bitmap, const RECT& movieRect) {
    validateBitmap(cue, bitmap);
    RenderedSubtitle result;
    result.width = static_cast<UINT>(bitmap.width);
    result.height = static_cast<UINT>(bitmap.height);
    result.bgra.resize(
        static_cast<std::size_t>(result.width) * result.height * 4);
    const std::size_t rowBytes = static_cast<std::size_t>(result.width) * 4;
    for (UINT row = 0; row < result.height; ++row) {
        std::copy_n(
            bitmap.pixels.data() + static_cast<std::size_t>(row) * bitmap.strideBytes,
            rowBytes,
            result.bgra.data() + static_cast<std::size_t>(row) * rowBytes);
    }

    const LONG movieWidth = movieRect.right - movieRect.left;
    const LONG movieHeight = movieRect.bottom - movieRect.top;
    result.destination = {
        mapCoordinate(movieRect.left, movieWidth, bitmap.x, cue.sourceWidth),
        mapCoordinate(movieRect.top, movieHeight, bitmap.y, cue.sourceHeight),
        mapCoordinate(
            movieRect.left, movieWidth, bitmap.x + bitmap.width, cue.sourceWidth),
        mapCoordinate(
            movieRect.top, movieHeight, bitmap.y + bitmap.height, cue.sourceHeight),
    };
    if (result.destination.right <= result.destination.left
        || result.destination.bottom <= result.destination.top) {
        throw std::invalid_argument("Subtitle bitmap collapses in the fitted movie rectangle");
    }
    return result;
}

RenderedSubtitle renderText(const std::wstring& text, const RECT& movieRect,
                            std::optional<int> textSafeBottom) {
    const int movieWidth = movieRect.right - movieRect.left;
    const int movieHeight = movieRect.bottom - movieRect.top;
    const int fontHeight = std::clamp(movieHeight / 18, 18, 72);
    const int padding = (std::max)(8, fontHeight / 3);
    const int maximumTextWidth = (std::max)(1, movieWidth * 4 / 5 - 2 * padding);

    MemoryDc dc;
    UniqueGdiObject font(CreateFontW(
        -fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));
    if (!font) {
        throw std::runtime_error("CreateFont failed for subtitle text");
    }
    SelectedObject selectedFont(dc.get(), font.get());

    RECT measured{0, 0, maximumTextWidth, 0};
    constexpr UINT flags = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX;
    if (DrawTextW(
            dc.get(), text.c_str(), static_cast<int>(text.size()), &measured,
            flags | DT_CALCRECT) <= 0) {
        throw std::runtime_error("DrawText failed to measure subtitle text");
    }

    const int width = measured.right - measured.left + 2 * padding;
    const int height = measured.bottom - measured.top + 2 * padding;
    const int bottomMargin = (std::max)(8, movieHeight / 20);
    const int safeBottom = std::clamp<int>(
        textSafeBottom.value_or(static_cast<int>(movieRect.bottom)),
        static_cast<int>(movieRect.top), static_cast<int>(movieRect.bottom));
    const int availableHeight = safeBottom - movieRect.top;
    if (width <= 0 || height <= 0 || width > movieWidth
        || height + bottomMargin > availableHeight) {
        throw std::invalid_argument("Subtitle text does not fit the movie rectangle");
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    UniqueGdiObject bitmap(CreateDIBSection(
        dc.get(), &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!bitmap || !bits) {
        throw std::runtime_error("CreateDIBSection failed for subtitle text");
    }
    SelectedObject selectedBitmap(dc.get(), bitmap.get());

    std::fill_n(
        static_cast<std::uint8_t*>(bits),
        static_cast<std::size_t>(width) * height * 4,
        std::uint8_t{0});
    SetTextColor(dc.get(), RGB(255, 255, 255));
    SetBkMode(dc.get(), TRANSPARENT);
    RECT textRect{padding, padding, width - padding, height - padding};
    if (DrawTextW(
            dc.get(), text.c_str(), static_cast<int>(text.size()), &textRect,
            flags) <= 0) {
        throw std::runtime_error("DrawText failed to render subtitle text");
    }
    if (!GdiFlush()) {
        throw std::runtime_error("GdiFlush failed for subtitle text");
    }

    RenderedSubtitle result;
    result.width = static_cast<UINT>(width);
    result.height = static_cast<UINT>(height);
    result.bgra.assign(
        static_cast<const std::uint8_t*>(bits),
        static_cast<const std::uint8_t*>(bits)
            + static_cast<std::size_t>(width) * height * 4);
    for (std::size_t alpha = 3; alpha < result.bgra.size(); alpha += 4) {
        result.bgra[alpha] = 255;
    }
    result.destination.left = movieRect.left + (movieWidth - width) / 2;
    result.destination.top = safeBottom - bottomMargin - height;
    result.destination.right = result.destination.left + width;
    result.destination.bottom = result.destination.top + height;

    return result;
}

} // namespace

std::vector<RenderedSubtitle> SubtitleRenderer::render(
    const std::vector<SubtitleCue>& activeCues,
    const RECT& fittedMovieRect,
    UINT canvasWidth,
    UINT canvasHeight,
    std::optional<int> textSafeBottom) const {
    validateCanvas(fittedMovieRect, canvasWidth, canvasHeight);

    std::vector<RenderedSubtitle> result;
    std::wstring text;
    for (const SubtitleCue& cue : activeCues) {
        for (const SubtitleBitmap& bitmap : cue.bitmaps) {
            result.push_back(renderBitmap(cue, bitmap, fittedMovieRect));
        }
        if (!cue.textUtf8.empty()) {
            if (!text.empty()) {
                text.push_back(L'\n');
            }
            text.append(utf8ToWide(cue.textUtf8));
            if (text.size()
                > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
                throw std::invalid_argument("Combined subtitle text is too large");
            }
        }
    }
    if (!text.empty()) {
        result.push_back(renderText(text, fittedMovieRect, textSafeBottom));
    }
    return result;
}

} // namespace odyssey
