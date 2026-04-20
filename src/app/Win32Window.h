#pragma once

#include <windows.h>
#include <functional>

namespace odyssey {

class Win32Window {
public:
    struct Callbacks {
        std::function<void(UINT width, UINT height)> onResize;
        std::function<void()> onToggleFullscreen;
        std::function<void()> onQuit;
    };

    Win32Window(const wchar_t* title, int clientWidth, int clientHeight, Callbacks cb);
    ~Win32Window();

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    HWND hwnd() const { return m_hwnd; }
    bool pumpMessages();          // false when WM_QUIT received
    void toggleBorderlessFullscreen();

private:
    static LRESULT CALLBACK s_wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT wndProc(HWND, UINT, WPARAM, LPARAM);

    HINSTANCE m_hinstance{nullptr};
    HWND      m_hwnd{nullptr};
    Callbacks m_cb;

    // Saved state for fullscreen restore.
    bool  m_fullscreen{false};
    DWORD m_savedStyle{0};
    DWORD m_savedExStyle{0};
    RECT  m_savedRect{};
};

} // namespace odyssey
