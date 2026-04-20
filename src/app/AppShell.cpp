#include "AppShell.h"

#include <cmath>

namespace odyssey {

// sRGB (#0A0A0A) -> linear. 0x0A/255 = 0.03921568. Below 0.04045 the transfer
// is linear/12.92, so linearValue = 0.03921568 / 12.92 ~= 0.003035.
static constexpr float kClearLinear = 0.003035f;
static const float kClearColor[4] = { kClearLinear, kClearLinear, kClearLinear, 1.0f };

AppShell::AppShell() {
    Win32Window::Callbacks cb;
    cb.onResize = [this](UINT w, UINT h) {
        if (m_device) m_device->resize(w, h);
    };
    cb.onToggleFullscreen = [this]() {
        if (m_window) m_window->toggleBorderlessFullscreen();
    };
    cb.onQuit = [this]() {
        m_running = false;
        PostQuitMessage(0);
    };

    m_window = std::make_unique<Win32Window>(L"Odyssey Player 3D", 1280, 720, std::move(cb));
    m_device = std::make_unique<D3D11Device>(m_window->hwnd(), 1280u, 720u);
}

AppShell::~AppShell() = default;

int AppShell::run() {
    while (m_running) {
        if (!m_window->pumpMessages()) break;
        m_device->clearAndPresent(kClearColor);
    }
    return 0;
}

} // namespace odyssey
