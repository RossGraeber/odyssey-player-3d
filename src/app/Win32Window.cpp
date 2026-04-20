#include "Win32Window.h"

namespace odyssey {

static const wchar_t* kClassName = L"OdysseyPlayer3DWindow";

Win32Window::Win32Window(const wchar_t* title, int clientWidth, int clientHeight, Callbacks cb)
    : m_cb(std::move(cb))
{
    m_hinstance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &Win32Window::s_wndProc;
    wc.hInstance     = m_hinstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    RECT r{0, 0, clientWidth, clientHeight};
    const DWORD style   = WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = 0;
    AdjustWindowRectEx(&r, style, FALSE, exStyle);

    m_hwnd = CreateWindowExW(
        exStyle, kClassName, title, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, m_hinstance, this);

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
}

Win32Window::~Win32Window() {
    if (m_hwnd) DestroyWindow(m_hwnd);
    UnregisterClassW(kClassName, m_hinstance);
}

bool Win32Window::pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

// We use a window-style swap to WS_POPUP + monitor rect rather than DXGI
// SetFullscreenState. Borderless fullscreen keeps Alt-Tab fast, avoids mode
// changes, and sidesteps exclusive-mode ownership issues the weaver presents
// later. Cost: no exclusive-mode latency win, acceptable for a media player.
void Win32Window::toggleBorderlessFullscreen() {
    if (!m_fullscreen) {
        m_savedStyle   = (DWORD)GetWindowLongPtrW(m_hwnd, GWL_STYLE);
        m_savedExStyle = (DWORD)GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
        GetWindowRect(m_hwnd, &m_savedRect);

        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);

        SetWindowLongPtrW(m_hwnd, GWL_STYLE,   (m_savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, m_savedExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(m_hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right  - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        m_fullscreen = true;
    } else {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE,   m_savedStyle);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, m_savedExStyle);
        SetWindowPos(m_hwnd, nullptr,
            m_savedRect.left, m_savedRect.top,
            m_savedRect.right  - m_savedRect.left,
            m_savedRect.bottom - m_savedRect.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOZORDER);
        m_fullscreen = false;
    }
}

LRESULT CALLBACK Win32Window::s_wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->wndProc(h, m, wp, lp);
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT Win32Window::wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE: {
        UINT w = LOWORD(lp), ht = HIWORD(lp);
        if (m_cb.onResize && w > 0 && ht > 0) m_cb.onResize(w, ht);
        return 0;
    }
    case WM_SYSKEYDOWN:
        // Alt-Enter
        if (wp == VK_RETURN && (HIWORD(lp) & KF_ALTDOWN)) {
            if (m_cb.onToggleFullscreen) m_cb.onToggleFullscreen();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (m_cb.onQuit) m_cb.onQuit();
            return 0;
        }
        break;
    case WM_CLOSE:
        if (m_cb.onQuit) m_cb.onQuit();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

} // namespace odyssey
