#include "PlayerControls.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace odyssey {
namespace {

constexpr int logicalWidth = 960;
constexpr int logicalHeight = 120;
constexpr std::uint64_t hideDelayMilliseconds = 2500;

struct Rect {
    int left;
    int top;
    int right;
    int bottom;

    bool contains(int x, int y) const noexcept {
        return x >= left && x < right && y >= top && y < bottom;
    }
};

struct Geometry {
    int scale;
    Rect panel;
    Rect seek;
    Rect volume;
};

struct GdiResources {
    HDC dc{nullptr};
    HBITMAP bitmap{nullptr};
    HGDIOBJ oldBitmap{nullptr};
    HFONT font{nullptr};
    HGDIOBJ oldFont{nullptr};

    ~GdiResources() {
        if (oldFont) SelectObject(dc, oldFont);
        if (font) DeleteObject(font);
        if (oldBitmap) SelectObject(dc, oldBitmap);
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
    }
};

bool validClient(int clientWidth, int clientHeight) noexcept {
    return clientWidth >= PlayerControls::minimumClientWidth &&
           clientHeight >= PlayerControls::minimumClientHeight;
}

int integerScale(int clientWidth, int clientHeight) noexcept {
    if (clientWidth <= 0 || clientHeight <= 0) return 1;
    return (std::max)(1, (std::min)(3, (std::min)(clientWidth / logicalWidth,
                                                   clientHeight / 540)));
}

Geometry geometryFor(int clientWidth, int clientHeight) noexcept {
    const int scale = integerScale(clientWidth, clientHeight);
    const int width = (std::min)(clientWidth, logicalWidth * scale);
    const int height = (std::min)(clientHeight, logicalHeight * scale);
    const int left = (clientWidth - width) / 2;
    const int top = clientHeight - height;
    const auto scaled = [=](int l, int t, int r, int b) {
        return Rect{left + l * scale, top + t * scale,
                    left + r * scale, top + b * scale};
    };
    return {scale, {left, top, left + width, top + height},
            scaled(100, 52, 868, 68), scaled(700, 78, 940, 98)};
}

Rect controlRect(PlayerCommand command, const Geometry& geometry) noexcept {
    const int s = geometry.scale;
    const int l = geometry.panel.left;
    const int t = geometry.panel.top;
    const auto rect = [=](int left, int top, int right, int bottom) {
        return Rect{l + left * s, t + top * s, l + right * s, t + bottom * s};
    };
    switch (command) {
    case PlayerCommand::Open: return rect(12, 78, 72, 98);
    case PlayerCommand::PlayPause: return rect(78, 78, 190, 98);
    case PlayerCommand::Stop: return rect(196, 78, 258, 98);
    case PlayerCommand::ToggleMute: return rect(570, 78, 650, 98);
    case PlayerCommand::AudioMenu: return rect(12, 101, 190, 117);
    case PlayerCommand::CaptionsMenu: return rect(196, 101, 390, 117);
    case PlayerCommand::LayoutMenu: return rect(396, 101, 520, 117);
    case PlayerCommand::SwapEyes: return rect(526, 101, 640, 117);
    case PlayerCommand::IsoTitleMenu: return rect(650, 101, 780, 117);
    case PlayerCommand::Minimize: return rect(758, 4, 814, 22);
    case PlayerCommand::Fullscreen: return rect(820, 4, 892, 22);
    case PlayerCommand::Close: return rect(900, 4, 948, 22);
    default: return {};
    }
}

PlayerCommand hitButton(int x, int y, const Geometry& geometry,
                        const PlayerControlState& state) noexcept {
    constexpr PlayerCommand commands[] = {
        PlayerCommand::Open, PlayerCommand::PlayPause, PlayerCommand::Stop,
        PlayerCommand::ToggleMute, PlayerCommand::AudioMenu, PlayerCommand::CaptionsMenu,
        PlayerCommand::LayoutMenu, PlayerCommand::SwapEyes, PlayerCommand::Minimize,
        PlayerCommand::Fullscreen, PlayerCommand::Close,
    };
    for (const PlayerCommand command : commands) {
        if ((command == PlayerCommand::AudioMenu && !state.audioMenuEnabled) ||
            (command == PlayerCommand::CaptionsMenu && !state.captionsMenuEnabled)) {
            continue;
        }
        if (controlRect(command, geometry).contains(x, y)) return command;
    }
    if (state.isIso && state.isoTitleMenuEnabled &&
        controlRect(PlayerCommand::IsoTitleMenu, geometry).contains(x, y)) {
        return PlayerCommand::IsoTitleMenu;
    }
    return PlayerCommand::None;
}

double fractionAt(int x, const Rect& track) noexcept {
    if (track.right <= track.left) return 0.0;
    return (std::clamp)(static_cast<double>(x - track.left) /
                            static_cast<double>(track.right - track.left),
                        0.0, 1.0);
}

double safeSeconds(double seconds) noexcept {
    constexpr double maximumDisplayedSeconds = 3599999.0;
    if (!std::isfinite(seconds) || seconds <= 0.0) return 0.0;
    return (std::min)(seconds, maximumDisplayedSeconds);
}

double safeUnitValue(double value) noexcept {
    if (!std::isfinite(value)) return 0.0;
    return (std::clamp)(value, 0.0, 1.0);
}

std::wstring timeText(double seconds) {
    seconds = safeSeconds(seconds);
    const auto total = static_cast<unsigned long long>(seconds);
    const auto hours = total / 3600;
    const auto minutes = (total / 60) % 60;
    const auto remaining = total % 60;
    wchar_t text[32]{};
    swprintf_s(text, L"%02llu:%02llu:%02llu", hours, minutes, remaining);
    return text;
}

COLORREF color(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return RGB(r, g, b);
}

void fill(HDC dc, const Rect& rect, COLORREF value) {
    RECT native{rect.left, rect.top, rect.right, rect.bottom};
    SetDCBrushColor(dc, value);
    FillRect(dc, &native, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void frame(HDC dc, const Rect& rect, COLORREF upper, COLORREF lower) {
    fill(dc, {rect.left, rect.top, rect.right, rect.top + 1}, upper);
    fill(dc, {rect.left, rect.top, rect.left + 1, rect.bottom}, upper);
    fill(dc, {rect.left, rect.bottom - 1, rect.right, rect.bottom}, lower);
    fill(dc, {rect.right - 1, rect.top, rect.right, rect.bottom}, lower);
}

void text(HDC dc, const std::wstring& value, const Rect& rect,
          COLORREF valueColor, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    RECT native{rect.left, rect.top, rect.right, rect.bottom};
    SetTextColor(dc, valueColor);
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &native, flags);
}

} // namespace

PlayerControlSurface PlayerControls::render(const PlayerControlState& state,
                                            int clientWidth,
                                            int clientHeight,
                                            std::uint64_t nowMilliseconds) const {
    PlayerControlSurface surface;
    if (!validClient(clientWidth, clientHeight) || !visible(state, nowMilliseconds)) {
        return surface;
    }

    const Geometry geometry = geometryFor(clientWidth, clientHeight);
    surface.left = geometry.panel.left;
    surface.top = geometry.panel.top;
    surface.width = geometry.panel.right - geometry.panel.left;
    surface.height = geometry.panel.bottom - geometry.panel.top;
    surface.scale = geometry.scale;
    surface.visible = true;

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = surface.width;
    bitmapInfo.bmiHeader.biHeight = -surface.height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    GdiResources resources;
    resources.bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS,
                                        &bits, nullptr, 0);
    resources.dc = resources.bitmap ? CreateCompatibleDC(nullptr) : nullptr;
    if (!resources.bitmap || !resources.dc || !bits) {
        return {};
    }
    resources.oldBitmap = SelectObject(resources.dc, resources.bitmap);
    resources.font = CreateFontW(-9 * surface.scale, 0, 0, 0, FW_BOLD,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 NONANTIALIASED_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Segoe UI");
    resources.oldFont = resources.font ? SelectObject(resources.dc, resources.font) : nullptr;
    HDC dc = resources.dc;
    SetBkMode(dc, TRANSPARENT);

    const auto local = [&](const Rect& rect) {
        return Rect{rect.left - surface.left, rect.top - surface.top,
                    rect.right - surface.left, rect.bottom - surface.top};
    };
    const auto logical = [&](int left, int top, int right, int bottom) {
        return Rect{left * surface.scale, top * surface.scale,
                    right * surface.scale, bottom * surface.scale};
    };

    const COLORREF charcoal = color(28, 30, 29);
    const COLORREF panel = color(47, 50, 48);
    const COLORREF raised = color(67, 71, 68);
    const COLORREF light = color(121, 128, 122);
    const COLORREF shadow = color(13, 15, 14);
    const COLORREF amber = color(255, 184, 62);
    const COLORREF green = color(91, 221, 105);
    const COLORREF normalText = color(224, 226, 220);
    const COLORREF dimText = color(139, 143, 138);

    fill(dc, logical(0, 0, logicalWidth, logicalHeight), charcoal);
    fill(dc, logical(3, 3, logicalWidth - 3, logicalHeight - 3), panel);
    frame(dc, logical(1, 1, logicalWidth - 1, logicalHeight - 1), light, shadow);
    text(dc, L"ODYSSEY 3D", logical(12, 4, 240, 22), amber);

    const auto drawButton = [&](PlayerCommand command, const std::wstring& label, bool enabled = true) {
        const Rect absolute = controlRect(command, geometry);
        const Rect rect = local(absolute);
        const bool hovered = absolute.contains(pointerX_, pointerY_);
        const bool pressed = pressed_ == command;
        fill(dc, rect, pressed ? charcoal : (hovered ? color(79, 84, 80) : raised));
        frame(dc, rect, pressed ? shadow : light, pressed ? light : shadow);
        text(dc, label, {rect.left + 4 * surface.scale, rect.top,
                         rect.right - 4 * surface.scale, rect.bottom},
             enabled ? normalText : dimText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    };

    drawButton(PlayerCommand::Minimize, L"MIN");
    drawButton(PlayerCommand::Fullscreen, L"FULL");
    drawButton(PlayerCommand::Close, L"\u00d7");

    if (!state.error.empty()) {
        text(dc, state.error, logical(12, 24, 948, 50), amber,
             DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    } else {
        text(dc, state.filename.empty() ? L"Open a movie" : state.filename,
             logical(12, 26, 720, 46), normalText);
        text(dc, state.status.empty() ? L"Ready" : state.status,
             logical(726, 26, 948, 46), green,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    const double duration = safeSeconds(state.durationSeconds);
    const double previewPosition = drag_ == Drag::Seek
        ? dragValue_ * duration : safeSeconds(state.positionSeconds);
    text(dc, timeText(previewPosition), logical(12, 50, 94, 70), amber);
    text(dc, timeText(duration), logical(874, 50, 948, 70), amber,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    const Rect seek = local(geometry.seek);
    fill(dc, seek, shadow);
    const double seekFraction = duration > 0.0
        ? safeUnitValue(previewPosition / duration) : 0.0;
    fill(dc, {seek.left + 2 * surface.scale, seek.top + 5 * surface.scale,
              seek.left + static_cast<int>((seek.right - seek.left) * seekFraction),
              seek.bottom - 5 * surface.scale}, amber);

    drawButton(PlayerCommand::Open, L"OPEN");
    drawButton(PlayerCommand::PlayPause, state.playing ? L"PAUSE" : L"PLAY");
    drawButton(PlayerCommand::Stop, L"STOP");
    drawButton(PlayerCommand::ToggleMute, state.muted ? L"UNMUTE" : L"MUTE");

    const Rect volume = local(geometry.volume);
    text(dc, L"VOL", logical(656, 78, 696, 98), dimText,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    fill(dc, volume, shadow);
    const double volumeFraction = safeUnitValue(state.volume);
    fill(dc, {volume.left + 2 * surface.scale, volume.top + 5 * surface.scale,
              volume.left + static_cast<int>((volume.right - volume.left) * volumeFraction),
              volume.bottom - 5 * surface.scale}, green);

    drawButton(PlayerCommand::AudioMenu,
               L"AUDIO: " + (state.audioLabel.empty() ? std::wstring(L"Default") : state.audioLabel),
               state.audioMenuEnabled);
    drawButton(PlayerCommand::CaptionsMenu,
               L"CAPTIONS: " + (state.captionsLabel.empty() ? std::wstring(L"Off") : state.captionsLabel),
               state.captionsMenuEnabled);
    drawButton(PlayerCommand::LayoutMenu,
               state.layoutLabel.empty() ? L"LAYOUT" : state.layoutLabel);
    drawButton(PlayerCommand::SwapEyes, L"SWAP EYES");
    if (state.isIso) {
        drawButton(PlayerCommand::IsoTitleMenu, L"ISO TITLE", state.isoTitleMenuEnabled);
    }

    GdiFlush();
    const size_t byteCount = static_cast<size_t>(surface.width) * surface.height * 4;
    surface.bgra.assign(static_cast<const std::uint8_t*>(bits),
                        static_cast<const std::uint8_t*>(bits) + byteCount);
    for (size_t index = 3; index < surface.bgra.size(); index += 4) {
        surface.bgra[index] = 255;
    }

    return surface;
}

PlayerAction PlayerControls::pointerMove(int x, int y, int clientWidth, int clientHeight,
                                         const PlayerControlState& state,
                                         std::uint64_t nowMilliseconds) {
    if (!validClient(clientWidth, clientHeight)) {
        cancelInteraction();
        pointerInside_ = false;
        lastMotionMilliseconds_ = nowMilliseconds;
        return {};
    }
    const Geometry geometry = geometryFor(clientWidth, clientHeight);
    pointerX_ = x;
    pointerY_ = y;
    pointerInside_ = geometry.panel.contains(x, y);
    lastMotionMilliseconds_ = nowMilliseconds;

    if (drag_ == Drag::Seek) {
        dragValue_ = fractionAt(x, geometry.seek);
    } else if (drag_ == Drag::Volume) {
        dragValue_ = fractionAt(x, geometry.volume);
        return {PlayerCommand::SetVolume, dragValue_};
    }
    (void)state;
    return {};
}

PlayerAction PlayerControls::pointerDown(int x, int y, int clientWidth, int clientHeight,
                                         const PlayerControlState& state,
                                         std::uint64_t nowMilliseconds) {
    if (!validClient(clientWidth, clientHeight)) {
        cancelInteraction();
        pointerInside_ = false;
        lastMotionMilliseconds_ = nowMilliseconds;
        return {};
    }
    pointerMove(x, y, clientWidth, clientHeight, state, nowMilliseconds);
    const Geometry geometry = geometryFor(clientWidth, clientHeight);
    if (geometry.seek.contains(x, y) && safeSeconds(state.durationSeconds) > 0.0) {
        drag_ = Drag::Seek;
        dragValue_ = fractionAt(x, geometry.seek);
        return {};
    }
    if (geometry.volume.contains(x, y)) {
        drag_ = Drag::Volume;
        dragValue_ = fractionAt(x, geometry.volume);
        return {PlayerCommand::SetVolume, dragValue_};
    }

    pressed_ = hitButton(x, y, geometry, state);
    return {};
}

PlayerAction PlayerControls::pointerUp(int x, int y, int clientWidth, int clientHeight,
                                       const PlayerControlState& state,
                                       std::uint64_t nowMilliseconds) {
    if (!validClient(clientWidth, clientHeight)) {
        cancelInteraction();
        pointerInside_ = false;
        lastMotionMilliseconds_ = nowMilliseconds;
        return {};
    }
    const Geometry geometry = geometryFor(clientWidth, clientHeight);
    pointerX_ = x;
    pointerY_ = y;
    pointerInside_ = geometry.panel.contains(x, y);
    lastMotionMilliseconds_ = nowMilliseconds;

    PlayerAction action;
    if (drag_ == Drag::Seek) {
        dragValue_ = fractionAt(x, geometry.seek);
        action = {PlayerCommand::Seek, dragValue_ * safeSeconds(state.durationSeconds)};
    } else if (drag_ == Drag::Volume) {
        dragValue_ = fractionAt(x, geometry.volume);
        action = {PlayerCommand::SetVolume, dragValue_};
    } else if (pressed_ != PlayerCommand::None &&
               pressed_ == hitButton(x, y, geometry, state)) {
        action = {pressed_, 0.0};
    }
    drag_ = Drag::None;
    pressed_ = PlayerCommand::None;
    return action;
}

void PlayerControls::pointerLeave(std::uint64_t nowMilliseconds) noexcept {
    pointerInside_ = false;
    lastMotionMilliseconds_ = nowMilliseconds;
}

void PlayerControls::cancelInteraction() noexcept {
    drag_ = Drag::None;
    pressed_ = PlayerCommand::None;
}

bool PlayerControls::captureNeeded() const noexcept {
    return drag_ != Drag::None || pressed_ != PlayerCommand::None;
}

bool PlayerControls::visible(const PlayerControlState& state,
                             std::uint64_t nowMilliseconds) const noexcept {
    if (!state.playing || state.forceVisible || pointerInside_ || state.menuOpen || drag_ != Drag::None ||
        pressed_ != PlayerCommand::None ||
        !state.error.empty()) {
        return true;
    }
    return nowMilliseconds < lastMotionMilliseconds_ ||
           nowMilliseconds - lastMotionMilliseconds_ < hideDelayMilliseconds;
}

double PlayerControls::seekPreviewSeconds(const PlayerControlState& state) const noexcept {
    if (drag_ != Drag::Seek) return safeSeconds(state.positionSeconds);
    return dragValue_ * safeSeconds(state.durationSeconds);
}

} // namespace odyssey
