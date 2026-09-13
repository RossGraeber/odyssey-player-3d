#include "app/ImmersityRuntime.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <string>

namespace {

class EnvironmentRestore {
public:
    explicit EnvironmentRestore(const wchar_t* name) : m_name(name) {
        SetLastError(ERROR_SUCCESS);
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        m_existed = required > 0 || GetLastError() == ERROR_SUCCESS;
        if (required > 0) {
            m_value.resize(required, L'\0');
            const DWORD written =
                GetEnvironmentVariableW(name, m_value.data(), required);
            m_value.resize(written);
        }
    }

    ~EnvironmentRestore() {
        SetEnvironmentVariableW(m_name.c_str(), m_existed ? m_value.c_str() : nullptr);
    }

private:
    std::wstring m_name;
    std::wstring m_value;
    bool m_existed{false};
};

TEST(ImmersityRuntime, MissingAuthoritativeDirectoryLeavesRuntimeUnavailable) {
    wchar_t tempDirectory[MAX_PATH]{};
    const DWORD tempLength = GetTempPathW(MAX_PATH, tempDirectory);
    ASSERT_GT(tempLength, 0u);
    ASSERT_LT(tempLength, static_cast<DWORD>(MAX_PATH));
    const std::wstring missingDirectory = std::wstring(tempDirectory)
        + L"odyssey-missing-immersity-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(GetTickCount64());
    ASSERT_EQ(GetFileAttributesW(missingDirectory.c_str()), INVALID_FILE_ATTRIBUTES);

    EnvironmentRestore restore(L"ODYSSEY_IMMERSITY_RUNTIME_DIR");
    ASSERT_TRUE(SetEnvironmentVariableW(
        L"ODYSSEY_IMMERSITY_RUNTIME_DIR", missingDirectory.c_str()));

    EXPECT_FALSE(odyssey::initializeImmersityRuntime());
    EXPECT_FALSE(odyssey::immersityRuntimeAvailable());
}

} // namespace
