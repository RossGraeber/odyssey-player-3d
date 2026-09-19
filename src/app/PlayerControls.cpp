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
    case PlayerCommand::AudioMenu: return rect(12, 100, 200, 117);
    case PlayerCommand::CaptionsMenu: return rect(206, 100, 410, 117);
    case PlayerCommand::LayoutMenu: return rect(416, 100, 560, 117);
    case PlayerCommand::SwapEyes: return rect(566, 100, 680, 117);
    case PlayerCommand::IsoTitleMenu: return rect(686, 100, 820, 117);
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

// Verdana chosen for dyslexia-friendly letterforms, non-antialiased at
// integer scale for a crisp pixel look.
HFONT pixelFont(int logicalHeightValue, int scale, bool bold) {
    return CreateFontW(-logicalHeightValue * scale, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                       VARIABLE_PITCH | FF_SWISS, L"Verdana");
}

std::wstring commandHint(PlayerCommand command, const PlayerControlState& state) {
    switch (command) {
    case PlayerCommand::Open: return L"Open a movie file  (O)";
    case PlayerCommand::PlayPause: return state.playing ? L"Pause  (Space)" : L"Play  (Space)";
    case PlayerCommand::Stop: return L"Stop and rewind to the start  (S)";
    case PlayerCommand::ToggleMute: return state.muted ? L"Unmute  (M)" : L"Mute  (M)";
    case PlayerCommand::AudioMenu:
        return state.audioMenuEnabled ? L"Choose the audio track  (A)" : L"No audio tracks available";
    case PlayerCommand::CaptionsMenu:
        return state.captionsMenuEnabled ? L"Choose captions and timing  (C)" : L"Captions not available yet";
    case PlayerCommand::LayoutMenu:
        return L"Choose the 3D layout: Auto, Full SBS, Half SBS, 2D  (L)";
    case PlayerCommand::SwapEyes: return L"Swap the left and right eye views  (E)";
    case PlayerCommand::IsoTitleMenu:
        return state.isoTitleMenuEnabled ? L"Choose the Blu-ray title  (I)" : L"Blu-ray titles not available";
    case PlayerCommand::Minimize: return L"Minimize the window";
    case PlayerCommand::Fullscreen:
        return state.fullscreen ? L"Leave fullscreen  (F, Esc)" : L"Go fullscreen  (F, Alt+Enter, double-click)";
    case PlayerCommand::Close: return L"Quit Odyssey  (Ctrl+Q)";
    default: return L"";
    }
}

COLORREF color(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return RGB(r, g, b);
}

