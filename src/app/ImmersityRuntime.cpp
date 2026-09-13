#include "ImmersityRuntime.h"

#include <windows.h>
#include <shlobj_core.h>

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace odyssey {
namespace {

class RuntimeState {
public:
    ~RuntimeState() {
        for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
            FreeLibrary(*it);
        }
    }

    std::once_flag once;
    bool available{false};
    std::vector<HMODULE> modules;
};

RuntimeState& runtimeState() {
    static RuntimeState state;
    return state;
}

struct CoTaskMemDeleter {
    void operator()(wchar_t* value) const noexcept {
        CoTaskMemFree(value);
    }
};

class PendingModules {
public:
    explicit PendingModules(std::size_t capacity) {
        m_modules.reserve(capacity);
    }

    ~PendingModules() {
        for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it) {
            FreeLibrary(*it);
        }
    }

    bool load(const std::wstring& path) {
        HMODULE module = LoadLibraryExW(
            path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            return false;
        }
        try {
            m_modules.push_back(module);
        } catch (...) {
            FreeLibrary(module);
            throw;
        }
        return true;
    }

    std::vector<HMODULE> release() noexcept {
        return std::move(m_modules);
    }

private:
    std::vector<HMODULE> m_modules;
};

std::wstring runtimeDirectory() {
    SetLastError(ERROR_SUCCESS);
    const DWORD required =
        GetEnvironmentVariableW(L"ODYSSEY_IMMERSITY_RUNTIME_DIR", nullptr, 0);
    if (required > 0) {
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(
            L"ODYSSEY_IMMERSITY_RUNTIME_DIR", value.data(), required);
        if (written == 0 || written >= required) {
            return {};
        }
        value.resize(written);
        return value;
    }
    if (GetLastError() == ERROR_SUCCESS) {
        return {};
    }

    PWSTR programFiles = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr,
                                    &programFiles))) {
        return {};
    }
    const std::unique_ptr<wchar_t, CoTaskMemDeleter> ownedProgramFiles(programFiles);
    std::wstring directory(ownedProgramFiles.get());
    directory.append(L"\\LeiaSR\\Platform\\bin");
    return directory;
}

std::wstring modulePath(const std::wstring& directory, const wchar_t* name) {
    std::wstring path = directory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path.append(name);
    return path;
}

void initialize(RuntimeState& state) noexcept {
    try {
        const std::wstring directory = runtimeDirectory();
        if (directory.empty()) {
            return;
        }
        constexpr std::array<const wchar_t*, 4> moduleNames{
            L"opencv_world343.dll",
            L"SimulatedRealityCore.dll",
            L"SimulatedRealityDisplays.dll",
            L"SimulatedRealityDirectX.dll",
        };

        PendingModules loaded(moduleNames.size());
        for (const wchar_t* name : moduleNames) {
            const std::wstring path = modulePath(directory, name);
            if (!loaded.load(path)) {
                return;
            }
        }
        state.modules = loaded.release();
        state.available = true;
    } catch (...) {
        state.available = false;
    }
}

} // namespace

bool initializeImmersityRuntime() noexcept {
    RuntimeState& state = runtimeState();
    std::call_once(state.once, [&state]() noexcept { initialize(state); });
    return state.available;
}

bool immersityRuntimeAvailable() noexcept {
    return runtimeState().available;
}

} // namespace odyssey
