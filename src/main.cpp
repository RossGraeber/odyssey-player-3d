#include <windows.h>
#include <shellapi.h>
#include <exception>
#include <string>
#include <wchar.h>

#include "app/AppShell.h"

namespace {

struct ParsedArgs {
    bool smokeTest{false};
    bool spike{false};
    bool spikeSmoke{false};
    bool play{false};
    bool playSmoke{false};
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
    ParsedArgs args = parseArgs();
    const bool headless = args.smokeTest || args.spikeSmoke || args.playSmoke;

    if (args.playSmoke && args.path.empty()) args.path = envVideoPath();

    try {
        odyssey::AppShell app;
        if (args.smokeTest)   return app.runSmokeTest();
        if (args.spikeSmoke)  return app.runSpikeSmokeTest(args.path);
        if (args.spike)       return app.runSpike(args.path);
        if (args.playSmoke)   return app.runPlaySmokeTest(args.path);
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
