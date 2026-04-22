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
        }
    }
    LocalFree(argv);
    return out;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const ParsedArgs args = parseArgs();
    const bool headless = args.smokeTest || args.spikeSmoke;

    try {
        odyssey::AppShell app;
        if (args.smokeTest)   return app.runSmokeTest();
        if (args.spikeSmoke)  return app.runSpikeSmokeTest(args.path);
        if (args.spike)       return app.runSpike(args.path);
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