void fill(HDC dc, const Rect& rect, COLORREF value) {
    RECT native{rect.left, rect.top, rect.right, rect.bottom};
    SetDCBrushColor(dc, value);
    FillRect(dc, &native, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void frame(HDC dc, const Rect& rect, COLORREF upper, COLORREF lower, int thickness = 1) {
    fill(dc, {rect.left, rect.top, rect.right, rect.top + thickness}, upper);
    fill(dc, {rect.left, rect.top, rect.left + thickness, rect.bottom}, upper);
    fill(dc, {rect.left, rect.bottom - thickness, rect.right, rect.bottom}, lower);
    fill(dc, {rect.right - thickness, rect.top, rect.right, rect.bottom}, lower);
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
    resources.font = pixelFont(11, surface.scale, true);
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

    const COLORREF charcoal = color(8, 10, 8);
    const COLORREF panel = color(30, 34, 30);
    const COLORREF raised = color(56, 62, 56);
    const COLORREF hover = color(84, 92, 84);
    const COLORREF light = color(168, 176, 168);
    const COLORREF shadow = color(0, 0, 0);
    const COLORREF amber = color(255, 204, 64);
    const COLORREF green = color(128, 240, 128);
    // Warm off-white on near-black ~= 16:1 contrast, avoids pure white glare.
    const COLORREF normalText = color(246, 244, 226);
    const COLORREF dimText = color(178, 184, 176);

    fill(dc, logical(0, 0, logicalWidth, logicalHeight), charcoal);
    fill(dc, logical(3, 3, logicalWidth - 3, logicalHeight - 3), panel);
    frame(dc, logical(1, 1, logicalWidth - 1, logicalHeight - 1), light, shadow, surface.scale);
    text(dc, L"Odyssey 3D", logical(12, 4, 120, 22), amber);

    {
        HFONT hintFont = pixelFont(9, surface.scale, false);
        HGDIOBJ previousFont = hintFont ? SelectObject(dc, hintFont) : nullptr;
        const COLORREF hintColor = (pointerInside_ || drag_ != Drag::None) ? normalText : dimText;
        text(dc, hoverHint(state, clientWidth, clientHeight), logical(126, 4, 752, 22), hintColor);
        if (previousFont) SelectObject(dc, previousFont);
        if (hintFont) DeleteObject(hintFont);
    }

    const auto drawButton = [&](PlayerCommand command, const std::wstring& label, bool enabled = true) {
        const Rect absolute = controlRect(command, geometry);
        const Rect rect = local(absolute);
        const bool hovered = pointerInside_ && absolute.contains(pointerX_, pointerY_);
        const bool pressed = pressed_ == command;
        fill(dc, rect, pressed ? charcoal : (hovered ? hover : raised));
        frame(dc, rect, pressed ? shadow : light, pressed ? light : shadow, surface.scale);
        if (hovered && !pressed) {
            frame(dc, rect, amber, amber, surface.scale);
        }
        text(dc, label, {rect.left + 4 * surface.scale, rect.top,
                         rect.right - 4 * surface.scale, rect.bottom},
             enabled ? normalText : dimText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    };

    drawButton(PlayerCommand::Minimize, L"Min");
    drawButton(PlayerCommand::Fullscreen, state.fullscreen ? L"Exit" : L"Full");
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

    const auto drawSlider = [&](const Rect& track, double fraction, COLORREF fillColor) {
        fill(dc, track, shadow);
        frame(dc, track, shadow, light, surface.scale);
        const int width = track.right - track.left;
        const int knobMin = track.left + 3 * surface.scale;
        const int knobMax = track.right - 3 * surface.scale;
        const int knobX = (std::clamp)(track.left + static_cast<int>(width * fraction), knobMin, knobMax);
        fill(dc, {track.left + 2 * surface.scale, track.top + 4 * surface.scale,
                  knobX, track.bottom - 4 * surface.scale}, fillColor);
        fill(dc, {knobX - 3 * surface.scale, track.top - surface.scale,
                  knobX + 3 * surface.scale, track.bottom + surface.scale}, shadow);
        fill(dc, {knobX - 2 * surface.scale, track.top,
                  knobX + 2 * surface.scale, track.bottom}, fillColor);
    };

    const Rect seek = local(geometry.seek);
    const double seekFraction = duration > 0.0
        ? safeUnitValue(previewPosition / duration) : 0.0;
    drawSlider(seek, seekFraction, amber);

    drawButton(PlayerCommand::Open, L"Open");
    drawButton(PlayerCommand::PlayPause, state.playing ? L"Pause" : L"Play");
    drawButton(PlayerCommand::Stop, L"Stop");
    drawButton(PlayerCommand::ToggleMute, state.muted ? L"Unmute" : L"Mute");

    const Rect volume = local(geometry.volume);
    text(dc, L"Vol", logical(656, 78, 696, 98), dimText,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    const double volumeFraction = safeUnitValue(state.volume);
    drawSlider(volume, volumeFraction, state.muted ? dimText : green);

    drawButton(PlayerCommand::AudioMenu,
               L"Audio: " + (state.audioLabel.empty() ? std::wstring(L"Default") : state.audioLabel),
               state.audioMenuEnabled);
    drawButton(PlayerCommand::CaptionsMenu,
               L"Captions: " + (state.captionsLabel.empty() ? std::wstring(L"Off") : state.captionsLabel),
               state.captionsMenuEnabled);
    drawButton(PlayerCommand::LayoutMenu,
               state.layoutLabel.empty() ? L"Layout" : state.layoutLabel);
    drawButton(PlayerCommand::SwapEyes, L"Swap Eyes");
    if (state.isIso) {
        drawButton(PlayerCommand::IsoTitleMenu, L"ISO Title", state.isoTitleMenuEnabled);
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
    if (!state.playing || state.forceVisible || state.menuOpen || drag_ != Drag::None ||
        pressed_ != PlayerCommand::None ||
        !state.error.empty()) {
        return true;
    }
    if (nowMilliseconds < revealUntilMilliseconds_) return true;
    if (!pointerInside_) return false;
    return nowMilliseconds < lastMotionMilliseconds_ ||
           nowMilliseconds - lastMotionMilliseconds_ < hideDelayMilliseconds;
}

double PlayerControls::seekPreviewSeconds(const PlayerControlState& state) const noexcept {
    if (drag_ != Drag::Seek) return safeSeconds(state.positionSeconds);
    return dragValue_ * safeSeconds(state.durationSeconds);
}

void PlayerControls::keyboardActivity(std::uint64_t nowMilliseconds) noexcept {
    revealUntilMilliseconds_ = nowMilliseconds + hideDelayMilliseconds;
}

bool PlayerControls::panelContains(int x, int y, int clientWidth, int clientHeight) const noexcept {
    return validClient(clientWidth, clientHeight) && geometryFor(clientWidth, clientHeight).panel.contains(x, y);
}

std::wstring PlayerControls::hoverHint(const PlayerControlState& state,
                                       int clientWidth, int clientHeight) const {
    static const std::wstring idleText =
        L"Space play/pause   Left/Right seek   Up/Down volume   M mute   F fullscreen";
    if (drag_ == Drag::None && (!pointerInside_ || !validClient(clientWidth, clientHeight))) {
        return idleText;
    }
    const Geometry geometry = geometryFor(clientWidth, clientHeight);
    const double duration = safeSeconds(state.durationSeconds);

    if (drag_ == Drag::Seek || geometry.seek.contains(pointerX_, pointerY_)) {
        if (duration <= 0.0) return L"Nothing to seek yet";
        const double seconds = drag_ == Drag::Seek
            ? dragValue_ * duration
            : fractionAt(pointerX_, geometry.seek) * duration;
        return L"Seek to " + timeText(seconds) +
               L"  (Left/Right 5s, Shift 30s, Ctrl 60s, Home)";
    }
    if (drag_ == Drag::Volume || geometry.volume.contains(pointerX_, pointerY_)) {
        const double fraction = drag_ == Drag::Volume ? dragValue_ : safeUnitValue(state.volume);
        wchar_t percent[16]{};
        swprintf_s(percent, L"Volume %d%%", static_cast<int>((std::lround)(fraction * 100.0)));
        return std::wstring(percent) + L"  (Up/Down, mouse wheel)";
    }

    constexpr PlayerCommand order[] = {
        PlayerCommand::Open, PlayerCommand::PlayPause, PlayerCommand::Stop,
        PlayerCommand::ToggleMute, PlayerCommand::AudioMenu, PlayerCommand::CaptionsMenu,
        PlayerCommand::LayoutMenu, PlayerCommand::SwapEyes, PlayerCommand::Minimize,
        PlayerCommand::Fullscreen, PlayerCommand::Close, PlayerCommand::IsoTitleMenu,
    };
    PlayerCommand hovered = PlayerCommand::None;
    if (pressed_ != PlayerCommand::None) {
        hovered = pressed_;
    } else {
        for (const PlayerCommand command : order) {
            if (command == PlayerCommand::IsoTitleMenu && !state.isIso) continue;
            if (controlRect(command, geometry).contains(pointerX_, pointerY_)) {
                hovered = command;
                break;
            }
        }
    }
    if (hovered == PlayerCommand::None) return idleText;
    return commandHint(hovered, state);
}

PlayerAction PlayerControls::keyAction(unsigned virtualKey, bool ctrl, bool shift,
                                       const PlayerControlState& state) noexcept {
    const double duration = safeSeconds(state.durationSeconds);
    const double position = safeSeconds(state.positionSeconds);
    const double step = ctrl ? 60.0 : (shift ? 30.0 : 5.0);
    const auto seekBy = [&](double delta) -> PlayerAction {
        if (duration <= 0.0) return {};
        return {PlayerCommand::Seek, (std::clamp)(position + delta, 0.0, duration)};
    };
    const auto volumeBy = [&](double delta) -> PlayerAction {
        return {PlayerCommand::SetVolume, (std::clamp)(safeUnitValue(state.volume) + delta, 0.0, 1.0)};
    };
    switch (virtualKey) {
    case VK_SPACE:
    case 'K':
    case VK_MEDIA_PLAY_PAUSE:
        return {PlayerCommand::PlayPause, 0.0};
    case 'S':
    case VK_MEDIA_STOP:
        return {PlayerCommand::Stop, 0.0};
    case 'M':
        return {PlayerCommand::ToggleMute, 0.0};
    case VK_LEFT:
        return seekBy(-step);
    case VK_RIGHT:
        return seekBy(step);
    case VK_HOME:
        return seekBy(-position);
    case VK_UP:
        return volumeBy(0.05);
    case VK_DOWN:
        return volumeBy(-0.05);
    case 'F':
    case VK_F11:
        return {PlayerCommand::Fullscreen, 0.0};
    case VK_ESCAPE:
        return state.fullscreen ? PlayerAction{PlayerCommand::Fullscreen, 0.0}
                                 : PlayerAction{PlayerCommand::Close, 0.0};
    case 'Q':
        return ctrl ? PlayerAction{PlayerCommand::Close, 0.0} : PlayerAction{};
    case 'O':
        return {PlayerCommand::Open, 0.0};
    case 'A':
        return state.audioMenuEnabled ? PlayerAction{PlayerCommand::AudioMenu, 0.0} : PlayerAction{};
    case 'C':
        return state.captionsMenuEnabled ? PlayerAction{PlayerCommand::CaptionsMenu, 0.0} : PlayerAction{};
    case 'L':
        return {PlayerCommand::LayoutMenu, 0.0};
    case 'E':
        return {PlayerCommand::SwapEyes, 0.0};
    case 'I':
        return (state.isIso && state.isoTitleMenuEnabled)
                   ? PlayerAction{PlayerCommand::IsoTitleMenu, 0.0}
                   : PlayerAction{};
    default:
        return {};
    }
}

} // namespace odyssey
