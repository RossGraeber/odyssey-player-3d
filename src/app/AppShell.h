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

    // M2: decode a full-SBS MKV via FFmpeg+D3D11VA, convert NV12 to RGBA,
    // weave to the panel. Returns 0 on clean EOF or Esc exit.
    int runPlay(const std::wstring& videoPath);

    // Headless variant: decode N frames, assert no sustained queue-full and
    // no D3D11 debug-layer errors, exit. Exit codes:
    //   0  — ran N frames clean
    //   77 — SR service unavailable OR video file missing
    //   other non-zero — real failure (decode, D3D, queue-full burst)
    int runPlaySmokeTest(const std::wstring& videoPath);

private:
    std::unique_ptr<Win32Window> m_window;
    std::unique_ptr<D3D11Device> m_device;
    bool m_running{true};
    unsigned m_resizeCount{0};
};

} // namespace odyssey
