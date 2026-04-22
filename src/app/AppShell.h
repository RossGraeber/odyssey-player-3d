#pragma once

#include <memory>
#include <string>
#include "Win32Window.h"
#include "D3D11Device.h"
#include "ImmersityWeaver.h"

namespace odyssey {

class AppShell {
public:
    AppShell();
    ~AppShell();

    int run();
    int runSmokeTest();

    // M1: load a SBS PNG and weave it until Esc. Returns 0 on clean exit.
    int runSpike(const std::wstring& pngPath);

    // Headless 30-frame variant of runSpike used by CTest. Exit codes:
    //   0  — ran 30 frames clean
    //   77 — SR service unavailable (CTest interprets as SKIPPED)
    //   other non-zero — real failure (PNG load, D3D error, unexpected throw)
    int runSpikeSmokeTest(const std::wstring& pngPath);

private:
    std::unique_ptr<Win32Window> m_window;
    std::unique_ptr<D3D11Device> m_device;
    bool m_running{true};
    unsigned m_resizeCount{0};
};

} // namespace odyssey
