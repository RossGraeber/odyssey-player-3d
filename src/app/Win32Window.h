#pragma once

#include <windows.h>
#include <functional>
#include <string>

namespace odyssey {

class Win32Window {
public:
    enum class MouseButton { Left, Right, Middle };

    struct Callbacks {
        std::function<void(UINT width, UINT height)> onResize;
        std::function<void()> onToggleFullscreen;
        std::function<void()> onQuit;
        std::function<void(int x, int y)> onMouseMove;
        std::function<void()> onMouseLeave;
        std::function<void(MouseButton button, int x, int y)> onMouseDown;
        std::function<void(MouseButton button, int x, int y)> onMouseUp;
        std::function<void()> onCaptureLost;
        std::function<void(bool focused)> onFocusChanged;
        std::function<void(bool minimized)> onMinimizedChanged;
        std::function<void(std::wstring path)> onFileDropped;
    };

    Win32Window(const wchar_t* title, int clientWidth, int clientHeight, Callbacks cb);
    ~Win32Window();

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    HWND hwnd() const { return m_hwnd; }
    bool pumpMessages();          // false when WM_QUIT received
    void toggleBorderlessFullscreen(HMONITOR targetMonitor = nullptr,
                                    const RECT* targetRect = nullptr);
    void setMouseCapture(bool enabled);
    bool hasMouseCapture() const noexcept;
    bool isFullscreen() const noexcept { return m_fullscreen; }
    void setCursorVisible(bool visible) noexcept;
    void minimize() noexcept;
    void clearCallbacks() noexcept { m_cb = {}; }
    UINT clientWidth() const noexcept;
    UINT clientHeight() const noexcept;

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
    RECT  m_fullscreenRect{};
    bool  m_minimized{false};
    bool  m_trackingMouseLeave{false};
    bool  m_suppressCaptureLost{false};
    bool  m_cursorVisible{true};
};

} // namespace odyssey
