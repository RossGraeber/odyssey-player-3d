#include "Win32Window.h"

#include "PlayerControls.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace odyssey {

static const wchar_t* kClassName = L"OdysseyPlayer3DWindow";

Win32Window::Win32Window(const wchar_t* title, int clientWidth, int clientHeight, Callbacks cb)
    : m_cb(std::move(cb))
{
    m_hinstance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = &Win32Window::s_wndProc;
    wc.hInstance     = m_hinstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    RECT r{0, 0, clientWidth, clientHeight};
    const DWORD style   = WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = 0;
    AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, GetDpiForSystem());

    m_hwnd = CreateWindowExW(
        exStyle, kClassName, title, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, m_hinstance, this);
    if (!m_hwnd) {
        const DWORD error = GetLastError();
        UnregisterClassW(kClassName, m_hinstance);
        throw std::runtime_error("CreateWindowExW failed (error=" +
                                 std::to_string(error) + ")");
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    DragAcceptFiles(m_hwnd, TRUE);
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
void Win32Window::toggleBorderlessFullscreen(HMONITOR targetMonitor, const RECT* targetRect) {
    if (!m_fullscreen) {
        m_savedStyle   = (DWORD)GetWindowLongPtrW(m_hwnd, GWL_STYLE);
        m_savedExStyle = (DWORD)GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
        GetWindowRect(m_hwnd, &m_savedRect);

        RECT fullscreenRect{};
        if (targetRect) {
            fullscreenRect = *targetRect;
        } else {
            HMONITOR monitor = targetMonitor
                ? targetMonitor : MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info)) return;
            fullscreenRect = info.rcMonitor;
        }
        if (fullscreenRect.right <= fullscreenRect.left ||
            fullscreenRect.bottom <= fullscreenRect.top) return;
        m_fullscreenRect = fullscreenRect;
        m_fullscreen = true;

        SetWindowLongPtrW(m_hwnd, GWL_STYLE,   (m_savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, m_savedExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(m_hwnd, HWND_TOP,
            fullscreenRect.left, fullscreenRect.top,
            fullscreenRect.right - fullscreenRect.left,
            fullscreenRect.bottom - fullscreenRect.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        m_fullscreen = false;
        SetWindowLongPtrW(m_hwnd, GWL_STYLE,   m_savedStyle);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, m_savedExStyle);
        SetWindowPos(m_hwnd, nullptr,
            m_savedRect.left, m_savedRect.top,
            m_savedRect.right  - m_savedRect.left,
            m_savedRect.bottom - m_savedRect.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOZORDER);
    }
}

void Win32Window::setMouseCapture(bool enabled) {
    if (enabled) {
        if (GetCapture() != m_hwnd) SetCapture(m_hwnd);
    } else if (GetCapture() == m_hwnd) {
        m_suppressCaptureLost = true;
        ReleaseCapture();
        m_suppressCaptureLost = false;
    }
}

bool Win32Window::hasMouseCapture() const noexcept {
    return GetCapture() == m_hwnd;
}

void Win32Window::setCursorVisible(bool visible) noexcept {
    m_cursorVisible = visible;
    if (visible) SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

void Win32Window::minimize() noexcept {
    ShowWindow(m_hwnd, SW_MINIMIZE);
}

UINT Win32Window::clientWidth() const noexcept {
    RECT rect{};
    return GetClientRect(m_hwnd, &rect)
        ? static_cast<UINT>((std::max)(0L, rect.right - rect.left)) : 0;
}

UINT Win32Window::clientHeight() const noexcept {
    RECT rect{};
    return GetClientRect(m_hwnd, &rect)
        ? static_cast<UINT>((std::max)(0L, rect.bottom - rect.top)) : 0;
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
    case WM_GETMINMAXINFO: {
        auto* minMax = reinterpret_cast<MINMAXINFO*>(lp);
        RECT minimum{0, 0, PlayerControls::minimumClientWidth,
                     PlayerControls::minimumClientHeight};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(h, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(h, GWL_EXSTYLE));
        AdjustWindowRectExForDpi(&minimum, style, FALSE, exStyle, GetDpiForWindow(h));
        minMax->ptMinTrackSize.x = minimum.right - minimum.left;
        minMax->ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* target = m_fullscreen
            ? &m_fullscreenRect : reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, target->left, target->top,
                     target->right - target->left,
                     target->bottom - target->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_SIZE: {
        UINT w = LOWORD(lp), ht = HIWORD(lp);
        const bool minimized = wp == SIZE_MINIMIZED;
        if (minimized != m_minimized) {
            m_minimized = minimized;
            if (m_cb.onMinimizedChanged) m_cb.onMinimizedChanged(minimized);
        }
        if (m_cb.onResize && w > 0 && ht > 0) m_cb.onResize(w, ht);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!m_trackingMouseLeave) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = h;
            m_trackingMouseLeave = TrackMouseEvent(&tracking) != FALSE;
        }
        if (m_cb.onMouseMove) m_cb.onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && !m_cursorVisible) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_MOUSELEAVE:
        m_trackingMouseLeave = false;
        if (m_cb.onMouseLeave) m_cb.onMouseLeave();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (m_cb.onMouseDown) {
            const MouseButton button = m == WM_LBUTTONDOWN ? MouseButton::Left
                : (m == WM_RBUTTONDOWN ? MouseButton::Right : MouseButton::Middle);
            m_cb.onMouseDown(button, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (m_cb.onMouseUp) {
            const MouseButton button = m == WM_LBUTTONUP ? MouseButton::Left
                : (m == WM_RBUTTONUP ? MouseButton::Right : MouseButton::Middle);
            m_cb.onMouseUp(button, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (m_cb.onMouseDoubleClick) m_cb.onMouseDoubleClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL: {
        m_wheelAccumulator += GET_WHEEL_DELTA_WPARAM(wp);
        const int notches = m_wheelAccumulator / WHEEL_DELTA;
        m_wheelAccumulator -= notches * WHEEL_DELTA;
        if (notches != 0 && m_cb.onMouseWheel) m_cb.onMouseWheel(notches);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (!m_suppressCaptureLost && reinterpret_cast<HWND>(lp) != h &&
            m_cb.onCaptureLost) {
            m_cb.onCaptureLost();
        }
        return 0;
    case WM_CANCELMODE:
        if (GetCapture() == h) {
            m_suppressCaptureLost = true;
            ReleaseCapture();
            m_suppressCaptureLost = false;
        }
        if (m_cb.onCaptureLost) m_cb.onCaptureLost();
        return 0;
    case WM_SETFOCUS:
        if (m_cb.onFocusChanged) m_cb.onFocusChanged(true);
        return 0;
    case WM_KILLFOCUS:
        if (m_cb.onFocusChanged) m_cb.onFocusChanged(false);
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        struct DropGuard {
            HDROP value;
            ~DropGuard() { DragFinish(value); }
        } guard{drop};
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        if (length > 0 && m_cb.onFileDropped) {
            std::vector<wchar_t> path(static_cast<size_t>(length) + 1);
            if (DragQueryFileW(drop, 0, path.data(), static_cast<UINT>(path.size())) > 0) {
                m_cb.onFileDropped(std::wstring(path.data(), length));
            }
        }
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
        if (m_cb.onKeyDown && m_cb.onKeyDown(static_cast<UINT>(wp),
                                              (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                                              (GetKeyState(VK_SHIFT) & 0x8000) != 0)) {
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
