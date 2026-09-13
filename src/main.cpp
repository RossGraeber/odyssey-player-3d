#include <windows.h>
#include <shellapi.h>
#include <exception>
#include <cmath>
#include <string>
#include <utility>
#include <wchar.h>

#include "app/AppShell.h"
#include "app/ImmersityRuntime.h"

namespace {

struct ParsedArgs {
    bool smokeTest{false};
    bool spike{false};
    bool spikeSmoke{false};
    bool play{false};
    bool playSmoke{false};
    bool playerSmoke{false};
    double playerSmokeSeconds{3.0};
    std::wstring path;
};

// Minimal arg parse — CommandLineToArgvW gives us a real argv from the Unicode
// command line, which keeps us sane vs the lpCmdLine string parsing dance.
static ParsedArgs parseArgs() {
    ParsedArgs out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return out;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--smoke-test") {
            out.smokeTest = true;
        } else if (a == L"--spike" && i + 1 < argc) {
            out.spike = true;
            out.path = argv[++i];
        } else if (a == L"--spike-smoke" && i + 1 < argc) {
            out.spikeSmoke = true;
            out.path = argv[++i];
        } else if (a == L"--play" && i + 1 < argc) {
            out.play = true;
            out.path = argv[++i];
        } else if (a == L"--play-smoke") {
            out.playSmoke = true;
            // Optional path arg; if absent, fall through to env-var lookup
            // in the smoke test itself.
            if (i + 1 < argc) out.path = argv[++i];
        } else if (a == L"--player-smoke" && i + 1 < argc) {
            out.playerSmoke = true;
            out.path = argv[++i];
            if (i + 1 < argc) {
                wchar_t* end = nullptr;
                const double parsed = wcstod(argv[i + 1], &end);
                if (end && *end == L'\0' && std::isfinite(parsed)) {
                    out.playerSmokeSeconds = parsed;
                    ++i;
                }
            }
        } else if (!a.empty() && a.front() != L'-') {
            out.play = true;
            out.path = std::move(a);
        }
    }
    LocalFree(argv);
    return out;
}

// Reads %ODYSSEY_TEST_M2_FSBS_MKV%. Empty string when unset.
static std::wstring envVideoPath() {
    wchar_t buf[1024]{};
    DWORD n = GetEnvironmentVariableW(L"ODYSSEY_TEST_M2_FSBS_MKV", buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf)) return {};
    return std::wstring(buf, n);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComLifetime {
        bool active;
        ~ComLifetime() { if (active) CoUninitialize(); }
    } com{SUCCEEDED(comResult)};
    if (FAILED(comResult)) {
        MessageBoxW(nullptr, L"Windows initialization failed", L"Odyssey Player 3D",
                    MB_ICONERROR | MB_OK);
        return 1;
    }
    odyssey::initializeImmersityRuntime();

    ParsedArgs args = parseArgs();
    const bool headless = args.smokeTest || args.spikeSmoke || args.playSmoke || args.playerSmoke;

    if (args.playSmoke && args.path.empty()) args.path = envVideoPath();

    try {
        odyssey::AppShell app;
        if (args.smokeTest)   return app.runSmokeTest();
        if (args.spikeSmoke)  return app.runSpikeSmokeTest(args.path);
        if (args.spike)       return app.runSpike(args.path);
        if (args.playSmoke)   return app.runPlaySmokeTest(args.path);
        if (args.playerSmoke) return app.runPlayerSmoke(args.path, args.playerSmokeSeconds);
        if (args.play)        return app.runPlay(args.path);
        return app.run();
    } catch (const std::exception& e) {
        if (headless) {
            OutputDebugStringA("[odyssey] fatal: ");
            OutputDebugStringA(e.what());
            OutputDebugStringA("\n");
            return 2;
        }
        MessageBoxA(nullptr, e.what(), "Odyssey Player 3D - fatal", MB_ICONERROR | MB_OK);
        return 1;
    }
}
