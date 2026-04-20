#include <windows.h>
#include <exception>
#include <wchar.h>

#include "app/AppShell.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
    const bool smoke = lpCmdLine && wcsstr(lpCmdLine, L"--smoke-test") != nullptr;
    try {
        odyssey::AppShell app;
        return smoke ? app.runSmokeTest() : app.run();
    } catch (const std::exception& e) {
        if (smoke) {
            OutputDebugStringA("[odyssey smoke] fatal: ");
            OutputDebugStringA(e.what());
            OutputDebugStringA("\n");
            return 2;
        }
        MessageBoxA(nullptr, e.what(), "Odyssey Player 3D - fatal", MB_ICONERROR | MB_OK);
        return 1;
    }
}
