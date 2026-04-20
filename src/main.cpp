#include <windows.h>
#include <exception>

#include "app/AppShell.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    try {
        odyssey::AppShell app;
        return app.run();
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Odyssey Player 3D - fatal", MB_ICONERROR | MB_OK);
        return 1;
    }
}
