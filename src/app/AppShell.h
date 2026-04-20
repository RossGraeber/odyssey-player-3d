#pragma once

#include <memory>
#include "Win32Window.h"
#include "D3D11Device.h"

namespace odyssey {

class AppShell {
public:
    AppShell();
    ~AppShell();

    int run();
    int runSmokeTest();

private:
    std::unique_ptr<Win32Window> m_window;
    std::unique_ptr<D3D11Device> m_device;
    bool m_running{true};
    unsigned m_resizeCount{0};
};

} // namespace odyssey
